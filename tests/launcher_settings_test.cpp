#include "launcher_settings.h"
#include "render_scale.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string value_of(const sfr::LauncherSettings& settings, const std::string& name,
                     const std::filesystem::path& directory = {}) {
    for (const auto& [key, value] : sfr::game_environment(settings, directory))
        if (key == name) return value;
    throw std::runtime_error("variable missing from the game environment");
}

void settings_round_trip() {
    sfr::LauncherSettings settings;
    settings.window_width = 1920;
    settings.window_height = 1080;
    settings.fullscreen = true;
    settings.vsync = true;
    settings.audio = false;
    settings.volume = 35;
    settings.skip_movies = true;
    settings.vertex_cache = false;
    settings.gpu_pipeline = false;
    settings.race_render_every = 2;
    settings.ui_sounds = false;
    settings.vulkan = true;
    settings.camera = "motion";
    settings.camera_device = "e2eSoft iVCam #2";
    settings.camera_mirror = true;
    settings.voice = true;
    settings.player1_device = "keyboard";
    settings.player2_device = "gamepad";
    settings.player1_gamepad = "Controller #1";
    settings.player2_gamepad = "Controller #2";
    settings.player1_keys = "a=G,b=H";
    settings.player2_keys = "a=J,b=K";
    settings.player1_pad = "a=b,b=a";
    settings.player2_pad = "a=x,x=a";
    settings.image_directory = std::filesystem::path(u8"C:/遊戲/image");
    settings.asset_directory = "D:/assets";
    const auto read = sfr::parse_launcher_settings(sfr::format_launcher_settings(settings));
    require(read.window_width == 1920 && read.window_height == 1080, "window size survives a round trip");
    require(read.fullscreen && read.vsync && !read.audio && read.volume == 35, "display and sound survive a round trip");
    require(read.skip_movies && !read.vertex_cache && !read.gpu_pipeline && read.race_render_every == 2 && !read.ui_sounds && read.vulkan,
            "advanced settings survive a round trip");
    require(read.camera == "motion" && read.camera_device == "e2eSoft iVCam #2" && read.camera_mirror,
            "the camera choice, name, and mirror setting survive a round trip");
    require(read.player1_device == settings.player1_device && read.player2_device == settings.player2_device &&
            read.player1_gamepad == settings.player1_gamepad && read.player2_gamepad == settings.player2_gamepad &&
            read.player1_keys == settings.player1_keys && read.player2_keys == settings.player2_keys &&
            read.player1_pad == settings.player1_pad && read.player2_pad == settings.player2_pad,
            "both players' controls survive a round trip alongside camera and pipeline settings");
    require(value_of(read, "SFR_CAMERA") == "motion" && value_of(read, "SFR_CAMERA_DEVICE") == settings.camera_device &&
            value_of(read, "SFR_CAMERA_MIRROR") == "1" && value_of(read, "SFR_VOICE") == "1" && read.voice &&
            value_of(read, "SFR_GPU_PIPELINE") == "0" &&
            value_of(read, "SFR_PLAYER1_INPUT") == settings.player1_device &&
            value_of(read, "SFR_PLAYER2_PAD") == settings.player2_pad,
            "camera, pipeline, and custom controls reach the game together");
    require(read.image_directory == settings.image_directory && read.asset_directory == settings.asset_directory,
            "non-ASCII directories survive a round trip");
}

void game_language_settings() {
    for (const char* code : {"auto", "en", "ja", "de", "fr", "es", "it"}) {
        const auto loaded = sfr::parse_launcher_settings(std::string("language=zh-TW\ngame_language=") + code + "\n");
        const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(loaded));
        require(saved.game_language == code && saved.language == "zh-TW" &&
                value_of(saved, "SFR_GAME_LANGUAGE") == code,
                "game language persists and reaches the runtime independently of launcher language");
    }
    for (const char* text : {"", "language=en\n", "game_language=invalid\n", "game_language=ES\n"})
        require(value_of(sfr::parse_launcher_settings(text), "SFR_GAME_LANGUAGE") == "auto",
                "legacy or invalid game language explicitly resets an inherited override");
}

void voice_language_settings() {
    for (const char* code : {"auto", "en", "ja"}) {
        const auto loaded = sfr::parse_launcher_settings(std::string("game_language=ja\nvoice_language=") + code + "\n");
        const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(loaded));
        require(saved.voice_language == code && saved.game_language == "ja" &&
                value_of(saved, "SFR_VOICE_LANGUAGE") == code,
                "voice language persists and reaches the runtime apart from the game language");
    }
    for (const char* text : {"", "voice_language=de\n", "voice_language=EN\n"})
        require(value_of(sfr::parse_launcher_settings(text), "SFR_VOICE_LANGUAGE") == "auto",
                "a missing or unknown voice language follows the game");
}

void graphics_backend_settings() {
    for (const char* text : {"", "vulkan=invalid\n", "window_width=1920\n"})
        require(value_of(sfr::parse_launcher_settings(text), "SFR_GRAPHICS") == "vulkan",
                "new, missing and invalid backend settings default to Vulkan");
    for (const char* text : {"vulkan=0\n", "vulkan=false\n", "vulkan=1\n", "vulkan=true\n"}) {
        const auto loaded = sfr::parse_launcher_settings(text);
        const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(loaded));
        const bool vulkan = std::string(text).find('0') == std::string::npos &&
                            std::string(text).find("false") == std::string::npos;
        require(saved.vulkan == vulkan &&
                value_of(saved, "SFR_GRAPHICS") == (vulkan ? "vulkan" : "d3d12"),
                "existing explicit backend choices survive load/save and reach the game");
    }
}

