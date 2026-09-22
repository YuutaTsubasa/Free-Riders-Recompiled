// The launcher's platform side on Linux and Android (SDL). On Linux the game
// is a program beside the launcher, started as a child process; on Android
// it is GameActivity in its own process (the runtime's settings are read
// while its library loads, so every run needs a fresh process), started
// through LauncherActivity, which also shows the system's document picker.
#include "launcher_platform.h"
#include "imgui.h"
#include <SDL.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#else
#include <sys/wait.h>
#endif

namespace sfr::launcher {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

#ifdef __ANDROID__
// ---------------------------------------------------------------- Android
// Why the game stopped, from the end of its log: 0 when the player closed
// it, 3 for another runtime stop, 1 when the log just ends (a crash).
uint32_t exit_code_from_log(const fs::path& log) {
    std::ifstream in(log, std::ios::binary | std::ios::ate);
    if (!in) return 1;
    const std::streamoff size = in.tellg();
    const std::streamoff start = std::max<std::streamoff>(0, size - 8192);
    in.seekg(start);
    std::string text(size_t(size - start), '\0');
    in.read(text.data(), std::streamsize(text.size()));
    const size_t stop = text.rfind("STOP ");
    if (stop == std::string::npos) return 1;
    return text.compare(stop, 18, "STOP window-closed") == 0 ? 0 : 3;
}

std::mutex picked_lock;
std::condition_variable picked_signal;
bool picked_ready = false;
std::string picked_path, picked_name;
std::map<std::string, std::string> source_names;  // /proc/self/fd/N -> document name

std::atomic<bool> game_launched{false}, launcher_back{false};

// Glyphs waiting for their pixels: an atlas custom rectangle each.
struct SystemGlyph {
    int rect;
    int width, height;
    std::vector<unsigned char> coverage;
};
std::vector<SystemGlyph> system_glyphs;

// Calls an argument-less void method of LauncherActivity.
void call_activity(const char* method) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass type = env->GetObjectClass(activity);
    if (jmethodID id = env->GetMethodID(type, method, "()V")) env->CallVoidMethod(activity, id);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

class AndroidGame final : public GameProcess {
public:
    explicit AndroidGame(fs::path log) : log_(std::move(log)) {}
    // Over once the launcher is back in front.
    std::optional<uint32_t> exit_code() override {
        if (!launcher_back.load()) return std::nullopt;
        game_launched = false;
        launcher_back = false;
        return exit_code_from_log(log_);
    }
private:
    fs::path log_;
};
#else
// ---------------------------------------------------------------- Linux
// The play defaults and the player's choices, in the game's environment.
void apply_environment(const LauncherSettings& settings, const fs::path& directory, bool checkout) {
    for (const auto& [name, value] : game_environment(settings)) {
        if (value.empty()) unsetenv(name.c_str());
        else setenv(name.c_str(), value.c_str(), 1);
    }
    setenv("SFR_SAVE_DIRECTORY", utf8(directory / "save").c_str(), 1);
    std::error_code error;
    if (!checkout && fs::is_regular_file(directory / "shaders.pack", error))
        setenv("SFR_SHADER_PACK", utf8(directory / "shaders.pack").c_str(), 1);
    // A release translates the shaders the pack lacks with its own tools.
    if (!checkout)
        for (const auto& [name, value] : shader_tool_environment(directory)) setenv(name.c_str(), utf8(value).c_str(), 1);
}

class ChildGame final : public GameProcess {
public:
    explicit ChildGame(pid_t pid) : pid_(pid) {}
    ~ChildGame() override {
        if (!done_) waitpid(pid_, nullptr, WNOHANG);
    }
    std::optional<uint32_t> exit_code() override {
        if (done_) return code_;
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) != pid_) return std::nullopt;
        done_ = true;
        code_ = WIFEXITED(status) ? uint32_t(WEXITSTATUS(status)) : 128u + uint32_t(WTERMSIG(status));
        return code_;
    }
