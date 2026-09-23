#include "launcher_settings.h"
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace sfr {
namespace {
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

void read_number(const std::string& text, uint32_t low, uint32_t high, uint32_t& out) {
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc() && end == text.data() + text.size() && value >= low && value <= high) out = value;
}

void read_flag(const std::string& text, bool& out) {
    if (text == "1" || text == "true") out = true;
    else if (text == "0" || text == "false") out = false;
}

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
}

LauncherSettings parse_launcher_settings(const std::string& text) {
    LauncherSettings settings;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string key = trim(std::string_view(line).substr(0, equals));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key == "window_width") read_number(value, 160, 16384, settings.window_width);
        else if (key == "window_height") read_number(value, 160, 16384, settings.window_height);
        else if (key == "fullscreen") read_flag(value, settings.fullscreen);
        else if (key == "vsync") read_flag(value, settings.vsync);
        else if (key == "audio") read_flag(value, settings.audio);
        else if (key == "volume") read_number(value, 0, 100, settings.volume);
        else if (key == "skip_movies") read_flag(value, settings.skip_movies);
        else if (key == "vertex_cache") read_flag(value, settings.vertex_cache);
        else if (key == "gpu_pipeline") read_flag(value, settings.gpu_pipeline);
        else if (key == "parallel") read_flag(value, settings.parallel);
        else if (key == "race_render_every") read_number(value, 1, 4, settings.race_render_every);
        else if (key == "ui_sounds") read_flag(value, settings.ui_sounds);
        else if (key == "vulkan") read_flag(value, settings.vulkan);
        else if (key == "touch_controls") read_flag(value, settings.touch_controls);
        else if (key == "tilt") read_flag(value, settings.tilt);
        else if (key == "language" && (value == "auto" || value == "en" || value == "zh-TW")) settings.language = value;
        else if (key == "image_directory") settings.image_directory = utf8_path(value);
        else if (key == "asset_directory") settings.asset_directory = utf8_path(value);
    }
    return settings;
}

std::string format_launcher_settings(const LauncherSettings& s) {
    std::ostringstream out;
    out << "# Sonic Free Riders Recompiled launcher settings\n"
        << "window_width=" << s.window_width << '\n'
        << "window_height=" << s.window_height << '\n'
        << "fullscreen=" << s.fullscreen << '\n'
        << "vsync=" << s.vsync << '\n'
        << "audio=" << s.audio << '\n'
        << "volume=" << s.volume << '\n'
        << "skip_movies=" << s.skip_movies << '\n'
        << "vertex_cache=" << s.vertex_cache << '\n'
        << "gpu_pipeline=" << s.gpu_pipeline << '\n'
        << "parallel=" << s.parallel << '\n'
        << "race_render_every=" << s.race_render_every << '\n'
        << "ui_sounds=" << s.ui_sounds << '\n'
        << "vulkan=" << s.vulkan << '\n'
        << "touch_controls=" << s.touch_controls << '\n'
        << "tilt=" << s.tilt << '\n'
        << "language=" << s.language << '\n'
        << "image_directory=" << path_utf8(s.image_directory) << '\n'
        << "asset_directory=" << path_utf8(s.asset_directory) << '\n';
    return out.str();
}

LauncherSettings load_launcher_settings(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parse_launcher_settings(text.str());
}

bool save_launcher_settings(const std::filesystem::path& file, const LauncherSettings& settings) {
    // Written aside and moved over, so a failed write keeps the old file.
    auto temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << format_launcher_settings(settings);
        if (!out.flush()) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
}

