#include "launcher_settings.h"
#include "game_language.h"
#include "render_scale.h"
#include "aspect_settings.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <tuple>

namespace sfr {
namespace {
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

// A setting's number for the game, with a point whatever the locale, three
// places at most and no trailing zeros ("0.35", "1.75").
std::string fixed_decimal(double value) {
    char text[32];
    const auto result = std::to_chars(text, text + sizeof text, value, std::chars_format::fixed, 3);
    std::string out(text, result.ptr);
    if (out.find('.') != std::string::npos) {
        while (out.back() == '0') out.pop_back();
        if (out.back() == '.') out.pop_back();
    }
    return out;
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

uint32_t validated_resolution_scale(uint32_t scale) {
    for (const uint32_t choice : resolution_scale_choices())
        if (scale == choice) return scale;
    return 0;
}

// The game's logical frame (the title's 1280x720 back buffer).
constexpr uint32_t logical_width = 1280, logical_height = 720;

bool valid_size(uint32_t width, uint32_t height) {
    return width >= 160 && width <= 16384 && height >= 160 && height <= 16384;
}

// The window the game opens: the player's, else the desktop's, else 1280x720.
std::pair<uint32_t, uint32_t> window_size(const LauncherSettings& s) {
    if (valid_size(s.window_width, s.window_height)) return {s.window_width, s.window_height};
    if (valid_size(s.display_width, s.display_height)) return {s.display_width, s.display_height};
    return {logical_width, logical_height};
}

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
}

namespace {
// avatar_model_entry=<path> lines, one per model on the Avatar page.
std::string avatar_entries(const LauncherSettings& s) {
    std::string lines;
    for (const auto& model : s.avatar_models) lines += "avatar_model_entry=" + path_utf8(model) + '\n';
    return lines;
}
}

LauncherSettings parse_launcher_settings(const std::string& text) {
    LauncherSettings settings;
    bool ratio_seen = false, scale_seen = false;
    uint32_t old_render_scale = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string key = trim(std::string_view(line).substr(0, equals));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key == "window_width") read_number(value, 160, 16384, settings.window_width);
        else if (key == "window_height") read_number(value, 160, 16384, settings.window_height);
        // The old render_scale key was a percent of 1280x720, converted below
        // once the window is known.
        else if (key == "render_scale") read_number(value, 1, 1000, old_render_scale);
        else if (key == "resolution_scale") {
            uint32_t scale = 0;
            read_number(value, 0, 1000, scale);
            settings.resolution_scale = validated_resolution_scale(scale);
            scale_seen = true;
        }
        else if (key == "aspect_ratio" && (value == "auto" || value == "16:9")) {
            settings.aspect_ratio = value;
            ratio_seen = true;
        }
        // Before the ratio list: aspect_mode was auto or original.
        else if (key == "aspect_mode" && !ratio_seen && (value == "auto" || value == "original"))
            settings.aspect_ratio = value == "original" ? "16:9" : "auto";
        else if (key == "ui_align" && (value == "edge" || value == "center")) settings.ui_align = value;
        else if (key == "fullscreen") read_flag(value, settings.fullscreen);
        else if (key == "vsync") read_flag(value, settings.vsync);
        else if (key == "audio") read_flag(value, settings.audio);
        else if (key == "volume") read_number(value, 0, 100, settings.volume);
        else if (key == "skip_movies") read_flag(value, settings.skip_movies);
        else if (key == "vertex_cache") read_flag(value, settings.vertex_cache);
        else if (key == "gpu_pipeline") read_flag(value, settings.gpu_pipeline);
        else if (key == "race_render_every") read_number(value, 1, 4, settings.race_render_every);
        else if (key == "title_stats") read_flag(value, settings.title_stats);
        else if (key == "ui_sounds") read_flag(value, settings.ui_sounds);
        else if (key == "vulkan") read_flag(value, settings.vulkan);
        else if (key == "touch_controls") read_flag(value, settings.touch_controls);
        else if (key == "tilt") read_flag(value, settings.tilt);
        else if (key == "camera" && (value == "off" || value == "picture" || value == "motion" || value == "kinect")) settings.camera = value;
        else if (key == "camera_device" && value.size() <= 128) settings.camera_device = value;
        else if (key == "camera_mirror") read_flag(value, settings.camera_mirror);
        else if (key == "kinect_smooth") read_flag(value, settings.kinect_smooth);
        else if (key == "kinect_lean") read_number(value, 10, 100, settings.kinect_lean);
        else if (key == "kinect_crouch_cm") read_number(value, 5, 35, settings.kinect_crouch_cm);
        else if (key == "menu_steady_radius") read_number(value, 0, 30, settings.menu_steady_radius);
        else if (key == "voice") read_flag(value, settings.voice);
        else if (key == "camera_debug") read_flag(value, settings.camera_debug);
        else if (key == "motion_trace") read_flag(value, settings.motion_trace);
        else if (key == "avatar_model") settings.avatar_model = utf8_path(value);
        else if (key == "avatar_model_entry" && !value.empty() &&
                 settings.avatar_models.size() < LauncherSettings::max_avatar_models)
            settings.avatar_models.push_back(utf8_path(value));
        else if (key == "player1_device" && (value == "both" || value == "gamepad" || value == "keyboard"))
            settings.player1_device = value;
        else if (key == "player2_device" && (value == "gamepad" || value == "keyboard" || value == "off"))
            settings.player2_device = value;
        else if (key == "player1_gamepad" && value.size() <= 128) settings.player1_gamepad = value;
        else if (key == "player2_gamepad" && value.size() <= 128) settings.player2_gamepad = value;
        else if (key == "player1_keys" && value.size() <= 1024) settings.player1_keys = value;
        else if (key == "player1_pad" && value.size() <= 1024) settings.player1_pad = value;
        else if (key == "player2_keys" && value.size() <= 1024) settings.player2_keys = value;
        else if (key == "player2_pad" && value.size() <= 1024) settings.player2_pad = value;
        else if (key == "language" && (value == "auto" || value == "en" || value == "zh-TW")) settings.language = value;
        else if (key == "game_language") settings.game_language = validated_game_language(value);
        else if (key == "voice_language" && (value == "auto" || value == "en" || value == "ja")) settings.voice_language = value;
        else if (key == "image_directory") settings.image_directory = utf8_path(value);
        else if (key == "asset_directory") settings.asset_directory = utf8_path(value);
        else if (key == "save_directory") settings.save_directory = utf8_path(value);
        else if (key == "online") read_flag(value, settings.online);
        else if (key == "online_host") read_flag(value, settings.online_host);
        else if (key == "online_server" && value.size() <= 127) settings.online_server = value;
        else if (key == "online_name" && value.size() <= 15) settings.online_name = value;
    }
    // render_scale=X drew X% of 720 lines. 100 is Original; another becomes
    // the nearest step of the window that draws about as many lines.
    if (!scale_seen && old_render_scale && old_render_scale != 100) {
        const uint32_t output = settings.window_height ? settings.window_height : logical_height;
        const uint32_t scale = (old_render_scale * logical_height + output / 2) / output;
        settings.resolution_scale = std::clamp<uint32_t>((scale + 12) / 25 * 25, 25, 300);
    }
    // A model chosen before the list existed is the list's first entry.
    if (!settings.avatar_model.empty() &&
        std::find(settings.avatar_models.begin(), settings.avatar_models.end(), settings.avatar_model) ==
            settings.avatar_models.end()) {
        if (settings.avatar_models.size() >= LauncherSettings::max_avatar_models) settings.avatar_models.pop_back();
        settings.avatar_models.insert(settings.avatar_models.begin(), settings.avatar_model);
    }
    return settings;
}

