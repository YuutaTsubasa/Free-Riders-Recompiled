#include "launcher_settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string value_of(const sfr::LauncherSettings& settings, const std::string& name) {
    for (const auto& [key, value] : sfr::game_environment(settings))
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
    settings.parallel = false;
    settings.race_render_every = 2;
    settings.ui_sounds = false;
    settings.vulkan = true;
    settings.camera = "motion";
    settings.image_directory = std::filesystem::path(u8"C:/遊戲/image");
    settings.asset_directory = "D:/assets";
    const auto read = sfr::parse_launcher_settings(sfr::format_launcher_settings(settings));
    require(read.window_width == 1920 && read.window_height == 1080, "window size survives a round trip");
    require(read.fullscreen && read.vsync && !read.audio && read.volume == 35, "display and sound survive a round trip");
    require(read.skip_movies && !read.vertex_cache && !read.gpu_pipeline && !read.parallel && read.race_render_every == 2 && !read.ui_sounds && read.vulkan,
            "advanced settings survive a round trip");
    require(read.camera == "motion", "the camera choice survives a round trip");
    require(read.image_directory == settings.image_directory && read.asset_directory == settings.asset_directory,
            "non-ASCII directories survive a round trip");
}

void malformed_values_keep_defaults() {
    const auto read = sfr::parse_launcher_settings(
        "window_width=12\nwindow_height=abc\nvolume=101\nfullscreen=maybe\nrace_render_every=9\n"
        "unknown=1\n# vsync=1\nno equals sign\n  audio = 0  \r\ncamera=webcam\n");
    const sfr::LauncherSettings defaults;
    require(read.camera == defaults.camera, "an unknown camera choice keeps the default");
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
    require(value_of(settings, "SFR_PARALLEL_WORKER") == "cores", "parallel guest cores by default");
    require(value_of(settings, "SFR_SKIP_MOVIES").empty(), "movies play by default");
    require(value_of(settings, "SFR_GRAPHICS").empty(), "Direct3D 12 by default");
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
    require(value_of(settings, "SFR_FULLSCREEN") == "0" && value_of(settings, "SFR_WINDOW_WIDTH") == "1280",
            "windowed 1280x720 by default");
    settings.fullscreen = true;
    settings.parallel = false;
    settings.window_height = 1440;
    settings.skip_movies = true;
    settings.audio = false;
    settings.vulkan = true;
    require(value_of(settings, "SFR_GRAPHICS") == "vulkan", "the Vulkan setting selects Vulkan");
    require(value_of(settings, "SFR_FULLSCREEN") == "1" && value_of(settings, "SFR_PARALLEL_WORKER") == "0" &&
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

    const auto file = root / "settings.ini";
    sfr::LauncherSettings settings;
    settings.volume = 7;
    require(sfr::save_launcher_settings(file, settings), "settings save");
    settings.volume = 8;
    require(sfr::save_launcher_settings(file, settings), "settings save over an existing file");
    require(sfr::load_launcher_settings(file).volume == 8, "saved settings load");
    require(sfr::load_launcher_settings(root / "missing.ini").volume == 100, "a missing file gives the defaults");
    std::filesystem::remove_all(root);
}
}

int main() {
    try {
        settings_round_trip();
        malformed_values_keep_defaults();
        environment_follows_settings();
        directories_are_found_and_checked();
        std::cout << "Launcher settings checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
