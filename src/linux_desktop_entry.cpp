#include "linux_desktop_entry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#if defined(__linux__) && !defined(__ANDROID__)
#include "launcher_art.h"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace sfr::desktop_entry {
namespace {
// The Exec key's reserved characters: an argument holding one is quoted.
bool reserved(char c) {
    return std::strchr(" \t\n\"'\\><~|&;$*?#()`", c) != nullptr && c != '\0';
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Desktop entries are UTF-8; a path that is not cannot be written into one.
bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto c = uint8_t(text[i]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!length || i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((uint8_t(text[i + k]) >> 6) != 2) return false;
        i += length;
    }
    return true;
}

std::string trimmed(std::string text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && space(text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && space(text[start])) ++start;
    return text.substr(start);
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ bytes[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

// The icon's tEXt chunk: keyword, a zero, the text.
const std::string& icon_marker() {
    static const std::string marker = std::string("Software") + '\0' + "Free Riders Recompiled launcher; " +
                                      managed_key + "=true";
    return marker;
}

// Whether anything is at the path (a file, a folder, even a broken link).
bool present(const fs::path& file) {
    std::error_code error;
    return fs::symlink_status(file, error).type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory);
}

// The file's bytes; nullopt when it is missing or cannot be read. Callers
// treat something present but unreadable as not the launcher's.
std::optional<std::string> read_file(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

// Written beside the file and renamed over it, so a menu never reads half a file.
bool write_file(const fs::path& file, const std::string& bytes) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    // A name nothing else uses: a file the player keeps beside it is never
    // truncated.
    fs::path temporary;
    const auto seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint64_t attempt = 0; attempt < 100 && temporary.empty(); ++attempt) {
        fs::path candidate = file;
        candidate += ".sfr-" + std::to_string(seed + attempt) + ".tmp";
        if (!present(candidate)) temporary = candidate;
    }
    if (temporary.empty()) return false;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), std::streamsize(bytes.size()));
        if (!out.flush()) {
            out.close();
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, file, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

Outcome keep(const fs::path& file, const std::string& wanted, bool (*managed)(const std::string&)) {
    const auto existing = read_file(file);
    if (!existing && present(file)) return Outcome::foreign;  // a folder, or a file it cannot read
    switch (change_for(existing, wanted, existing && managed(*existing))) {
    case Change::none: return Outcome::unchanged;
    case Change::foreign: return Outcome::foreign;
    case Change::write: break;
    }
    return write_file(file, wanted) ? Outcome::written : Outcome::failed;
}

Outcome drop(const fs::path& file, bool (*managed)(const std::string&)) {
    const auto existing = read_file(file);
    if (!existing) return present(file) ? Outcome::foreign : Outcome::unchanged;
    if (!managed(*existing)) return Outcome::foreign;
    std::error_code error;
    return fs::remove(file, error) && !error ? Outcome::removed : Outcome::failed;
}

bool managed_text(const std::string& text) { return is_managed_entry(text); }
bool managed_icon(const std::string& bytes) { return is_managed_icon(bytes); }

// update-desktop-database, when installed: the menu's MIME cache. Only some
// desktops need it, and a missing program is no failure.
void update_database(const fs::path& applications) {
#if defined(__linux__) && !defined(__ANDROID__)
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return;
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const std::string folder = applications.string();
    char program[] = "update-desktop-database";
    char* arguments[] = {program, const_cast<char*>(folder.c_str()), nullptr};
    pid_t child = 0;
    if (posix_spawnp(&child, program, &actions, nullptr, arguments, environ) == 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    posix_spawn_file_actions_destroy(&actions);
#else
    (void)applications;
#endif
}
}

std::string exec_argument(const std::string& argument) {
    bool quote = argument.empty();
    for (const char c : argument) quote |= reserved(c);
    std::string out;
    if (quote) out += '"';
    for (const char c : argument) {
        if (quote && (c == '"' || c == '`' || c == '$' || c == '\\')) out += '\\';
        if (c == '%') out += '%';
        out += c;
    }
    if (quote) out += '"';
    return string_value(out);
}

std::string string_value(const std::string& value) {
    // Spaces at either end as \s: readers drop unescaped ones there, and a
    // folder may well end in one.
    const size_t first = value.find_first_not_of(' ');
    const size_t last = value.find_last_not_of(' ');
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == ' ' && (first == std::string::npos || i < first || i > last)) {
            out += "\\s";
            continue;
        }
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

std::string entry_text(const fs::path& launcher, const fs::path& directory) {
    const std::string program = utf8(launcher);
    std::ostringstream out;
    out << "# Written by the Free Riders Recompiled launcher, which keeps it pointing at\n"
           "# where the launcher is. To remove it, turn off General > Add to application\n"
           "# menu in the launcher (or delete this file and the icon of the same name).\n"
           "[Desktop Entry]\n"
           "Type=Application\n"
           // The newest desktop-file-validate 0.26 (Ubuntu 22.04) knows;
           // nothing written here needs 1.5.
           "Version=1.4\n"
           "Name=Free Riders Recompiled\n"
           "GenericName=Racing Game\n"
           "GenericName[zh_TW]=賽車遊戲\n"
           "Comment=Sonic Free Riders, recompiled to run on this computer\n"
           "Comment[zh_TW]=在這台電腦上執行的 Sonic Free Riders 重編譯版\n"
        << "Icon=" << app_id << '\n'
        << "TryExec=" << string_value(program) << '\n'
        << "Exec=" << exec_argument(program) << '\n'
        << "Path=" << string_value(utf8(directory)) << '\n'
        << "Terminal=false\n"
           "Categories=Game;\n"
           "Keywords=Sonic;Free Riders;Kinect;\n"
        << "StartupWMClass=" << app_id << '\n'
        << managed_key << "=true\n";
    return out.str();
}

bool is_managed_entry(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    bool in_group = false;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            in_group = line == "[Desktop Entry]";
            continue;
        }
        const size_t equals = line.find('=');
        if (in_group && equals != std::string::npos && trimmed(line.substr(0, equals)) == managed_key)
            return trimmed(line.substr(equals + 1)) == "true";
    }
    return false;
}