void kinect_tuning_settings() {
    const sfr::LauncherSettings defaults;
    require(defaults.kinect_smooth && defaults.kinect_lean == 100 && defaults.kinect_crouch_cm == 15,
            "Kinect tuning defaults to the game's own behaviour");
    for (const char* name : {"SFR_KINECT_SMOOTH", "SFR_KINECT_LEAN_SCALE", "SFR_KINECT_CROUCH_DEPTH"})
        require(value_of(defaults, name).empty(), "at the defaults the game decides for itself");
    const auto tuned = sfr::parse_launcher_settings(sfr::format_launcher_settings(sfr::parse_launcher_settings(
        "kinect_smooth=0\nkinect_lean=50\nkinect_crouch_cm=20\n")));
    require(!tuned.kinect_smooth && tuned.kinect_lean == 50 && tuned.kinect_crouch_cm == 20,
            "Kinect tuning survives a save and a load");
    require(value_of(tuned, "SFR_KINECT_SMOOTH") == "0", "smoothing off reaches the game");
    require(value_of(tuned, "SFR_KINECT_LEAN_SCALE") == "1.75", "half the lean is half of 3.5");
    require(value_of(tuned, "SFR_KINECT_CROUCH_DEPTH") == "0.2", "twenty centimetres are 0.2 metres");
    const auto odd = sfr::parse_launcher_settings("kinect_lean=5\nkinect_crouch_cm=40\n");
    require(odd.kinect_lean == 100 && odd.kinect_crouch_cm == 15, "values outside the sliders are ignored");
}

void runtime_render_scale() {
    namespace rs = sfr::render_scale;
    bool valid = false;
    require(rs::parse_percent(nullptr) == 100 && rs::parse_percent("") == 100, "no SFR_RENDER_SCALE is 100%");
    require(rs::parse_percent("167", &valid) == 167 && valid, "any whole percent is accepted");
    require(rs::parse_percent("10") == 25 && rs::parse_percent("900") == 800 && rs::parse_percent("99999999999") == 100,
            "percents are clamped to 25..800, absurd numbers ignored");
    require(rs::parse_percent("1.5", &valid) == 100 && !valid && rs::parse_percent("-50") == 100,
            "malformed values fall back to 100%");
    require(rs::fit_percent(800, 1280, 720) == 640 && rs::fit_percent(400, 4096, 720) == 200 &&
            rs::fit_percent(150, 1720, 720) == 150, "the percent is lowered only past 8192 pixels");
    require(rs::scaled_extent(1720, 201) == 3458 && rs::scaled_extent(720, 167) == 1202 &&
            rs::scaled_extent(19, 75) == 14 && rs::scaled_extent(2, 25) == 2,
            "even sides stay even, odd ones round to the nearest pixel");
}
void aspect_settings() {
    const sfr::LauncherSettings defaults;
#ifdef __ANDROID__
    require(defaults.aspect_ratio == "16:9", "a phone keeps the original picture");
#else
    require(defaults.aspect_ratio == "auto" && defaults.ui_align == "edge", "aspect defaults to auto with edge alignment");
    require(value_of(defaults, "SFR_ASPECT_MODE") == "auto" && value_of(defaults, "SFR_UI_ALIGN") == "edge",
            "the aspect variables are always set");
    require(sfr::format_launcher_settings(defaults).find("aspect_ratio=auto\n") != std::string::npos,
            "Auto is saved");
#endif
    require(value_of(defaults, "SFR_ASPECT_RATIO").empty(), "Auto passes no ratio, the window's shape decides");
    require(value_of(defaults, "SFR_WINDOW_WIDTH") == "1280", "Auto keeps the window width the player chose");
    const auto center = sfr::parse_launcher_settings("aspect_ratio=auto\nui_align=center\n");
    require(value_of(center, "SFR_UI_ALIGN") == "center", "center alignment reaches the game in auto mode");

    const auto original = sfr::parse_launcher_settings("aspect_ratio=16:9\nui_align=center\nwindow_width=3440\nwindow_height=1440\n");
    require(value_of(original, "SFR_ASPECT_MODE") == "original" && value_of(original, "SFR_ASPECT_RATIO").empty(),
            "16:9 selects the original picture without a fixed ratio override");
    require(value_of(original, "SFR_WINDOW_WIDTH") == "3440" && value_of(original, "SFR_WINDOW_HEIGHT") == "1440",
            "original mode preserves the selected output resolution");
    const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(original));
    require(saved.aspect_ratio == "16:9" && saved.ui_align == "center", "hidden alignment choice is preserved");
    require(value_of(saved, "SFR_UI_ALIGN") == "edge", "original mode ignores the stored alignment");
    require(sfr::parse_launcher_settings("aspect_mode=original\n").aspect_ratio == "16:9", "old original is 16:9");
    require(sfr::parse_launcher_settings("aspect_mode=auto\n").aspect_ratio == "auto", "old auto is auto");
    require(sfr::parse_launcher_settings("aspect_ratio=auto\naspect_mode=original\n").aspect_ratio == "auto",
            "new ratio key wins over the old key");
    for (const char* ratio : {"21:9", "32:9", "48:9", "4:3"}) {
        const auto bad = sfr::parse_launcher_settings(std::string("aspect_ratio=") + ratio + "\nui_align=left\n");
        require(bad.aspect_ratio == defaults.aspect_ratio && bad.ui_align == "edge", "unsupported choices use defaults");
    }
    require(sfr::aspect_ratio_choices() == std::vector<std::string>{"auto", "16:9"}, "launcher offers only Auto and 16:9");
    const auto wide = sfr::parse_launcher_settings("aspect_ratio=auto\nwindow_width=5120\nwindow_height=1440\nui_align=center\n");
    require(value_of(wide, "SFR_WINDOW_WIDTH") == "5120" && value_of(wide, "SFR_WINDOW_HEIGHT") == "1440" &&
            value_of(wide, "SFR_ASPECT_RATIO").empty() && value_of(wide, "SFR_UI_ALIGN") == "center",
            "Auto preserves an ultrawide resolution and the chosen HUD alignment");
    const auto sizes = sfr::common_window_sizes();
    for (const auto& wide_size : {std::pair{2560u, 720u}, std::pair{2560u, 1080u}, std::pair{3440u, 1440u},
                                  std::pair{5120u, 1440u}})
        require(std::any_of(sizes.begin(), sizes.end(),
                            [&](const sfr::WindowSize& s) {
                                return s.width == wide_size.first && s.height == wide_size.second;
                            }),
                "the common window sizes include the ultrawide ones");
}