std::string format_launcher_settings(const LauncherSettings& s) {
    std::ostringstream out;
    out << "# Sonic Free Riders Recompiled launcher settings\n"
        << "window_width=" << s.window_width << '\n'
        << "window_height=" << s.window_height << '\n'
        << "resolution_scale=" << validated_resolution_scale(s.resolution_scale) << '\n'
        << "aspect_ratio=" << (s.aspect_ratio == "auto" ? "auto" : "16:9") << '\n'
        << "ui_align=" << (s.ui_align == "center" ? "center" : "edge") << '\n'
        << "fullscreen=" << s.fullscreen << '\n'
        << "vsync=" << s.vsync << '\n'
        << "audio=" << s.audio << '\n'
        << "volume=" << s.volume << '\n'
        << "skip_movies=" << s.skip_movies << '\n'
        << "vertex_cache=" << s.vertex_cache << '\n'
        << "gpu_pipeline=" << s.gpu_pipeline << '\n'
        << "race_render_every=" << s.race_render_every << '\n'
        << "title_stats=" << s.title_stats << '\n'
        << "ui_sounds=" << s.ui_sounds << '\n'
        << "vulkan=" << s.vulkan << '\n'
        << "touch_controls=" << s.touch_controls << '\n'
        << "tilt=" << s.tilt << '\n'
        << "camera=" << s.camera << '\n'
        << "camera_device=" << s.camera_device << '\n'
        << "camera_mirror=" << s.camera_mirror << '\n'
        << "kinect_smooth=" << s.kinect_smooth << '\n'
        << "kinect_lean=" << s.kinect_lean << '\n'
        << "kinect_crouch_cm=" << s.kinect_crouch_cm << '\n'
        << "menu_steady_radius=" << (std::min)(s.menu_steady_radius, 30u) << '\n'
        << "voice=" << s.voice << '\n'
        << "camera_debug=" << s.camera_debug << '\n'
        << "motion_trace=" << s.motion_trace << '\n'
        << "avatar_model=" << path_utf8(s.avatar_model) << '\n'
        << avatar_entries(s)
        << "player1_device=" << s.player1_device << '\n'
        << "player2_device=" << s.player2_device << '\n'
        << "player1_gamepad=" << s.player1_gamepad << '\n'
        << "player2_gamepad=" << s.player2_gamepad << '\n'
        << "player1_keys=" << s.player1_keys << '\n'
        << "player1_pad=" << s.player1_pad << '\n'
        << "player2_keys=" << s.player2_keys << '\n'
        << "player2_pad=" << s.player2_pad << '\n'
        << "online=" << s.online << '\n'
        << "online_host=" << s.online_host << '\n'
        << "online_server=" << s.online_server << '\n'
        << "online_name=" << s.online_name << '\n'
        << "language=" << s.language << '\n'
        << "game_language=" << validated_game_language(s.game_language) << '\n'
        << "voice_language=" << s.voice_language << '\n'
        << "image_directory=" << path_utf8(s.image_directory) << '\n'
        << "asset_directory=" << path_utf8(s.asset_directory) << '\n'
        << "save_directory=" << path_utf8(s.save_directory) << '\n';
    return out.str();
}

