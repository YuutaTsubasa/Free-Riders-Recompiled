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

void rendering_resolution_settings() {
    const sfr::LauncherSettings defaults;
    require(defaults.render_scale == 100, "the render scale defaults to 100 percent");
    require(sfr::format_launcher_settings(defaults).find("render_scale=100\n") != std::string::npos,
            "internal rendering defaults to native 1280x720");
    require(value_of(defaults, "SFR_RENDER_SCALE") == "100",
            "native rendering explicitly overrides an inherited render scale");
    require(value_of(sfr::parse_launcher_settings("window_width=1920\nwindow_height=1080\n"),
                     "SFR_RENDER_SCALE") == "100",
            "older settings keep native rendering regardless of output window size");

    for (const char* scale : {"50", "75", "100", "150", "200"}) {
        const auto settings = sfr::parse_launcher_settings(
            std::string("window_width=1920\nwindow_height=1080\nrender_scale=") + scale + "\n");
        const auto saved = sfr::format_launcher_settings(settings);
        require(saved.find(std::string("render_scale=") + scale + "\n") != std::string::npos,
                "each supported internal resolution is serialized");
        const auto reloaded = sfr::parse_launcher_settings(saved);
        require(value_of(reloaded, "SFR_RENDER_SCALE") == scale,
                "each internal resolution survives a round trip and overrides the game environment");
        require(reloaded.window_width == 1920 && reloaded.window_height == 1080 &&
                value_of(reloaded, "SFR_WINDOW_WIDTH") == "1920" &&
                value_of(reloaded, "SFR_WINDOW_HEIGHT") == "1080",
                "internal rendering does not change the output window size");
    }
    for (const char* scale : {"", "abc", "0", "49", "51", "99", "101", "125", "201",
                              "-50", "+50", "50.0", "50junk", "4294967296"}) {
        const auto settings = sfr::parse_launcher_settings(std::string("render_scale=") + scale + "\n");
        require(value_of(settings, "SFR_RENDER_SCALE") == "100",
                "malformed or unsupported internal resolutions fall back to native rendering");
    }
    require(value_of(sfr::parse_launcher_settings("render_scale=50\nrender_scale=125\n"),
                     "SFR_RENDER_SCALE") == "100",
            "an invalid later render scale resets to native rendering");
    sfr::LauncherSettings invalid;
    invalid.render_scale = 125;
    require(sfr::format_launcher_settings(invalid).find("render_scale=100\n") != std::string::npos &&
            value_of(invalid, "SFR_RENDER_SCALE") == "100",
            "unsupported in-memory settings serialize and launch at native resolution");
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
    const auto missing = sfr::parse_launcher_settings("avatar_model=Z:/missing/avatar.glb\n");
    require(value_of(missing, "SFR_AVATAR_MODEL") == "Z:/missing/avatar.glb",
            "an unavailable model stays selected so it can be reported instead of silently discarded");
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
    require(value_of(settings, "SFR_FULLSCREEN") == "0" && value_of(settings, "SFR_WINDOW_WIDTH") == "1280",
            "windowed 1280x720 by default");
    settings.fullscreen = true;
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
        rendering_resolution_settings();
        camera_debug_settings();
        avatar_model_settings();
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