void rendering_resolution_settings() {
    const sfr::LauncherSettings defaults;
    require(defaults.resolution_scale == 0, "the resolution scale defaults to Original");
    require(sfr::format_launcher_settings(defaults).find("resolution_scale=0\n") != std::string::npos,
            "the resolution scale is saved as a percent, 0 for Original");
    const auto& choices = sfr::resolution_scale_choices();
    require(choices.size() == 13 && choices.front() == 0 && choices.back() == 300,
            "Original, then 0.25x to 3.0x in 12 steps");
    for (size_t i = 1; i < choices.size(); ++i)
        require(choices[i] == 25 * i, "the choices are every multiple of 25 from 25 to 300");
    // Original draws the game's own 720 lines whatever the output: what every
    // earlier version drew, on a 4K display or a phone alike.
    for (const char* window : {"window_width=3840\nwindow_height=2160\nfullscreen=0\n",
                               "window_width=1920\nwindow_height=1080\nfullscreen=0\n"})
        require(value_of(sfr::parse_launcher_settings(window), "SFR_RENDER_SCALE") == "100",
                "Original renders 720 lines at any output size");
    require(sfr::resolution_scale_label(25) == "0.25x" && sfr::resolution_scale_label(50) == "0.5x" &&
            sfr::resolution_scale_label(75) == "0.75x" && sfr::resolution_scale_label(100) == "1.0x" &&
            sfr::resolution_scale_label(125) == "1.25x" && sfr::resolution_scale_label(150) == "1.5x" &&
            sfr::resolution_scale_label(175) == "1.75x" && sfr::resolution_scale_label(300) == "3.0x",
            "labels show two decimals only when needed");
    require(value_of(defaults, "SFR_RENDER_SCALE") == "100",
            "without a window or desktop size 1.0x renders the title's 720 lines");

    // 1.0x draws as many lines as the window shows: the game's percent of 720.
    const struct { const char* window; const char* scale; const char* percent; uint32_t width, height; } cases[] = {
        {"window_width=3440\nwindow_height=1440\n", "100", "200", 2560, 1440},
        {"window_width=3440\nwindow_height=1440\n", "50", "100", 1280, 720},
        {"window_width=1920\nwindow_height=1080\n", "100", "150", 1920, 1080},
        {"window_width=1920\nwindow_height=1080\n", "150", "225", 2880, 1620},
        {"window_width=1280\nwindow_height=720\n", "300", "300", 3840, 2160},
        {"window_width=2560\nwindow_height=1440\n", "250", "500", 6400, 3600},
        {"window_width=5120\nwindow_height=1440\n", "100", "200", 2560, 1440},
        // The frame stays 1280x720 (16:9) whatever the window's shape; only
        // its height decides the scale. 166.7% rounds to 167.
        {"window_width=1920\nwindow_height=1200\n", "100", "167", 2138, 1202},
        // 37.5% rounds to 38, and odd sides round to even ones.
        {"window_width=960\nwindow_height=540\n", "50", "38", 486, 274},
    };
    for (const auto& c : cases) {
        const auto settings =
            sfr::parse_launcher_settings(std::string(c.window) + "aspect_ratio=16:9\nresolution_scale=" + c.scale + "\n");
        require(std::to_string(settings.resolution_scale) == c.scale, "each resolution scale is read");
        const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(settings));
        require(saved.resolution_scale == settings.resolution_scale, "the resolution scale survives a round trip");
        require(value_of(saved, "SFR_RENDER_SCALE") == c.percent,
                "SFR_RENDER_SCALE = round(window height * scale / 720 * 100)");
        const auto internal = sfr::internal_resolution(saved);
        require(internal.width == c.width && internal.height == c.height && internal.width % 2 == 0 &&
                internal.height % 2 == 0, "the internal size scales the 1280x720 frame, in even pixels");
        require(value_of(saved, "SFR_WINDOW_WIDTH") == std::to_string(saved.window_width),
                "the resolution scale does not change the window size");
    }

    // Full screen scales the display, not the window.
    auto full = sfr::parse_launcher_settings(
        "window_width=1280\nwindow_height=720\nfullscreen=1\naspect_ratio=16:9\nresolution_scale=100\n");
    sfr::apply_default_window_size(full, 3840, 2160);
    require(full.window_width == 1280 && full.window_height == 720, "a saved window size is kept");
    require(value_of(full, "SFR_RENDER_SCALE") == "300", "full screen scales the display's height");
    // 3.0x of 2160 lines asks for 900%: the most is 800%, and 1280 wide at
    // 720 lines may not exceed 8192 pixels (640%).
    full.resolution_scale = 300;
    require(value_of(full, "SFR_RENDER_SCALE") == "800", "the asked percent stays within 25..800");
    const auto big = sfr::internal_resolution(full);
    require(big.percent == 640 && big.width == 8192 && big.height == 4608, "and is lowered to fit 8192 pixels");
    full.display_width = 5120;
    full.display_height = 1440;
    const auto wide = sfr::internal_resolution(full);
    require(wide.logical_width == 1280 && wide.requested_percent == 600 && wide.percent == 600 &&
            wide.width == 7680 && wide.height == 4320, "with 16:9 a wide display still scales the 1280x720 frame");
    // Auto widens the logical frame as the game does, so the internal size
    // keeps the output's shape.
    full.aspect_ratio = "auto";
    full.display_width = 3440;
    full.display_height = 1440;
    full.resolution_scale = 100;
    const auto auto_wide = sfr::internal_resolution(full);
    require(auto_wide.logical_width == 1720 && auto_wide.width == 3440 && auto_wide.height == 1440,
            "Auto at 1.0x renders a 3440x1440 display at its own size");
    full.resolution_scale = 0;
    const auto auto_original = sfr::internal_resolution(full);
    require(auto_original.percent == 100 && auto_original.width == 1720 && auto_original.height == 720,
            "Original in Auto keeps 720 lines across the wider frame");

    // The old key (a percent of 1280x720) becomes the step that draws about
    // as many lines in the saved window; 100 was the original 720 lines.
    const struct { const char* window; const char* old; uint32_t scale; } olds[] = {
        {"window_width=1280\nwindow_height=720\n", "100", 0},
        {"window_width=1280\nwindow_height=720\n", "200", 200},
        {"window_width=1280\nwindow_height=720\n", "50", 50},
        {"window_width=1920\nwindow_height=1080\n", "150", 100},
        {"window_width=1920\nwindow_height=1080\n", "300", 200},
        {"window_width=1920\nwindow_height=1080\n", "50", 25},
        {"", "75", 75},
    };
    for (const auto& o : olds) {
        const auto old = sfr::parse_launcher_settings(std::string(o.window) + "render_scale=" + o.old + "\n");
        require(old.resolution_scale == o.scale, "an old render_scale is converted to the nearest step");
        require(sfr::format_launcher_settings(old).find("render_scale=") == std::string::npos,
                "and is not saved again");
    }
    require(sfr::parse_launcher_settings("render_scale=200\nresolution_scale=150\n").resolution_scale == 150,
            "the new key wins over the old one");
    for (const char* scale : {"", "abc", "24", "26", "49", "99", "101", "110", "130", "310", "350", "400",
                              "-50", "+50", "50.0", "50junk", "4294967296"}) {
        const auto settings = sfr::parse_launcher_settings(std::string("resolution_scale=") + scale + "\n");
        require(settings.resolution_scale == 0, "malformed or unsupported resolution scales read as Original");
    }
    require(sfr::parse_launcher_settings("resolution_scale=50\nresolution_scale=130\n").resolution_scale == 0,
            "an invalid later resolution scale resets to Original");
    sfr::LauncherSettings invalid;
    invalid.resolution_scale = 130;
    require(sfr::format_launcher_settings(invalid).find("resolution_scale=0\n") != std::string::npos &&
            value_of(invalid, "SFR_RENDER_SCALE") == "100",
            "unsupported in-memory settings serialize and launch as Original");
}