LauncherSettings load_launcher_settings(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    auto settings = parse_launcher_settings(text.str());
    settings.avatar_model = resolved_avatar_model(settings, std::filesystem::absolute(file).parent_path());
    for (auto& model : settings.avatar_models)
        if (!model.is_absolute()) model = (std::filesystem::absolute(file).parent_path() / model).lexically_normal();
    return settings;
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

std::filesystem::path resolved_avatar_model(const LauncherSettings& settings, const std::filesystem::path& directory) {
    if (settings.avatar_model.empty() || settings.avatar_model.is_absolute() || directory.empty()) return settings.avatar_model;
    return (directory / settings.avatar_model).lexically_normal();
}

std::filesystem::path resolved_save_directory(const LauncherSettings& settings,
                                              const std::filesystem::path& directory) {
    if (settings.save_directory.empty()) return directory / "save";
    if (settings.save_directory.is_absolute() || directory.empty()) return settings.save_directory;
    return (directory / settings.save_directory).lexically_normal();
}

bool has_game_save(const std::filesystem::path& saves) {
    std::error_code error;
    for (std::filesystem::directory_iterator player(saves, error), end; !error && player != end; player.increment(error))
        if (std::filesystem::is_regular_file(player->path() / "00000001" / "SfrAllData.sav" / "SfrAllData.sav", error))
            return true;
    return false;
}

std::vector<std::pair<std::string, std::string>> game_environment(const LauncherSettings& s,
                                                               const std::filesystem::path& directory) {
    const auto [window_width, window_height] = window_size(s);
    const InternalResolution internal = internal_resolution(s);
    const bool original = s.aspect_ratio == "16:9";
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
        // Every guest thread in parallel, always: with one execution permit
        // ("parallel=0", the old "Multi-core execution" switch turned off)
        // the game stopped at start, since v0.6.0 at least (Issue #1). An
        // old settings.ini that still says parallel=0 is ignored.
        {"SFR_PARALLEL_WORKER", "all"},
        {"SFR_VERTEX_CACHE", s.vertex_cache ? "1" : "0"},
        {"SFR_GPU_PIPELINE", s.gpu_pipeline ? "1" : "0"},
        {"SFR_TITLE_STATS", s.title_stats ? "1" : "0"},
        {"SFR_AUDIO", s.audio ? "1" : "0"},
        // The player is signed in, so the game keeps records (docs/saves.md).
        {"SFR_PROFILE", "1"},
        {"SFR_GAME_LANGUAGE", std::string(validated_game_language(s.game_language))},
        {"SFR_VOICE_LANGUAGE", s.voice_language},
        {"SFR_VOLUME", std::to_string(s.volume)},
        {"SFR_SKIP_MOVIES", s.skip_movies ? "1" : ""},
        {"SFR_WINDOW_WIDTH", std::to_string(window_width)},
        {"SFR_WINDOW_HEIGHT", std::to_string(window_height)},
        // The resolution scale of the output height, as the game's percent
        // of its 720 lines (always set: an inherited value is overridden).
        {"SFR_RENDER_SCALE", std::to_string(internal.requested_percent)},
        {"SFR_ASPECT_MODE", original ? "original" : "auto"},
        // Clear any inherited fixed ratio; Auto follows the window or display.
        {"SFR_ASPECT_RATIO", ""},
        // The alignment only means something in auto mode.
        {"SFR_UI_ALIGN", !original && s.ui_align == "center" ? "center" : "edge"},
#ifdef __ANDROID__
        {"SFR_FULLSCREEN", "1"},  // a phone always fills its display (Issue #118)
#else
        {"SFR_FULLSCREEN", s.fullscreen ? "1" : "0"},
#endif
        {"SFR_VSYNC", s.vsync ? "1" : "0"},
        {"SFR_GRAPHICS", s.vulkan ? "vulkan" : "d3d12"},
        // "off" leaves the camera alone; "picture" opens it for the image the
        // title shows; "motion" also drives the Kinect player's body with it;
        // "kinect" is a real sensor tracking the players (kinect_sensor.h).
        {"SFR_CAMERA", s.camera == "off" ? "" : s.camera},
        {"SFR_CAMERA_DEVICE", s.camera_device},
        {"SFR_CAMERA_MIRROR", s.camera_mirror ? "1" : "0"},
        // Left unset at the defaults, so the game's own choices stand.
        {"SFR_KINECT_SMOOTH", s.kinect_smooth ? "" : "0"},
        {"SFR_KINECT_LEAN_SCALE", s.kinect_lean == 100 ? "" : fixed_decimal(3.5 * s.kinect_lean / 100.0)},
        {"SFR_KINECT_CROUCH_DEPTH", s.kinect_crouch_cm == 15 ? "" : fixed_decimal(s.kinect_crouch_cm / 100.0)},
        {"SFR_MENU_HAND_STEADY", s.menu_steady_radius ? "1" : "0"},
        {"SFR_MENU_STEADY_RADIUS", s.menu_steady_radius ? std::to_string((std::min)(s.menu_steady_radius, 30u)) : ""},
        {"SFR_VOICE", s.voice ? "1" : ""},
        {"SFR_CAMERA_RACE_TRACE", s.motion_trace && (s.camera == "kinect" || s.camera == "motion") ? "1" : ""},
        {"SFR_AVATAR", s.avatar_model.empty() ? "0" : "1"},
        {"SFR_AVATAR_MODEL", path_utf8(resolved_avatar_model(s, directory))},
        // Mods (mod_loader.h): HedgeModManager's or the launcher's cpkredir.ini.
        {"SFR_MODS_INI", path_utf8(directory / "cpkredir.ini")},
#ifdef _WIN32
        {"SFR_CAMERA_DEBUG", s.camera == "motion" && s.camera_debug ? "1" : "0"},
#else
        {"SFR_CAMERA_DEBUG", "0"},
#endif
        // Controls: who plays with what, and which button does what. Empty
        // removes the variable, and the game then uses its own defaults.
        {"SFR_PLAYER1_INPUT", s.player1_device},
        {"SFR_PLAYER2_INPUT", s.player2_device},
        {"SFR_PLAYER1_GAMEPAD", s.player1_gamepad},
        {"SFR_PLAYER2_GAMEPAD", s.player2_gamepad},
        {"SFR_PLAYER1_KEYS", s.player1_keys},
        {"SFR_PLAYER1_PAD", s.player1_pad},
        {"SFR_PLAYER2_KEYS", s.player2_keys},
        {"SFR_PLAYER2_PAD", s.player2_pad},
        // Online play (live_client.h, live_service.h).
        {"SFR_LIVE_HOST", s.online && s.online_host ? "1" : ""},
        {"SFR_LIVE_SERVER", s.online && !s.online_host ? s.online_server : ""},
        {"SFR_LIVE_NAME", s.online ? s.online_name : ""},
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
    // A release remains self-contained even when extracted below a checkout.
    // Otherwise start_game selects the ancestor's unversioned shader pack and
    // tools instead of the compatible files shipped beside this executable.
    if (std::filesystem::is_regular_file(launcher_directory / "shaders.pack", error)) return {};
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
    return {{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 720}, {2560, 1080},
            {2560, 1440}, {3440, 1440}, {3840, 2160}, {5120, 1440}};
}

const std::vector<std::string>& aspect_ratio_choices() {
    static const std::vector<std::string> choices{"auto", "16:9"};
    return choices;
}

bool is_listed_window_size(uint32_t width, uint32_t height, uint32_t desktop_width, uint32_t desktop_height) {
    if (width == desktop_width && height == desktop_height) return true;
    for (const auto& size : common_window_sizes())
        if (size.width == width && size.height == height) return true;
    return false;
}

bool parse_window_dimension(const std::string& text, uint32_t& value) {
    if (text.empty()) return false;
    uint64_t number = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        number = std::min<uint64_t>(number * 10 + uint64_t(c - '0'), 1000000);
    }
    value = uint32_t(std::clamp<uint64_t>(number, 160, 16384));
    return true;
}

