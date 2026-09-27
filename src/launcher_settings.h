#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace sfr {
// What the launcher asks of the player before the game starts, kept in
// settings.ini next to the launcher. The game reads all of it at startup
// (environment variables and arguments), so it is fixed for a run.
struct LauncherSettings {
    uint32_t window_width = 1280, window_height = 720;
    bool fullscreen = false;
    bool vsync = false;
    bool audio = true;
    uint32_t volume = 100;           // percent
    bool skip_movies = false;
    bool vertex_cache = true;        // SFR_VERTEX_CACHE
    bool gpu_pipeline = true;        // SFR_GPU_PIPELINE
    bool parallel = true;            // SFR_PARALLEL_WORKER=cores (off: serial)
    uint32_t race_render_every = 1;  // SFR_RENDER_EVERY
    bool ui_sounds = true;           // the launcher's own sounds
    bool vulkan = false;             // SFR_GRAPHICS=vulkan instead of Direct3D 12
    bool touch_controls = true;      // SFR_TOUCH_CONTROLS (Android)
    bool tilt = true;                // SFR_TILT: tilt to steer (Android)
    // Where the Kinect player's body comes from, and whether the camera
    // picture the title shows is a real one. "pad" is the emulated player the
    // gamepad drives; "camera" tracks a real body with a webcam, which cannot
    // share the pad's sticks, so the two are one choice. "picture" keeps the
    // pad in charge and only gives the title a camera image.
    // "kinect" is a real Kinect tracking the players, as on the console.
    std::string camera = "off";      // "off", "picture", "motion" or "kinect"
    // Which of the host's cameras, by the name it lists (SFR_CAMERA_DEVICE).
    // A name and not a number, because the list changes: a phone camera or a
    // capture card comes and goes, and the number then means another camera.
    std::string camera_device;
    // Some cameras hand over a mirrored picture (phone-as-webcam apps
    // usually do). Nothing in a picture of a person says which it is, so
    // it is a setting: with it wrong, left and right are the wrong way
    // round and the cursor runs off the side of the screen.
    bool camera_mirror = false;   // SFR_CAMERA_MIRROR
    // Motion only, experimental: the webcam's body races as well, read by
    // the title's own gesture detectors instead of the pad.
    bool camera_race = false;     // SFR_CAMERA_RACE
    // Where a real Kinect stands around the player, the screen ahead:
    // "front", "front-right", "right", "behind-right", "behind",
    // "behind-left", "left" or "front-left" (kinect_sensor.h). A board is
    // ridden side-on, so the side the chest faces sees the whole body.
    std::string kinect_placement = "front";  // SFR_KINECT_PLACEMENT
    // Voice commands through the host's speech recognizer and the default
    // microphone (a Kinect's array, made the default, is heard), in English
    // or Traditional Chinese; the pad's buttons still say them too.
    bool voice = false;  // SFR_VOICE
    // Controls (input_bindings.h). The devices each player uses, and how
    // their keys and pad buttons are arranged, as "a=Z,b=X,..." lines. An
    // empty line means the defaults, so a settings file written before any
    // of this existed plays exactly as it did.
    std::string player1_device = "both";     // "both", "gamepad" or "keyboard"
    std::string player2_device = "gamepad";  // "gamepad", "keyboard" or "off"
    std::string player1_keys, player1_pad, player2_keys, player2_pad;
    // Which controller each player uses, by the name the host lists it under
    // (pad_devices.h). SDL device indices can change when a pad is plugged in.
    // Empty means whichever one the host offers first.
    std::string player1_gamepad, player2_gamepad;
    std::string language = "auto";   // the launcher's: "auto" (the system's), "en" or "zh-TW"
    std::filesystem::path image_directory, asset_directory;
};

// Unknown keys and malformed values are ignored (the default stays), so an
// older or hand-edited file still loads.
LauncherSettings parse_launcher_settings(const std::string& text);
std::string format_launcher_settings(const LauncherSettings& settings);
LauncherSettings load_launcher_settings(const std::filesystem::path& file);
bool save_launcher_settings(const std::filesystem::path& file, const LauncherSettings& settings);

// The game's environment for these settings: the play defaults (no
// diagnostic limits or traces, 60 fps cap) plus the player's choices. An
// empty value removes the variable.
std::vector<std::pair<std::string, std::string>> game_environment(const LauncherSettings& settings);
// Its command line after the program: image directory, asset directory,
// region.
std::vector<std::wstring> game_arguments(const LauncherSettings& settings);

// Whether a directory holds what the game needs: a complete image dump, or
// the disc's files.
bool is_image_directory(const std::filesystem::path& directory);
bool is_asset_directory(const std::filesystem::path& directory);
// Where the files are when the settings do not say: game/image and
// game/assets beside the launcher (what the installer writes), else a
// development checkout above it (out/recomp/image-loader, private/assets).
std::filesystem::path default_image_directory(const std::filesystem::path& launcher_directory);
std::filesystem::path default_asset_directory(const std::filesystem::path& launcher_directory);

// The checkout the game runs in: it translates shaders at run time with
// tools/XenosRecomp/XenosRecomp/shader_common.h and
// out/tools/shader-translator/shader_translate.exe, and caches them in
// out/shaders/runtime, all relative to its working directory. The nearest
// directory at or above the launcher holding the first two; empty when
// there is none.
std::filesystem::path find_runtime_root(const std::filesystem::path& launcher_directory);

// A release's own shader tools (scripts/package_release.py): shader-tools/
// beside the launcher with the translator, the pinned shader_common.h and
// dxc-bin's DXC. The environment that points the game at them and at a
// shader-cache/ beside the launcher; empty when they are not all there.
std::vector<std::pair<std::string, std::filesystem::path>> shader_tool_environment(const std::filesystem::path& launcher_directory);

// Common window sizes offered in the list (the launcher adds the desktop's).
struct WindowSize { uint32_t width, height; };
std::vector<WindowSize> common_window_sizes();
}