void window_size_defaults_to_the_desktop() {
    sfr::LauncherSettings fresh;
    require(fresh.window_width == 0 && fresh.window_height == 0, "no window size is chosen at first");
    require(value_of(fresh, "SFR_WINDOW_WIDTH") == "1280" && value_of(fresh, "SFR_WINDOW_HEIGHT") == "720",
            "without a desktop size the game gets 1280x720");
    sfr::apply_default_window_size(fresh, 3440, 1440);
    require(fresh.window_width == 3440 && fresh.window_height == 1440, "an unset window takes the desktop size");
    require(value_of(fresh, "SFR_RENDER_SCALE") == "100", "and Original still renders 720 lines");
    fresh.resolution_scale = 100;
    require(value_of(fresh, "SFR_RENDER_SCALE") == "200", "while 1.0x renders the desktop's lines");
    const auto saved = sfr::parse_launcher_settings(sfr::format_launcher_settings(fresh));
    require(saved.window_width == 3440 && saved.window_height == 1440, "the filled-in size is saved");
    auto chosen = sfr::parse_launcher_settings("fullscreen=0\nwindow_width=1600\nwindow_height=900\n");
    sfr::apply_default_window_size(chosen, 3440, 1440);
    require(chosen.window_width == 1600 && chosen.window_height == 900, "an explicit window size is kept");
    auto partial = sfr::parse_launcher_settings("fullscreen=0\nwindow_width=1600\n");
    sfr::apply_default_window_size(partial, 2560, 1440);
    require(partial.window_width == 2560 && partial.window_height == 1440, "half a size is not a choice");
    auto unknown = sfr::parse_launcher_settings("");
    sfr::apply_default_window_size(unknown, 0, 0);
    require(unknown.window_width == 1280 && unknown.window_height == 720, "an unknown desktop leaves 1280x720");
}