void apply_default_window_size(LauncherSettings& settings, uint32_t desktop_width, uint32_t desktop_height) {
    if (valid_size(desktop_width, desktop_height)) {
        settings.display_width = desktop_width;
        settings.display_height = desktop_height;
    }
    if (valid_size(settings.window_width, settings.window_height)) return;
    const auto [width, height] = window_size(settings);
    settings.window_width = width;
    settings.window_height = height;
}

const std::vector<uint32_t>& resolution_scale_choices() {
    static const std::vector<uint32_t> choices = [] {
        std::vector<uint32_t> values;
        values.push_back(0);  // Original
        for (uint32_t percent = 25; percent <= 300; percent += 25) values.push_back(percent);
        return values;
    }();
    return choices;
}

std::string resolution_scale_label(uint32_t percent) {
    // Two decimals only when needed: 0.25x, 0.5x, 0.75x, 1.0x, 1.25x.
    char text[24];
    if (percent % 10 == 0) std::snprintf(text, sizeof text, "%u.%ux", percent / 100, percent % 100 / 10);
    else std::snprintf(text, sizeof text, "%u.%02ux", percent / 100, percent % 100);
    return text;
}

InternalResolution internal_resolution(const LauncherSettings& s) {
    return internal_resolution(s, s.resolution_scale);
}