std::vector<std::pair<std::string, std::string>> game_environment(const LauncherSettings& s) {
    return {
        // Playing: no diagnostic time or call limits, no traces.
        {"SFR_CALL_BUDGET", "18446744073709551615"},
        {"SFR_WATCHDOG_SECONDS", "31536000"},
        {"SFR_ALLOW_RENDER_TARGETS", "1"},
        {"SFR_TRACE_GRAPHICS", "0"},
        {"SFR_DIAGNOSTIC_ENTRIES", "0"},
        {"SFR_TRACE_IMPORTS", "0"},
        // A race steps a sixtieth of a second per frame.
        {"SFR_FRAME_LIMIT", "60"},
        {"SFR_RENDER_EVERY", std::to_string(s.race_render_every)},
        {"SFR_PARALLEL_WORKER", s.parallel ? "cores" : "0"},
        {"SFR_VERTEX_CACHE", s.vertex_cache ? "1" : "0"},
        {"SFR_GPU_PIPELINE", s.gpu_pipeline ? "1" : "0"},
        {"SFR_AUDIO", s.audio ? "1" : "0"},
        // The player is signed in, so the game keeps records (docs/saves.md).
        {"SFR_PROFILE", "1"},
        {"SFR_VOLUME", std::to_string(s.volume)},
        {"SFR_SKIP_MOVIES", s.skip_movies ? "1" : ""},
        {"SFR_WINDOW_WIDTH", std::to_string(s.window_width)},
        {"SFR_WINDOW_HEIGHT", std::to_string(s.window_height)},
        {"SFR_FULLSCREEN", s.fullscreen ? "1" : "0"},
        {"SFR_VSYNC", s.vsync ? "1" : "0"},
        {"SFR_GRAPHICS", s.vulkan ? "vulkan" : ""},
#ifdef __ANDROID__
        {"SFR_TOUCH_CONTROLS", s.touch_controls ? "1" : "0"},
        {"SFR_TILT", s.tilt ? "1" : "0"},
#endif
    };
}

std::vector<std::wstring> game_arguments(const LauncherSettings& s) {
    return {s.image_directory.wstring(), s.asset_directory.wstring(), L"--game-region=ntsc-us"};
}

bool is_image_directory(const std::filesystem::path& directory) {
    std::error_code error;
    return !directory.empty() && std::filesystem::is_regular_file(directory / "complete.txt", error) &&
           std::filesystem::is_regular_file(directory / "image.bin", error);
}

bool is_asset_directory(const std::filesystem::path& directory) {
    std::error_code error;
    return !directory.empty() && std::filesystem::is_regular_file(directory / "default.xex", error);
}

namespace {
std::filesystem::path search(const std::filesystem::path& launcher_directory, const char* installed,
                             const char* checkout, bool (*accepts)(const std::filesystem::path&)) {
    if (const auto beside = launcher_directory / installed; accepts(beside)) return beside;
    auto directory = launcher_directory;
    for (int level = 0; level < 5 && !directory.empty(); ++level) {
        if (const auto candidate = directory / checkout; accepts(candidate)) return candidate;
        if (directory == directory.parent_path()) break;
        directory = directory.parent_path();
    }
    return launcher_directory / installed;
}
}

std::filesystem::path default_image_directory(const std::filesystem::path& launcher_directory) {
    return search(launcher_directory, "game/image", "out/recomp/image-loader", is_image_directory);
}

std::filesystem::path default_asset_directory(const std::filesystem::path& launcher_directory) {
    return search(launcher_directory, "game/assets", "private/assets", is_asset_directory);
}

std::filesystem::path find_runtime_root(const std::filesystem::path& launcher_directory) {
    std::error_code error;
    auto directory = launcher_directory;
    for (int level = 0; level < 6 && !directory.empty(); ++level) {
        if (std::filesystem::is_regular_file(directory / "tools/XenosRecomp/XenosRecomp/shader_common.h", error) &&
            std::filesystem::is_regular_file(directory / "out/tools/shader-translator/shader_translate.exe", error))
            return directory;
        if (directory == directory.parent_path()) break;
        directory = directory.parent_path();
    }
    return {};
}

std::vector<std::pair<std::string, std::filesystem::path>> shader_tool_environment(const std::filesystem::path& launcher_directory) {
    const auto tools = launcher_directory / "shader-tools";
#ifdef _WIN32
    const char* translator = "shader_translate.exe";
    const char* dxc = "dxc.exe";
    const char* libraries[] = {"dxcompiler.dll", "dxil.dll"};
#else
    const char* translator = "shader_translate";
    const char* dxc = "dxc-linux";
    const char* libraries[] = {"libdxcompiler.so", "libdxil.so"};
#endif
    std::error_code error;
    for (const char* name : {translator, dxc, "shader_common.h", libraries[0], libraries[1]})
        if (!std::filesystem::is_regular_file(tools / name, error)) return {};
    return {
        {"SFR_SHADER_TRANSLATOR", tools / translator},
        {"SFR_SHADER_COMMON", tools / "shader_common.h"},
        {"SFR_DXC", tools / dxc},
        {"SFR_DXC_SPIRV", tools / dxc},
        {"SFR_DXC_LIBRARY", tools},
        {"SFR_RUNTIME_SHADER_CACHE", launcher_directory / "shader-cache"},
    };
}

std::vector<WindowSize> common_window_sizes() {
    return {{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
}
}