void custom_window_size() {
    require(sfr::is_listed_window_size(1920, 1080, 3440, 1440), "a common size is listed");
    require(sfr::is_listed_window_size(3440, 1440, 3440, 1440), "the desktop size is listed");
    require(!sfr::is_listed_window_size(3000, 1300, 1920, 1080), "an unlisted size is custom");
    require(!sfr::is_listed_window_size(1280, 721, 1920, 1080), "one pixel off is custom");
    uint32_t value = 777;
    require(sfr::parse_window_dimension("1920", value) && value == 1920, "digits are read");
    require(sfr::parse_window_dimension("0001600", value) && value == 1600, "leading zeros are fine");
    require(sfr::parse_window_dimension("5", value) && value == 160, "a small value is raised to 160");
    require(sfr::parse_window_dimension("20000", value) && value == 16384, "a large value is lowered to 16384");
    require(sfr::parse_window_dimension("99999999999999999999", value) && value == 16384, "an enormous value cannot overflow");
    value = 777;
    for (const char* text : {"", " ", "12a", "-5", "1e3", "19 20", "+7"})
        require(!sfr::parse_window_dimension(text, value) && value == 777, "empty or non-numeric text is ignored");
    auto custom = sfr::parse_launcher_settings("fullscreen=0\nwindow_width=3000\nwindow_height=1300\n");
    sfr::apply_default_window_size(custom, 1920, 1080);
    require(custom.window_width == 3000 && custom.window_height == 1300, "a custom size persists through the existing keys");
    require(!sfr::is_listed_window_size(custom.window_width, custom.window_height, 1920, 1080),
            "and is custom again on the next launch");
    require(sfr::internal_resolution(custom).output_height == 1300, "the scale follows the custom size");
}

void title_stats_settings() {
    const sfr::LauncherSettings defaults;
    require(defaults.title_stats && value_of(defaults, "SFR_TITLE_STATS") == "1" &&
                sfr::format_launcher_settings(defaults).find("title_stats=1\n") != std::string::npos,
            "the title shows the performance by default");
    const auto off = sfr::parse_launcher_settings("title_stats=0\n");
    require(!off.title_stats && value_of(off, "SFR_TITLE_STATS") == "0" &&
                sfr::format_launcher_settings(off).find("title_stats=0\n") != std::string::npos,
            "turned off, the title is left alone and it stays off");
}

void save_directory_settings() {
    const auto root = std::filesystem::temp_directory_path() / "sfr_launcher_saves_test";
    std::filesystem::remove_all(root);
    const sfr::LauncherSettings defaults;
    require(sfr::resolved_save_directory(defaults, root) == root / "save", "the saves default to save beside the launcher");
    require(sfr::format_launcher_settings(defaults).find("save_directory=\n") != std::string::npos,
            "an empty save folder is written as empty");
    const auto relative = sfr::parse_launcher_settings("save_directory=../saves\n");
    require(relative.save_directory == std::filesystem::path("../saves") &&
                sfr::resolved_save_directory(relative, root / "launcher") == root / "saves",
            "a relative save folder is from the launcher");
    const auto chosen = (root / "elsewhere").lexically_normal();
    sfr::LauncherSettings absolute;
    absolute.save_directory = chosen;
    require(sfr::resolved_save_directory(sfr::parse_launcher_settings(sfr::format_launcher_settings(absolute)), root) == chosen,
            "a chosen save folder survives a save and is used as it is");
    require(!sfr::has_game_save(chosen), "a missing folder holds no save");
    std::filesystem::create_directories(chosen / "E000C56DDFFD8EA0" / "00000001" / "SfrAllData.sav");
    require(!sfr::has_game_save(chosen), "an empty content folder is no save");
    std::ofstream(chosen / "E000C56DDFFD8EA0" / "00000001" / "SfrAllData.sav" / "SfrAllData.sav") << "x";
    require(sfr::has_game_save(chosen), "a player's SfrAllData.sav is a save");
    std::filesystem::remove_all(root);
}

void menu_steady_settings() {
    const sfr::LauncherSettings defaults;
    require(sfr::format_launcher_settings(defaults).find("menu_steady_radius=8\n") != std::string::npos,
            "the menu cursor steadying defaults to an 8 mm circle");
    require(value_of(defaults, "SFR_MENU_HAND_STEADY") == "1" && value_of(defaults, "SFR_MENU_STEADY_RADIUS") == "8",
            "the default circle reaches the game");
    const auto wide = sfr::parse_launcher_settings("menu_steady_radius=15\n");
    require(wide.menu_steady_radius == 15 && value_of(wide, "SFR_MENU_STEADY_RADIUS") == "15" &&
                sfr::format_launcher_settings(wide).find("menu_steady_radius=15\n") != std::string::npos,
            "a chosen circle survives a save and reaches the game");
    const auto off = sfr::parse_launcher_settings("menu_steady_radius=0\n");
    require(value_of(off, "SFR_MENU_HAND_STEADY") == "0" && value_of(off, "SFR_MENU_STEADY_RADIUS").empty(),
            "0 starts the steadying off");
    for (const char* bad : {"31", "-1", "big"})
        require(sfr::parse_launcher_settings(std::string("menu_steady_radius=") + bad + "\n").menu_steady_radius == 8,
                "an out-of-range circle keeps the default");
}

void motion_trace_settings() {
    const sfr::LauncherSettings defaults;
    require(!defaults.motion_trace && sfr::format_launcher_settings(defaults).find("motion_trace=0\n") != std::string::npos,
            "the motion trace defaults to off");
    require(value_of(defaults, "SFR_CAMERA_RACE_TRACE").empty(), "with the trace off the game writes no trace");
    for (const char* camera : {"kinect", "motion"}) {
        const auto traced = sfr::parse_launcher_settings(sfr::format_launcher_settings(sfr::parse_launcher_settings(
            std::string("camera=") + camera + "\nmotion_trace=1\n")));
        require(traced.motion_trace && sfr::format_launcher_settings(traced).find("motion_trace=1\n") != std::string::npos,
                "the motion trace survives a save and a load");
        require(value_of(traced, "SFR_CAMERA_RACE_TRACE") == "1", "the motion trace reaches the game for a body camera");
    }
    for (const char* camera : {"off", "picture"}) {
        const auto pad = sfr::parse_launcher_settings(std::string("camera=") + camera + "\nmotion_trace=1\n");
        require(pad.motion_trace && value_of(pad, "SFR_CAMERA_RACE_TRACE").empty(),
                "without a Kinect or the motion camera there is nothing to trace, but the choice is kept");
    }
    for (const char* flag : {"0", "false", "invalid"})
        require(!sfr::parse_launcher_settings(std::string("camera=kinect\nmotion_trace=") + flag + "\n").motion_trace,
                "false or invalid trace flags keep the trace off");
}

