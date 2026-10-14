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
    // 0 = not chosen yet: the launcher fills in the desktop size
    // (apply_default_window_size); the game falls back to 1280x720 without it.
    uint32_t window_width = 0, window_height = 0;
    // The rendering resolution as a percent of the output (window, or the
    // display in full screen) height: 100 draws as many lines as are shown.
    // 0 is Original, the game's own 720 lines whatever the output, which is
    // what every earlier version drew and stays the default: a higher one
    // costs a handheld or a phone its frame rate. The game gets it as
    // SFR_RENDER_SCALE, a percent of its 720 lines (internal_resolution). The
    // old render_scale key (a percent of 1280x720) is converted.
    uint32_t resolution_scale = 0;
    // Not saved: the desktop size the launcher sees (0 = unknown), which full
    // screen shows the game at.
    uint32_t display_width = 0, display_height = 0;
    bool fullscreen = true;  // Borderless; false is Windowed
    // Auto follows the window/display shape; 16:9 keeps the original picture.
    // Neither changes the selected output resolution. Alignment affects the
    // race HUD in Auto and is ignored with 16:9.
    // 16:9 until Auto is right everywhere: at 21:9 the two-player split, its
    // On your Gear screen and the pause menu are still misplaced, and on a
    // phone (about 20:9) Auto draws a quarter more and stretches the touch
    // controls. Auto is the player's choice meanwhile.
    std::string aspect_ratio = "16:9";  // "auto" or "16:9"
    std::string ui_align = "edge";      // "edge" or "center"
    bool vsync = false;
    bool audio = true;
    uint32_t volume = 100;           // percent
    bool skip_movies = false;
    bool vertex_cache = true;        // SFR_VERTEX_CACHE
    bool gpu_pipeline = true;        // SFR_GPU_PIPELINE
    uint32_t race_render_every = 1;  // SFR_RENDER_EVERY
    bool title_stats = true;         // SFR_TITLE_STATS: frame rate and more in the game window's title
    bool ui_sounds = true;           // the launcher's own sounds
    bool vulkan = true;             // legacy settings key; selects Vulkan or D3D12
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
    // A real Kinect's tuning (Issue #97). Smoothing steadies a v2's joints
    // (v1's SDK smooths its own when asked); the lean is the steering a body
    // lean gives, as a percent of the most; the crouch is how far the hips
    // drop for a crouch, in centimetres. The defaults are the game's own.
    bool kinect_smooth = true;      // SFR_KINECT_SMOOTH=0 when off
    uint32_t kinect_lean = 100;     // SFR_KINECT_LEAN_SCALE = 3.5 x percent
    uint32_t kinect_crouch_cm = 15; // SFR_KINECT_CROUCH_DEPTH in metres
    // The menu cursor's steadying (pose_stability.h), webcam and Kinect: the
    // circle a hand held still may wander in without moving the cursor, in
    // millimetres; 0 turns it off. SFR_MENU_STEADY_RADIUS and
    // SFR_MENU_HAND_STEADY.
    uint32_t menu_steady_radius = 8;
    // Voice commands through the host's speech recognizer and the default
    // microphone (a Kinect's array, made the default, is heard), in English
    // or Traditional Chinese; the pad's buttons still say them too.
    bool voice = false;  // SFR_VOICE
    // Windows motion mode: show the skeleton submitted to the game in a
    // separate window, without a camera image (SFR_CAMERA_DEBUG).
    bool camera_debug = false;
    // Optional shared local-player Avatar model. Empty keeps the custom avatar disabled.
    std::filesystem::path avatar_model;
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
    std::string game_language = "auto"; // independent disc language; game_language.h
    std::string voice_language = "auto"; // "en" or "ja" voices apart from the text; voice_language.h
    std::filesystem::path image_directory, asset_directory;
    // Where the game keeps its saves (SFR_SAVE_DIRECTORY): empty is save/
    // beside the launcher; a relative path is from the launcher too.
    std::filesystem::path save_directory;
    // Online play through this project's own server (live_service.h): this
    // device hosts it (SFR_LIVE_HOST) or connects to online_server
    // (SFR_LIVE_SERVER, host[:port]); online_name is the gamertag shown
    // (SFR_LIVE_NAME), which leaves the profile's saves where they are.
    bool online = false, online_host = false;
    std::string online_server, online_name;
};

// Unknown keys and malformed values are ignored (the default stays), so an
// older or hand-edited file still loads.
LauncherSettings parse_launcher_settings(const std::string& text);
std::string format_launcher_settings(const LauncherSettings& settings);
LauncherSettings load_launcher_settings(const std::filesystem::path& file);
bool save_launcher_settings(const std::filesystem::path& file, const LauncherSettings& settings);

// Model paths typed by the player are relative to the launcher, never the
// game's working directory. Empty selections stay empty.
std::filesystem::path resolved_avatar_model(const LauncherSettings& settings,
                                            const std::filesystem::path& launcher_directory);

// The saves' folder for these settings, from the launcher's directory.
std::filesystem::path resolved_save_directory(const LauncherSettings& settings,
                                              const std::filesystem::path& launcher_directory);
// Whether a saves' folder holds a game save (<player>/00000001/SfrAllData.sav/).
bool has_game_save(const std::filesystem::path& save_directory);

// The game's environment for these settings: the play defaults (no
// diagnostic limits or traces, 60 fps cap) plus the player's choices. An
// empty value removes the variable.
std::vector<std::pair<std::string, std::string>> game_environment(const LauncherSettings& settings,
    const std::filesystem::path& launcher_directory = {});
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

// A bundled shaders.pack takes priority and returns no checkout. Otherwise,
// the checkout the game runs in: it translates shaders at run time with
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

// Whether a window size is one the list offers: a common size or the
// desktop's. Anything else is shown as "Custom" and kept as typed.
bool is_listed_window_size(uint32_t width, uint32_t height, uint32_t desktop_width, uint32_t desktop_height);

// Reads a window width or height typed by the player: digits only, at least
// 160 and at most 16384 (a number outside the range is clamped to it). Empty
// or non-numeric text is rejected and leaves value unchanged.
bool parse_window_dimension(const std::string& text, uint32_t& value);

// A window size not chosen yet (0) becomes the desktop's, and is the
// player's choice from then on; the display size is recorded for full screen.
// An unknown desktop leaves 1280x720.
void apply_default_window_size(LauncherSettings& settings, uint32_t desktop_width, uint32_t desktop_height);

// The resolution scale choices: 0 (Original, 720 lines), then percents of
// the output size, 0.25x to 3.0x in steps of 0.25x (25 to 300), in order.
// Anything else reads as Original.
const std::vector<uint32_t>& resolution_scale_choices();

// A choice as the slider shows it: "0.25x", "0.5x", "0.75x", "1.0x", "1.25x".
std::string resolution_scale_label(uint32_t percent);

// What the game draws for these settings: the output it is shown in (the
// window, or the display in full screen), the SFR_RENDER_SCALE percent
// asked for, round(output_height * resolution_scale / 720) within 25..800,
// and the internal size that gives with the game's 1280x720 logical frame,
// lowered as the game lowers it when a side would exceed 8192.
struct InternalResolution {
    uint32_t output_width, output_height;
    uint32_t requested_percent, percent;
    uint32_t logical_width, width, height;
};
InternalResolution internal_resolution(const LauncherSettings& settings);
InternalResolution internal_resolution(const LauncherSettings& settings, uint32_t resolution_scale);
// The aspect ratio choices, in the order the launcher lists them.
const std::vector<std::string>& aspect_ratio_choices();
}
