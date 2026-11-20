#include "linux_desktop_entry.h"
#include "image_decode.h"
#include "launcher_art.h"
#define SFR_VIDEO_DRIVER_RULE_ONLY 1
#include "video_driver.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace de = sfr::desktop_entry;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// The value of a key in [Desktop Entry], as written (still escaped).
std::string value_of(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (line.compare(0, key.size() + 1, key + "=") == 0) return line.substr(key.size() + 1);
    return {};
}

// What a desktop reads from an Exec value: the string escapes first, then
// the quoting of the argument (the specification's two levels).
std::string read_exec(const std::string& value) {
    std::string unescaped;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            unescaped += next == 'n' ? '\n' : next == 't' ? '\t' : next == 'r' ? '\r' : next == 's' ? ' ' : next;
        } else {
            unescaped += value[i];
        }
    }
    std::string argument;
    const bool quoted = unescaped.size() >= 2 && unescaped.front() == '"' && unescaped.back() == '"';
    const std::string body = quoted ? unescaped.substr(1, unescaped.size() - 2) : unescaped;
    for (size_t i = 0; i < body.size(); ++i) {
        if (quoted && body[i] == '\\' && i + 1 < body.size()) {
            argument += body[++i];
        } else if (body[i] == '%' && i + 1 < body.size() && body[i + 1] == '%') {
            argument += '%';
            ++i;
        } else {
            require(quoted || std::string(" \t\n\"'\\><~|&;$*?#()`").find(body[i]) == std::string::npos,
                    "an unquoted argument holds no reserved character");
            argument += body[i];
        }
    }
    return argument;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void exec_quoting() {
    require(de::exec_argument("/opt/sfr/FreeRidersRecompiled") == "/opt/sfr/FreeRidersRecompiled",
            "a plain path is written as it is");
    require(de::exec_argument("/home/me/My Games/FreeRidersRecompiled") == "\"/home/me/My Games/FreeRidersRecompiled\"",
            "a path with a space is quoted");
    require(de::exec_argument("/games/it's") == "\"/games/it's\"", "a single quote is quoted, not escaped");
    require(de::exec_argument("/a/$x") == "\"/a/\\\\$x\"",
            "a dollar sign is quoted with a backslash, and the backslash escaped again as a string");
    require(de::exec_argument("/a/b\\c") == "\"/a/b\\\\\\\\c\"", "a backslash is four in the file");
    require(de::exec_argument("/100%/x") == "/100%%/x", "a percent sign is doubled, not a field code");
    for (const std::string path : {"/home/me/My Games/FreeRidersRecompiled", "/a b/$x\"y\\z`w", "/c/100% (copy)/run",
                                   "/x/new\nline\ttab", "/plain/path", "/~user/;&|<>*?#"})
        require(read_exec(de::exec_argument(path)) == path, "a desktop reads back the very path");
    require(de::string_value("/a\\b\nc\td") == "/a\\\\b\\nc\\td", "string values escape \\, newline and tab");
    require(de::string_value("/games/Free Riders ") == "/games/Free Riders\\s" && de::string_value(" x") == "\\sx" &&
                de::string_value("a  b") == "a  b",
            "spaces at either end are \\s, inside they stay");
    require(read_exec(de::exec_argument("/games/Free Riders /run")) == "/games/Free Riders /run" &&
                de::entry_text("/g/x /run", "/g/x ").find("\nPath=/g/x\\s\n") != std::string::npos,
            "a folder ending in a space reads back whole");
}

void entry_contents() {
    const std::string text = de::entry_text("/home/me/Free Riders/FreeRidersRecompiled", "/home/me/Free Riders");
    require(text.find("[Desktop Entry]\n") != std::string::npos, "the entry's group");
    require(value_of(text, "Type") == "Application" && value_of(text, "Name") == "Free Riders Recompiled",
            "an application named Free Riders Recompiled");
    require(value_of(text, "Exec") == "\"/home/me/Free Riders/FreeRidersRecompiled\"" &&
                value_of(text, "TryExec") == "/home/me/Free Riders/FreeRidersRecompiled" &&
                value_of(text, "Path") == "/home/me/Free Riders",
            "Exec quoted, TryExec and Path plain");
    require(value_of(text, "Icon") == de::app_id && value_of(text, "StartupWMClass") == de::app_id,
            "the icon and the window class are the app id");
    require(value_of(text, "Categories") == "Game;" && value_of(text, "Terminal") == "false", "a game, no terminal");
    require(de::is_managed_entry(text), "the launcher's entry is marked as its own");
    const char* name = sfr::linux_window_class(nullptr, "", nullptr);
    require(name && std::string(name) == de::app_id, "the windows carry the entry's id as their class");
    require(!sfr::linux_window_class("Mine", nullptr, nullptr) && !sfr::linux_window_class(nullptr, "mine", nullptr) &&
                !sfr::linux_window_class(nullptr, nullptr, "org.mine"),
            "a class the player set is left alone");
}