InternalResolution internal_resolution(const LauncherSettings& s, uint32_t resolution_scale) {
    InternalResolution r{};
    // The frame is shown in the display in full screen (always on Android),
    // otherwise in the window.
#ifdef __ANDROID__
    const bool fullscreen = true;
#else
    const bool fullscreen = s.fullscreen;
#endif
    std::tie(r.output_width, r.output_height) =
        fullscreen && valid_size(s.display_width, s.display_height)
            ? std::pair{s.display_width, s.display_height} : window_size(s);
#ifdef __ANDROID__
    if (r.output_height > r.output_width) std::swap(r.output_width, r.output_height);  // played in landscape
#endif
    // percent of 720 = output_height * (scale / 100) / 720 * 100; Original
    // is 100 whatever the output.
    const uint32_t chosen = validated_resolution_scale(resolution_scale);
    const uint64_t scaled = uint64_t(r.output_height) * chosen;
    r.requested_percent = chosen ? uint32_t(std::clamp<uint64_t>((scaled + logical_height / 2) / logical_height,
                                                                 render_scale::min_percent, render_scale::max_percent))
                                 : render_scale::default_percent;
    // The logical frame is the title's 1280x720, widened in Auto for an
    // output wider than 16:9 as the game widens it (aspect_settings.cpp).
    r.logical_width = logical_width;
    if (s.aspect_ratio != "16:9" && r.output_height) {
        const double exact = double(logical_height) * r.output_width / r.output_height;
        const uint32_t wide = std::clamp<uint32_t>(uint32_t(std::lround(exact / 2.0)) * 2u, logical_width, 4096);
        if (aspect::wider_than_16x9(wide, logical_height)) r.logical_width = wide;
    }
    r.percent = render_scale::fit_percent(r.requested_percent, r.logical_width, logical_height);
    r.width = render_scale::scaled_extent(r.logical_width, r.percent);
    r.height = render_scale::scaled_extent(logical_height, r.percent);
    return r;
}
}