void camera_debug_settings() {
    const sfr::LauncherSettings defaults;
    require(sfr::format_launcher_settings(defaults).find("camera_debug=0\n") != std::string::npos,
            "the skeleton debug window defaults to off");
    for (const char* flag : {"1", "true"}) {
        const auto enabled = sfr::parse_launcher_settings(std::string("camera=motion\ncamera_debug=") + flag + "\n");
        const auto saved = sfr::format_launcher_settings(enabled);
        require(saved.find("camera_debug=1\n") != std::string::npos,
                "the debug preference survives a save");
        require(sfr::format_launcher_settings(sfr::parse_launcher_settings(saved)) == saved,
                "the debug preference survives reloading");
    }
    for (const char* flag : {"0", "false", "invalid"}) {
        const auto disabled = sfr::parse_launcher_settings(std::string("camera_debug=") + flag + "\n");
        require(sfr::format_launcher_settings(disabled).find("camera_debug=0\n") != std::string::npos,
                "false or invalid debug flags keep debugging off");
    }
    for (const char* camera : {"off", "picture", "motion"}) {
        for (const char* flag : {"0", "1"}) {
            const auto settings = sfr::parse_launcher_settings(std::string("camera=") + camera + "\ncamera_debug=" + flag + "\n");
            std::string expected = "0";
#ifdef _WIN32
            if (settings.camera == "motion" && std::string(flag) == "1") expected = "1";
#endif
            require(value_of(settings, "SFR_CAMERA_DEBUG") == expected,
                    "debugging explicitly overrides the inherited environment and requires Windows motion mode plus opt-in");
        }
    }
}

void avatar_model_settings() {
    const auto defaults = sfr::parse_launcher_settings("volume=35\n");
    require(sfr::format_launcher_settings(defaults).find("avatar_model=\n") != std::string::npos,
            "older settings default to no avatar model");
    require(value_of(defaults, "SFR_AVATAR") == "0" && value_of(defaults, "SFR_AVATAR_MODEL").empty(),
            "no selected model disables the avatar and clears an inherited model");

    const std::u8string model = u8"C:/遊戲/My Avatars/騎士 = 1.vrm";
    const std::string model_utf8(model.begin(), model.end());
    const auto selected = sfr::parse_launcher_settings("avatar_model=" + model_utf8 + "\n");
    const auto saved = sfr::format_launcher_settings(selected);
    require(saved.find("avatar_model=" + model_utf8 + "\n") != std::string::npos,
            "the selected Unicode model path with spaces is serialized as UTF-8");
    const auto reloaded = sfr::parse_launcher_settings(saved);
    require(value_of(reloaded, "SFR_AVATAR_MODEL") == model_utf8 && value_of(reloaded, "SFR_AVATAR") == "1",
            "the selected model survives a round trip and enables the avatar at launch");
    const auto cleared = sfr::parse_launcher_settings(saved + "avatar_model=\n");
    require(value_of(cleared, "SFR_AVATAR_MODEL").empty() && value_of(cleared, "SFR_AVATAR") == "0",
            "clearing a previously selected model overrides a stale inherited environment");
    // Issue #132: the models added are kept, so another can be chosen later.
    require(reloaded.avatar_models.size() == 1 && reloaded.avatar_models[0] == selected.avatar_model,
            "a model chosen before the list existed becomes its first entry");
    auto several = reloaded;
    several.avatar_models.push_back("D:/avatars/second.glb");
    several.avatar_model = "D:/avatars/second.glb";
    const auto both = sfr::parse_launcher_settings(sfr::format_launcher_settings(several));
    require(both.avatar_models.size() == 2 && both.avatar_models[1] == "D:/avatars/second.glb" &&
            value_of(both, "SFR_AVATAR_MODEL") == "D:/avatars/second.glb",
            "every added model is kept, and the chosen one is used");
    auto none = both;
    none.avatar_model.clear();
    const auto kept = sfr::parse_launcher_settings(sfr::format_launcher_settings(none));
    require(kept.avatar_models.size() == 2 && value_of(kept, "SFR_AVATAR") == "0",
            "choosing no model keeps the list and disables the avatar");
    const auto missing = sfr::parse_launcher_settings("avatar_model=Z:/missing/avatar.glb\n");
    require(value_of(missing, "SFR_AVATAR_MODEL") == "Z:/missing/avatar.glb",
            "an unavailable model stays selected so it can be reported instead of silently discarded");
}

void display_mode_defaults_to_borderless() {
    require(sfr::LauncherSettings{}.fullscreen, "Borderless is the default display mode");
    require(sfr::parse_launcher_settings("").fullscreen, "a missing fullscreen key is Borderless");
    require(sfr::parse_launcher_settings("window_width=1280\n").fullscreen, "other keys leave Borderless alone");
    require(!sfr::parse_launcher_settings("fullscreen=0\n").fullscreen, "an explicit 0 is Windowed");
    require(sfr::parse_launcher_settings("fullscreen=1\n").fullscreen, "an explicit 1 is Borderless");
    sfr::LauncherSettings windowed;
    windowed.fullscreen = false;
    require(!sfr::parse_launcher_settings(sfr::format_launcher_settings(windowed)).fullscreen,
            "Windowed survives a round trip");
}