private:
    pid_t pid_;
    bool done_ = false;
    uint32_t code_ = 0;
};

// The first line a command prints, or empty.
std::string first_line(const std::string& command) {
    std::string line;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[4096];
        if (std::fgets(buffer, sizeof buffer, pipe)) line = buffer;
        pclose(pipe);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

bool have(const char* program) { return !first_line(std::string("command -v ") + program).empty(); }

std::string shell_quoted(const std::string& text) {
    std::string quoted = "'";
    for (char c : text) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return quoted + "'";
}
#endif
}

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_com_freeriders_recompiled_LauncherActivity_nativeDocumentPicked(
        JNIEnv* env, jclass, jstring path, jstring name) {
    const auto text = [env](jstring value) {
        if (!value) return std::string();
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string copy = chars ? chars : "";
        if (chars) env->ReleaseStringUTFChars(value, chars);
        return copy;
    };
    std::lock_guard hold(picked_lock);
    picked_path = text(path);
    picked_name = text(name);
    picked_ready = true;
    picked_signal.notify_all();
}
#endif

#ifdef __ANDROID__
void add_system_glyphs(ImFont* font, float pixel_size, bool bold, const std::vector<unsigned int>& codepoints) {
    if (codepoints.empty()) return;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass type = env->GetObjectClass(activity);
    jmethodID render = env->GetMethodID(type, "renderGlyphs", "([IFZ)[B");
    jintArray points = env->NewIntArray(jsize(codepoints.size()));
    std::vector<jint> values(codepoints.begin(), codepoints.end());
    env->SetIntArrayRegion(points, 0, jsize(values.size()), values.data());
    auto result = render ? static_cast<jbyteArray>(env->CallObjectMethod(activity, render, points, jfloat(pixel_size), jboolean(bold)))
                         : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); result = nullptr; }
    std::vector<unsigned char> data;
    if (result) {
        data.resize(size_t(env->GetArrayLength(result)));
        env->GetByteArrayRegion(result, 0, jsize(data.size()), reinterpret_cast<jbyte*>(data.data()));
        env->DeleteLocalRef(result);
    }
    env->DeleteLocalRef(points);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    const auto read = [&](size_t at, auto& value) { std::memcpy(&value, data.data() + at, sizeof value); };
    size_t at = 0;
    for (unsigned int codepoint : codepoints) {
        if (at + 20 > data.size()) break;
        int32_t width = 0, height = 0;
        float advance = 0, x = 0, y = 0;
        read(at, width); read(at + 4, height); read(at + 8, advance); read(at + 12, x); read(at + 16, y);
        at += 20;
        if (width <= 0 || height <= 0 || at + size_t(width) * size_t(height) > data.size()) break;
        SystemGlyph glyph{ImGui::GetIO().Fonts->AddCustomRectFontGlyph(font, ImWchar(codepoint), width, height, advance, ImVec2(x, y)),
                          width, height, std::vector<unsigned char>(data.begin() + std::ptrdiff_t(at),
                                                                    data.begin() + std::ptrdiff_t(at + size_t(width) * size_t(height)))};
        at += size_t(width) * size_t(height);
        system_glyphs.push_back(std::move(glyph));
    }
}

void draw_system_glyphs() {
    ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
    if (!atlas.IsBuilt()) atlas.Build();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
    for (const SystemGlyph& glyph : system_glyphs) {
        const ImFontAtlasCustomRect* rect = atlas.GetCustomRectByIndex(glyph.rect);
        if (!rect || !rect->IsPacked()) continue;
        for (int y = 0; y < glyph.height; ++y)
            std::memcpy(pixels + size_t(rect->Y + y) * size_t(width) + rect->X, glyph.coverage.data() + size_t(y) * size_t(glyph.width),
                        size_t(glyph.width));
    }
    system_glyphs.clear();
}
#endif

void handle_event(const void* event) {
#ifdef __ANDROID__
    // Back in front after the game: its activity (and process) ended.
    if (static_cast<const SDL_Event*>(event)->type == SDL_APP_DIDENTERFOREGROUND && game_launched) launcher_back = true;
#else
    (void)event;
#endif
}