Change change_for(const std::optional<std::string>& existing, const std::string& wanted, bool managed) {
    if (!existing) return Change::write;
    if (!managed) return Change::foreign;
    return *existing == wanted ? Change::none : Change::write;
}

std::vector<uint8_t> icon_png(const std::vector<uint32_t>& argb, int size) {
    std::vector<uint8_t> out{137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    std::vector<uint8_t> header;
    put32(header, uint32_t(size));
    put32(header, uint32_t(size));
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace
    chunk(out, "IHDR", header);
    chunk(out, "tEXt", std::vector<uint8_t>(icon_marker().begin(), icon_marker().end()));
    // Rows of filter 0 and RGBA, in stored (uncompressed) DEFLATE blocks.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(size) * (size_t(size) * 4 + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0);
        for (int x = 0; x < size; ++x) {
            const uint32_t p = argb[size_t(y) * size_t(size) + size_t(x)];
            raw.insert(raw.end(), {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p), uint8_t(p >> 24)});
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t length = std::min<size_t>(raw.size() - at, 65535);
        const bool last = at + length == raw.size();
        z.insert(z.end(), {uint8_t(last), uint8_t(length), uint8_t(length >> 8), uint8_t(~length), uint8_t(~length >> 8)});
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + length));
        at += length;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool is_managed_icon(const std::string& bytes) {
    return bytes.size() > 8 && bytes.compare(0, 4, "\x89PNG") == 0 && bytes.find(icon_marker()) != std::string::npos;
}

fs::path data_home(const char* xdg_data_home, const char* home) {
    // The specification: a relative $XDG_DATA_HOME is invalid and ignored.
    if (xdg_data_home && *xdg_data_home == '/') return fs::path(xdg_data_home);
    if (home && *home == '/') return fs::path(home) / ".local" / "share";
    return {};
}

fs::path entry_file(const fs::path& data_home) {
    return data_home / "applications" / (std::string(app_id) + ".desktop");
}

fs::path icon_file(const fs::path& data_home) {
    const std::string size = std::to_string(icon_size);
    return data_home / "icons" / "hicolor" / (size + "x" + size) / "apps" / (std::string(app_id) + ".png");
}

bool packaged(const char* appimage, const char* flatpak_id, const char* snap, bool flatpak_info) {
    const auto set = [](const char* value) { return value && *value; };
    return set(appimage) || set(flatpak_id) || set(snap) || flatpak_info;
}

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
    case Outcome::unchanged: return "unchanged";
    case Outcome::written: return "written";
    case Outcome::removed: return "removed";
    case Outcome::foreign: return "not_ours";
    case Outcome::failed: return "failed";
    case Outcome::skipped: return "skipped";
    }
    return "?";
}

Result sync(const fs::path& data_home, bool wanted, const fs::path& launcher, const fs::path& directory,
            const std::vector<uint8_t>& icon) {
    Result result;
    if (data_home.empty()) return result;
    const fs::path entry = entry_file(data_home), picture = icon_file(data_home);
    if (wanted) {
        const std::string text = entry_text(launcher, directory);
        result.icon = keep(picture, std::string(icon.begin(), icon.end()), managed_icon);
        result.entry = valid_utf8(text) ? keep(entry, text, managed_text) : Outcome::failed;
    } else {
        result.entry = drop(entry, managed_text);
        result.icon = drop(picture, managed_icon);
    }
    if (result.entry == Outcome::written || result.entry == Outcome::removed) update_database(entry.parent_path());
    return result;
}

#if defined(__linux__) && !defined(__ANDROID__)
bool available() {
    std::error_code error;
    return !packaged(std::getenv("APPIMAGE"), std::getenv("FLATPAK_ID"), std::getenv("SNAP"),
                     fs::exists("/.flatpak-info", error)) &&
           !data_home(std::getenv("XDG_DATA_HOME"), std::getenv("HOME")).empty();
}

Result sync_launcher(bool wanted) {
    if (!available()) return {};
    std::error_code error;
    const fs::path launcher = fs::read_symlink("/proc/self/exe", error);
    // A program replaced while it runs reads as "<path> (deleted)": no entry for it.
    const std::string name = launcher.string();
    if (error || !launcher.is_absolute() ||
        (name.size() > 10 && name.compare(name.size() - 10, 10, " (deleted)") == 0))
        return {};
    return sync(data_home(std::getenv("XDG_DATA_HOME"), std::getenv("HOME")), wanted, launcher,
                launcher.parent_path(), icon_png(render_icon(icon_size), icon_size));
}
#endif
}
