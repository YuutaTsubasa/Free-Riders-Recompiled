#pragma once
#include "launcher_settings.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImFont;

namespace sfr::launcher {
// What the launcher does differently per platform: Windows
// (launcher_platform_win32.cpp) and SDL on Linux and Android
// (launcher_platform_sdl.cpp). The pages themselves are shared.

// Where settings.ini, game/, save/ and game.log live: beside the launcher,
// or the app's external files directory on Android.
std::filesystem::path launcher_directory();

// A disc image file, or with folders a folder. Android picks only files
// (through the system's document picker; the result reads like a path).
std::optional<std::filesystem::path> pick_path(const std::filesystem::path& start, bool folders);
bool can_pick_folders();
// How a picked source is shown (the document's name on Android).
std::string source_name(const std::filesystem::path& source);
// Copies a picked file (on Android through its open descriptor: reopening
// the document by path is refused). Replaces the destination.
bool copy_picked_file(const std::filesystem::path& from, const std::filesystem::path& to);

// The game, started with the launcher's settings; its trace goes to log.
class GameProcess {
public:
    virtual ~GameProcess() = default;
    // Null while it runs.
    virtual std::optional<uint32_t> exit_code() = 0;
};
std::unique_ptr<GameProcess> start_game(const LauncherSettings& settings, const std::filesystem::path& directory,
                                        const std::filesystem::path& log);
// The game program beside the launcher; empty when it is part of the app.
std::filesystem::path game_program(const std::filesystem::path& directory);
// Whether the launcher should close when the player closes the game
// (desktop), or come back (Android, where the game is another activity).
bool quit_with_game();

bool can_open_log();
void open_log(const std::filesystem::path& log);

// Font files, first existing one wins: Latin regular, semibold and a heavy
// italic for the title; Chinese regular and bold.
struct FontFiles {
    std::vector<std::filesystem::path> body, heading, title, chinese, chinese_bold;
};
FontFiles font_files();

// 1 for Traditional Chinese, else 0 (English).
int ui_language();

#ifndef _WIN32
// Each SDL event, for the platform's own bookkeeping (Android: the launcher
// coming back in front after the game).
void handle_event(const void* sdl_event);
#endif

#ifdef __ANDROID__
// Android's CJK fonts are variable CFF2 fonts, which ImGui's rasterizer
// cannot read: those characters are drawn by Android's own text renderer.
// Add them to a font before the atlas is built, draw them in after.
void add_system_glyphs(ImFont* font, float pixel_size, bool bold, const std::vector<unsigned int>& codepoints);
void draw_system_glyphs();
#endif

#ifdef _WIN32
// The launcher window owns the file dialogs; the main loop waits on the game.
void set_owner_window(void* window);
void* wait_handle(GameProcess& game);
#endif
}