fs::path launcher_directory() {
#ifdef __ANDROID__
    const char* path = SDL_AndroidGetExternalStoragePath();
    return path ? fs::path(path) : fs::path(".");
#else
    char* base = SDL_GetBasePath();
    fs::path directory = base ? fs::path(base).parent_path() : fs::current_path();
    SDL_free(base);
    return directory;
#endif
}

bool can_pick_folders() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

bool copy_picked_file(const fs::path& from, const fs::path& to) {
    const std::string text = utf8(from);
    constexpr std::string_view passed = "/proc/self/fd/";
    int input = -1;
    if (text.rfind(passed, 0) == 0) {
        input = dup(std::atoi(text.c_str() + passed.size()));
        if (input >= 0) lseek(input, 0, SEEK_SET);
    } else {
        input = open(text.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (input < 0) return false;
    const fs::path partial = fs::path(to).concat(".partial");
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1 << 20);
    bool ok = bool(out);
    for (ssize_t got; ok && (got = read(input, buffer.data(), buffer.size())) != 0;) {
        if (got < 0) { if (errno == EINTR) continue; ok = false; break; }
        ok = bool(out.write(buffer.data(), got));
    }
    close(input);
    ok = ok && bool(out.flush());
    out.close();
    std::error_code error;
    if (ok) fs::rename(partial, to, error);
    if (!ok || error) fs::remove(partial, error);
    return ok && !error;
}

std::string source_name(const fs::path& source) {
#ifdef __ANDROID__
    if (const auto found = source_names.find(utf8(source)); found != source_names.end()) return found->second;
#endif
    return utf8(source);
}

std::optional<fs::path> pick_path(const fs::path& start, bool folders) {
    struct ReleaseKeys {
        ~ReleaseKeys() { ImGui::GetIO().ClearInputKeys(); }
    } release_keys;
#ifdef __ANDROID__
    (void)start;
    (void)folders;
    {
        std::lock_guard hold(picked_lock);
        picked_ready = false;
    }
    call_activity("pickDocument");
    std::unique_lock hold(picked_lock);
    picked_signal.wait(hold, [] { return picked_ready; });
    if (picked_path.empty()) return std::nullopt;
    source_names[picked_path] = picked_name.empty() ? picked_path : picked_name;
    return fs::path(picked_path);
#else
    // A desktop's own dialog when zenity or kdialog is there; otherwise the
    // path can be typed, dropped on the window or given on the command line.
    std::error_code error;
    const fs::path begin = fs::is_directory(start, error) ? start : start.parent_path();
    std::string command;
    if (have("zenity"))
        command = std::string("zenity --file-selection") + (folders ? " --directory" : "") +
                  " --filename=" + shell_quoted(utf8(begin) + "/") + " 2>/dev/null";
    else if (have("kdialog"))
        command = std::string("kdialog ") + (folders ? "--getexistingdirectory " : "--getopenfilename ") +
                  shell_quoted(utf8(begin)) + " 2>/dev/null";
    else
        return std::nullopt;
    const std::string chosen = first_line(command);
    if (chosen.empty()) return std::nullopt;
    return fs::path(std::u8string(chosen.begin(), chosen.end()));
#endif
}

fs::path game_program(const fs::path& directory) {
#ifdef __ANDROID__
    (void)directory;
    return {};
#else
    return directory / "sfr_cpu_diagnostic";
#endif
}

bool quit_with_game() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

std::unique_ptr<GameProcess> start_game(const LauncherSettings& settings, const fs::path& directory, const fs::path& log) {
#ifdef __ANDROID__
    // GameActivity reads settings.env before its libraries load.
    {
        std::ofstream env(directory / "settings.env", std::ios::binary | std::ios::trunc);
        if (!env) return nullptr;
        env << "# Written by the launcher at each start.\n";
        for (const auto& [name, value] : game_environment(settings)) env << name << '=' << value << '\n';
    }
    game_launched = true;
    launcher_back = false;
    call_activity("launchGame");
    return std::make_unique<AndroidGame>(log);
#else
    const fs::path root = find_runtime_root(directory);
    apply_environment(settings, directory, !root.empty());
    const fs::path program = game_program(directory);
    std::vector<std::string> arguments{utf8(program)};
    for (const auto& argument : game_arguments(settings)) arguments.push_back(utf8(fs::path(argument)));
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const std::string working = utf8(root.empty() ? directory : root), log_path = utf8(log);
    const pid_t pid = fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        // In the checkout when there is one: the game translates its shaders
        // with tools found relative to its working directory.
        if (chdir(working.c_str()) != 0) _exit(126);
        const int output = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output >= 0) {
            dup2(output, 1);
            dup2(output, 2);
            close(output);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    return std::make_unique<ChildGame>(pid);
#endif
}

bool can_open_log() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

void open_log(const fs::path& log) {
#ifndef __ANDROID__
    const std::string command = "xdg-open " + shell_quoted(utf8(log)) + " >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) return;
#else
    (void)log;
#endif
}

FontFiles font_files() {
#ifdef __ANDROID__
    const fs::path fonts = "/system/fonts";
    // Static faces: the variable Roboto-Regular.ttf of newer releases does
    // not load (ImGui's rasterizer reads only default outlines of static fonts).
    return {{fonts / "RobotoStatic-Regular.ttf", fonts / "Roboto-Regular.ttf"},
            {fonts / "RobotoStatic-Medium.ttf", fonts / "Roboto-Medium.ttf", fonts / "RobotoStatic-Regular.ttf"},
            {fonts / "Roboto-BlackItalic.ttf", fonts / "Roboto-Bold.ttf", fonts / "RobotoStatic-Regular.ttf"},
            {},  // Chinese: add_system_glyphs
            {}};
#else
    // fontconfig's choice first (only TrueType/OpenType files load), then
    // the usual Debian/Ubuntu paths.
    const auto match = [](const char* pattern) -> std::vector<fs::path> {
        const std::string file = have("fc-match") ? first_line(std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null") : "";
        const bool usable = file.size() > 4 && (file.ends_with(".ttf") || file.ends_with(".otf") || file.ends_with(".ttc"));
        return usable ? std::vector<fs::path>{fs::path(file)} : std::vector<fs::path>{};
    };
    const auto with = [](std::vector<fs::path> first, std::initializer_list<fs::path> rest) {
        first.insert(first.end(), rest);
        return first;
    };
    const fs::path dejavu = "/usr/share/fonts/truetype/dejavu", noto = "/usr/share/fonts/truetype/noto";
    const fs::path cjk = "/usr/share/fonts/opentype/noto", wqy = "/usr/share/fonts/truetype/wqy";
    return {with(match("sans"), {noto / "NotoSans-Regular.ttf", dejavu / "DejaVuSans.ttf"}),
            with(match("sans:semibold"), {noto / "NotoSans-SemiBold.ttf", dejavu / "DejaVuSans-Bold.ttf"}),
            with(match("sans:black:italic"), {noto / "NotoSans-BlackItalic.ttf", dejavu / "DejaVuSans-BoldOblique.ttf",
                                               dejavu / "DejaVuSans-Bold.ttf"}),
            with(match("sans:lang=zh-tw"), {cjk / "NotoSansCJK-Regular.ttc", wqy / "wqy-microhei.ttc"}),
            with(match("sans:bold:lang=zh-tw"), {cjk / "NotoSansCJK-Bold.ttc", cjk / "NotoSansCJK-Regular.ttc",
                                                  wqy / "wqy-microhei.ttc"})};
#endif
}

int ui_language() {
    int language = 0;
    if (SDL_Locale* locales = SDL_GetPreferredLocales()) {
        if (locales[0].language && std::string(locales[0].language) == "zh") language = 1;
        SDL_free(locales);
    }
    return language;
}
}