void managed_marker() {
    require(!de::is_managed_entry("[Desktop Entry]\nName=Mine\nExec=mine\n"), "an entry without the marker is not ours");
    require(!de::is_managed_entry("[Desktop Entry]\nX-FreeRidersRecompiled-Managed=false\n"), "false is not ours");
    require(!de::is_managed_entry("[Desktop Entry]\nName=Mine\n[Desktop Action x]\nX-FreeRidersRecompiled-Managed=true\n"),
            "the marker counts only in [Desktop Entry]");
    require(!de::is_managed_entry("# X-FreeRidersRecompiled-Managed=true\n[Desktop Entry]\n"), "nor in a comment");
    require(de::is_managed_entry("[Desktop Entry]\r\nX-FreeRidersRecompiled-Managed = true\r\n"),
            "spaces around = and CRLF line ends are read");
    require(de::change_for(std::nullopt, "a", false) == de::Change::write, "a missing file is written");
    require(de::change_for(std::string("a"), "a", true) == de::Change::none, "the same text is not written again");
    require(de::change_for(std::string("old"), "a", true) == de::Change::write, "our file that differs is updated");
    require(de::change_for(std::string("theirs"), "a", false) == de::Change::foreign, "someone else's file is left");
}

void icon_file() {
    const auto pixels = sfr::render_icon(de::icon_size);
    const auto png = de::icon_png(pixels, de::icon_size);
    std::string error;
    const auto decoded = sfr::decode_png(png, &error);
    require(bool(decoded) && decoded.width == uint32_t(de::icon_size) && decoded.height == uint32_t(de::icon_size),
            "the icon is a PNG of icon_size pixels a side");
    bool same = true;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t p = pixels[i];
        const uint8_t* q = &decoded.rgba[i * 4];
        same &= q[0] == uint8_t(p >> 16) && q[1] == uint8_t(p >> 8) && q[2] == uint8_t(p) && q[3] == uint8_t(p >> 24);
    }
    require(same, "its pixels are the launcher's icon");
    require(de::is_managed_icon(std::string(png.begin(), png.end())), "the icon is marked as the launcher's");
    require(!de::is_managed_icon("\x89PNG\r\n\x1a\nsomething else"), "another PNG is not");
}

void paths() {
    require(de::data_home("/x/data", "/home/me") == fs::path("/x/data"), "XDG_DATA_HOME when absolute");
    require(de::data_home("relative", "/home/me") == fs::path("/home/me") / ".local" / "share" &&
                de::data_home("", "/home/me") == fs::path("/home/me") / ".local" / "share" &&
                de::data_home(nullptr, "/home/me") == fs::path("/home/me") / ".local" / "share",
            "else ~/.local/share");
    require(de::data_home(nullptr, nullptr).empty() && de::data_home(nullptr, "").empty(), "nothing without a home");
    require(de::entry_file("/d") == fs::path("/d") / "applications" / "com.freeriders.recompiled.desktop",
            "the entry's file");
    require(de::icon_file("/d") == fs::path("/d") / "icons" / "hicolor" / "256x256" / "apps" / "com.freeriders.recompiled.png",
            "the icon's file");
    require(!de::packaged(nullptr, nullptr, nullptr, false) && !de::packaged("", "", "", false), "a plain folder");
    require(de::packaged("/x.AppImage", nullptr, nullptr, false) && de::packaged(nullptr, "org.x", nullptr, false) &&
                de::packaged(nullptr, nullptr, "/snap/x", false) && de::packaged(nullptr, nullptr, nullptr, true),
            "AppImage, Flatpak and Snap integrate themselves");
}