void malformed_values_keep_defaults() {
    const auto read = sfr::parse_launcher_settings(
        "window_width=12\nwindow_height=abc\nvolume=101\nfullscreen=maybe\nrace_render_every=9\n"
        "unknown=1\n# vsync=1\nno equals sign\n  audio = 0  \r\ncamera=webcam\n");
    const sfr::LauncherSettings defaults;
    require(read.camera == defaults.camera, "an unknown camera choice keeps the default");
    require(sfr::parse_launcher_settings("camera=kinect\n").camera == "kinect", "a real Kinect is a camera choice");
    require(sfr::parse_launcher_settings("camera_device=" + std::string(200, 'x') + "\n").camera_device == defaults.camera_device,
            "a name longer than any camera has keeps the default");
    require(read.window_width == defaults.window_width && read.window_height == defaults.window_height,
            "out-of-range or non-numeric sizes keep the default");
    require(read.volume == defaults.volume && read.fullscreen == defaults.fullscreen &&
            read.race_render_every == defaults.race_render_every, "invalid values keep the default");
    require(read.vsync == defaults.vsync, "comments are not settings");
    require(!read.audio, "surrounding spaces and CR are ignored");
}

void environment_follows_settings() {
    sfr::LauncherSettings settings;
    require(value_of(settings, "SFR_FRAME_LIMIT") == "60", "the game is capped at 60 fps");
    require(value_of(settings, "SFR_PARALLEL_WORKER") == "all", "every guest thread in parallel by default");
    require(value_of(sfr::parse_launcher_settings("parallel=0\n"), "SFR_PARALLEL_WORKER") == "all",
            "an old parallel=0 no longer runs the game on one permit, where it stopped at start");
    require(value_of(settings, "SFR_SKIP_MOVIES").empty(), "movies play by default");
    require(value_of(settings, "SFR_GRAPHICS") == "vulkan", "Vulkan by default");
    require(value_of(settings, "SFR_CAMERA").empty(), "the camera is left alone by default");
    require(value_of(settings, "SFR_PROFILE") == "1", "the player is signed in so the game saves");
    require(value_of(settings, "SFR_GPU_PIPELINE") == "1", "the graphics card works a frame behind by default");
    {
        sfr::LauncherSettings touch;
        touch.touch_controls = false;
        touch.tilt = false;
        touch.language = "zh-TW";
        const auto parsed = sfr::parse_launcher_settings(sfr::format_launcher_settings(touch));
        require(!parsed.touch_controls && !parsed.tilt, "the touch settings survive a save");
        require(parsed.language == "zh-TW", "the launcher language survives a save");
        require(sfr::parse_launcher_settings("language=klingon\n").language == "auto", "an unknown language is the system's");
    }
    {
        // Controls: the first player has the keyboard behind their pad and
        // the second waits for one to be plugged in, which is what this did
        // before any of it could be changed.
        require(value_of(settings, "SFR_PLAYER1_INPUT") == "both" &&
                value_of(settings, "SFR_PLAYER2_INPUT") == "gamepad", "the devices each player starts with");
        require(value_of(settings, "SFR_PLAYER1_KEYS").empty() && value_of(settings, "SFR_PLAYER2_PAD").empty(),
                "and no bindings, so the game uses its own");
        sfr::LauncherSettings controls;
        controls.player2_device = "keyboard";
        controls.player2_keys = "a=G,b=H";
        controls.player1_pad = "a=b,b=a";
        controls.player1_gamepad = "PlayStation controller";
        controls.player2_gamepad = "Controller 2";
        const auto parsed = sfr::parse_launcher_settings(sfr::format_launcher_settings(controls));
        require(parsed.player2_device == "keyboard" && parsed.player2_keys == "a=G,b=H" &&
                parsed.player1_pad == "a=b,b=a", "the controls survive a save");
        require(value_of(controls, "SFR_PLAYER2_KEYS") == "a=G,b=H", "and reach the game");
        require(parsed.player1_gamepad == controls.player1_gamepad && parsed.player2_gamepad == controls.player2_gamepad,
                "each controller choice survives a save");
        require(value_of(controls, "SFR_PLAYER1_GAMEPAD") == controls.player1_gamepad &&
                value_of(controls, "SFR_PLAYER2_GAMEPAD") == controls.player2_gamepad,
                "each controller choice reaches the game");
        require(sfr::parse_launcher_settings("player2_device=mouse\n").player2_device == "gamepad",
                "a device nobody has is ignored");
    }
    require(value_of(settings, "SFR_FULLSCREEN") == "1" && value_of(settings, "SFR_WINDOW_WIDTH") == "1280",
            "borderless by default, with the 1280x720 window size kept for Windowed");
    settings.fullscreen = false;
    require(value_of(settings, "SFR_FULLSCREEN") == "0", "Windowed turns the full screen switch off");
    settings.fullscreen = true;
    settings.window_width = 3440;
    settings.window_height = 1440;
    settings.skip_movies = true;
    settings.audio = false;
    settings.vulkan = true;
    require(value_of(settings, "SFR_GRAPHICS") == "vulkan", "the Vulkan setting selects Vulkan");
    settings.vulkan = false;
    require(value_of(settings, "SFR_GRAPHICS") == "d3d12", "the D3D12 choice is explicit");
    require(value_of(settings, "SFR_FULLSCREEN") == "1" && value_of(settings, "SFR_PARALLEL_WORKER") == "all" &&
            value_of(settings, "SFR_WINDOW_HEIGHT") == "1440" && value_of(settings, "SFR_SKIP_MOVIES") == "1" &&
            value_of(settings, "SFR_AUDIO") == "0", "the environment carries the player's choices");
    settings.image_directory = "C:/a";
    settings.asset_directory = "C:/b";
    const auto arguments = sfr::game_arguments(settings);
    require(arguments.size() == 3 && arguments[2] == L"--game-region=ntsc-us", "the region follows the directories");
}

