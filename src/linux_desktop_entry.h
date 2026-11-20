#pragma once
// The Linux launcher's entry in the desktop's application menu (Issue #365).
//
// At each start on a Linux desktop the launcher keeps a per-user desktop
// entry (freedesktop Desktop Entry Specification) and its icon:
//   $XDG_DATA_HOME/applications/com.freeriders.recompiled.desktop
//   $XDG_DATA_HOME/icons/hicolor/256x256/apps/com.freeriders.recompiled.png
// ($XDG_DATA_HOME defaults to ~/.local/share). Exec= and Path= are where the
// launcher is now, so a moved folder is followed on its next start; a file
// that already says the same is not written again. The launcher's and the
// game's windows carry the same id as their X11 class and Wayland app ID
// (video_driver.h), so the desktop groups them with the entry.
//
// Only files the launcher wrote are ever replaced or removed: the entry has
// X-FreeRidersRecompiled-Managed=true, the icon a PNG text chunk saying the
// same. General > "Add to application menu" (application_menu in
// settings.ini, on by default) turned off removes them.
//
// Nothing is done inside an AppImage, Flatpak or Snap, which have their own
// desktop integration, nor on Windows, macOS or Android.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sfr::desktop_entry {
// The reverse-DNS id: the entry's file name, the icon's name and the windows'
// class (the macOS bundle and the Android package use it too).
inline constexpr const char* app_id = "com.freeriders.recompiled";
// The key that marks a file as the launcher's own.
inline constexpr const char* managed_key = "X-FreeRidersRecompiled-Managed";
inline constexpr int icon_size = 256;

// One argument of Exec=, quoted as the specification asks when it holds a
// reserved character (inside double quotes ", `, $ and \ get a backslash),
// with % doubled; then escaped as any string value (\ becomes \\).
std::string exec_argument(const std::string& argument);
// A string value (Path=, TryExec=): \ as \\, newline, tab and carriage
// return as \n, \t and \r, and spaces at either end as \s.
std::string string_value(const std::string& value);

// The whole entry for a launcher program and the folder it runs in.
std::string entry_text(const std::filesystem::path& launcher, const std::filesystem::path& directory);
// Whether a desktop entry's text has the marker in its [Desktop Entry] group.
bool is_managed_entry(const std::string& text);

// What to do with a file the launcher keeps, from what is there now
// (nullopt: nothing) and what it would write.
enum class Change { none, write, foreign };
Change change_for(const std::optional<std::string>& existing, const std::string& wanted, bool managed);

// The icon (launcher_art.h's, drawn at icon_size) as a PNG with the marker
// in a tEXt chunk; is_managed_icon looks for that chunk.
std::vector<uint8_t> icon_png(const std::vector<uint32_t>& argb, int size);
bool is_managed_icon(const std::string& bytes);

// $XDG_DATA_HOME when it is an absolute path, else $HOME/.local/share;
// empty when neither is known.
std::filesystem::path data_home(const char* xdg_data_home, const char* home);
std::filesystem::path entry_file(const std::filesystem::path& data_home);
std::filesystem::path icon_file(const std::filesystem::path& data_home);

// Whether the program runs inside a package with its own desktop
// integration: the variables an AppImage, Flatpak or Snap sets, or Flatpak's
// /.flatpak-info.
bool packaged(const char* appimage, const char* flatpak_id, const char* snap, bool flatpak_info);

// What sync did, for the launcher's log line.
enum class Outcome { unchanged, written, removed, foreign, failed, skipped };
const char* outcome_name(Outcome outcome);
struct Result { Outcome entry = Outcome::skipped, icon = Outcome::skipped; };

// Writes (wanted) or removes (!wanted) the entry and icon under data_home,
// touching only the launcher's own files; runs update-desktop-database on
// the applications folder after the entry changed, when it is installed.
Result sync(const std::filesystem::path& data_home, bool wanted, const std::filesystem::path& launcher,
            const std::filesystem::path& directory, const std::vector<uint8_t>& icon);

#if defined(__linux__) && !defined(__ANDROID__)
// The launcher's start and its "Add to application menu" switch: sync for
// this program (/proc/self/exe), from the environment. Skipped when packaged
// or without a home.
Result sync_launcher(bool wanted);
bool available();
#endif
}