void sync_files() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path home = fs::temp_directory_path() / ("sfr_desktop_entry_" + std::to_string(stamp));
    fs::create_directories(home);
    const fs::path entry = de::entry_file(home), icon = de::icon_file(home);
    const auto png = de::icon_png(sfr::render_icon(de::icon_size), de::icon_size);
    using O = de::Outcome;

    // A file of the player's beside the entry, named as a temporary might be.
    fs::create_directories(entry.parent_path());
    const fs::path beside = entry.string() + ".new";
    { std::ofstream(beside, std::ios::binary) << "mine"; }
    auto result = de::sync(home, true, "/opt/a b/FreeRidersRecompiled", "/opt/a b", png);
    require(result.entry == O::written && result.icon == O::written, "the first start writes both");
    // (update-desktop-database, where installed, adds its mimeinfo.cache.)
    size_t temporaries = 0;
    for (const auto& item : fs::directory_iterator(entry.parent_path()))
        temporaries += item.path().extension() == ".tmp";
    require(fs::is_regular_file(entry) && fs::is_regular_file(icon) && !temporaries && read_file(beside) == "mine",
            "both are in place, no temporary left, the file beside untouched");
    fs::remove(beside);
    require(value_of(read_file(entry), "Exec") == "\"/opt/a b/FreeRidersRecompiled\"", "Exec is the launcher");

    result = de::sync(home, true, "/opt/a b/FreeRidersRecompiled", "/opt/a b", png);
    require(result.entry == O::unchanged && result.icon == O::unchanged, "an unchanged launcher writes nothing");

    result = de::sync(home, true, "/srv/moved/FreeRidersRecompiled", "/srv/moved", png);
    require(result.entry == O::written && result.icon == O::unchanged, "a moved launcher updates the entry only");
    require(value_of(read_file(entry), "Exec") == "/srv/moved/FreeRidersRecompiled" &&
                value_of(read_file(entry), "Path") == "/srv/moved",
            "Exec and Path follow the launcher");

    result = de::sync(home, false, {}, {}, png);
    require(result.entry == O::removed && result.icon == O::removed && !fs::exists(entry) && !fs::exists(icon),
            "off removes the launcher's own files");
    result = de::sync(home, false, {}, {}, png);
    require(result.entry == O::unchanged && result.icon == O::unchanged, "off with nothing there does nothing");

    // Files of the same names someone else wrote (a package, the player):
    // never replaced, never removed.
    const std::string theirs = "[Desktop Entry]\nType=Application\nName=Mine\nExec=/usr/bin/mine\n";
    { std::ofstream(entry, std::ios::binary) << theirs; }
    fs::create_directories(icon.parent_path());
    { std::ofstream(icon, std::ios::binary) << "\x89PNG\r\n\x1a\nnot ours"; }
    result = de::sync(home, true, "/opt/x/FreeRidersRecompiled", "/opt/x", png);
    require(result.entry == O::foreign && result.icon == O::foreign && read_file(entry) == theirs,
            "someone else's files are not replaced");
    result = de::sync(home, false, {}, {}, png);
    require(result.entry == O::foreign && result.icon == O::foreign && read_file(entry) == theirs && fs::exists(icon),
            "nor removed");

    // A folder where the entry goes is not written over (nor reported written).
    fs::remove(entry);
    fs::create_directories(entry);
    result = de::sync(home, true, "/opt/x/FreeRidersRecompiled", "/opt/x", png);
    require(result.entry == O::foreign && fs::is_directory(entry), "a folder in the way is left alone");
    require(de::sync(home, false, {}, {}, png).entry == O::foreign && fs::is_directory(entry), "and not removed");

    require(de::sync({}, true, "/opt/x/FreeRidersRecompiled", "/opt/x", png).entry == O::skipped, "no home, nothing");
    std::error_code error;
    fs::remove_all(home, error);
}
}

int main() {
    try {
        exec_quoting();
        entry_contents();
        managed_marker();
        icon_file();
        paths();
        sync_files();
    } catch (const std::exception& error) {
        std::cerr << "linux_desktop_entry_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "linux_desktop_entry_test: ok\n";
    return 0;
}