void directories_are_found_and_checked() {
    const auto root = std::filesystem::temp_directory_path() / "sfr_launcher_settings_test";
    std::filesystem::remove_all(root);
    const auto launcher = root / "out" / "build" / "host";
    std::filesystem::create_directories(launcher);
    require(!sfr::is_image_directory(root / "out/recomp/image-loader"), "a missing directory is not an image");
    require(sfr::default_image_directory(launcher) == launcher / "game/image", "nothing found falls back to game/image");
    std::filesystem::create_directories(root / "out/recomp/image-loader");
    std::ofstream(root / "out/recomp/image-loader/image.bin") << 'x';
    require(!sfr::is_image_directory(root / "out/recomp/image-loader"), "an image without complete.txt is incomplete");
    std::ofstream(root / "out/recomp/image-loader/complete.txt") << 'x';
    require(sfr::default_image_directory(launcher) == root / "out/recomp/image-loader", "a checkout above is found");
    std::filesystem::create_directories(root / "private/assets");
    std::ofstream(root / "private/assets/default.xex") << 'x';
    require(sfr::default_asset_directory(launcher) == root / "private/assets", "checkout assets are found");
    std::filesystem::create_directories(launcher / "game/assets");
    std::ofstream(launcher / "game/assets/default.xex") << 'x';
    require(sfr::default_asset_directory(launcher) == launcher / "game/assets", "installed files come first");

    require(sfr::find_runtime_root(launcher).empty(), "no checkout, no runtime root");
    std::filesystem::create_directories(root / "tools/XenosRecomp/XenosRecomp");
    std::ofstream(root / "tools/XenosRecomp/XenosRecomp/shader_common.h") << 'x';
    require(sfr::find_runtime_root(launcher).empty(), "the shader translator is needed too");
    std::filesystem::create_directories(root / "out/tools/shader-translator");
    std::ofstream(root / "out/tools/shader-translator/shader_translate.exe") << 'x';
    require(sfr::find_runtime_root(launcher) == root, "the checkout above the launcher is the runtime root");

    // An extracted release can live under a development checkout. Its own
    // shader pack must win over an unrelated ancestor's cache and tools.
    std::ofstream(launcher / "shaders.pack") << "bundled pack";
    require(sfr::find_runtime_root(launcher).empty(),
            "a bundled shader pack keeps the release independent of an ancestor checkout");
    std::filesystem::remove(launcher / "shaders.pack");
    require(sfr::find_runtime_root(launcher) == root, "unbundled development builds still find the checkout");

    const auto file = root / "settings.ini";
    sfr::LauncherSettings settings;
    settings.volume = 7;
    require(sfr::save_launcher_settings(file, settings), "settings save");
    settings.volume = 8;
    require(sfr::save_launcher_settings(file, settings), "settings save over an existing file");
    require(sfr::load_launcher_settings(file).volume == 8, "saved settings load");
    settings.avatar_model = std::filesystem::path(u8"models/騎士 1.vrm");
    require(sfr::save_launcher_settings(file, settings), "relative avatar settings save");
    const auto avatar_settings = sfr::load_launcher_settings(file);
    require(avatar_settings.avatar_model == root / settings.avatar_model,
            "a loaded relative model is anchored to settings.ini, not the process working directory");
    const auto absolute_avatar = (root / settings.avatar_model).lexically_normal().u8string();
    require(value_of(avatar_settings, "SFR_AVATAR_MODEL") == std::string(absolute_avatar.begin(), absolute_avatar.end()),
            "loaded relative model reaches the game as an absolute path");
    std::filesystem::create_directories(root / "models");
    std::ofstream(root / settings.avatar_model) << "model";
    {
        struct RestoreDirectory {
            std::filesystem::path previous = std::filesystem::current_path();
            ~RestoreDirectory() { std::filesystem::current_path(previous); }
        } restore;
        std::filesystem::current_path(launcher);
        require(!std::filesystem::is_regular_file(settings.avatar_model), "the model is not in the process working directory");
        const auto resolved = sfr::resolved_avatar_model(settings, root);
        require(std::filesystem::is_regular_file(resolved) && resolved == root / settings.avatar_model,
                "the UI validates a typed relative path against the launcher directory");
        require(value_of(settings, "SFR_AVATAR_MODEL", root) == std::string(absolute_avatar.begin(), absolute_avatar.end()),
                "a typed relative model launches from the same absolute path the UI validates");
        settings.avatar_model = resolved;
        require(sfr::resolved_avatar_model(settings, launcher) == resolved, "absolute model selections are unchanged");
        settings.avatar_model.clear();
        require(sfr::resolved_avatar_model(settings, root).empty() && value_of(settings, "SFR_AVATAR_MODEL", root).empty(),
                "clearing the model never resolves to the launcher directory itself");
    }
    require(sfr::load_launcher_settings(root / "missing.ini").volume == 100, "a missing file gives the defaults");
    std::filesystem::remove_all(root);
}
}

int main() {
    try {
        settings_round_trip();
        game_language_settings();
        voice_language_settings();
        graphics_backend_settings();
        kinect_tuning_settings();
        runtime_render_scale();
        rendering_resolution_settings();
        window_size_defaults_to_the_desktop();
        custom_window_size();
        aspect_settings();
        camera_debug_settings();
        motion_trace_settings();
        menu_steady_settings();
        save_directory_settings();
        title_stats_settings();
        avatar_model_settings();
        malformed_values_keep_defaults();
        display_mode_defaults_to_borderless();
        environment_follows_settings();
        directories_are_found_and_checked();
        std::cout << "Launcher settings checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
