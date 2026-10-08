#include "mod_loader.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace sfr {
namespace fs = std::filesystem;
namespace {
// A small ini reader: [Section] then key=value lines; quotes around a value
// are dropped; keys and sections match without regard to case.
struct Ini {
    std::map<std::string, std::map<std::string, std::string>> sections;
    static std::string lower(std::string text) {
        for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    static std::string trim(std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }
    bool read(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::string line, section;
        bool first = true;
        while (std::getline(in, line)) {
            if (first && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
                line.erase(0, 3);  // UTF-8 byte order mark
            first = false;
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = lower(trim(line.substr(1, line.size() - 2)));
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            std::string value = trim(line.substr(equals + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            sections[section][lower(trim(line.substr(0, equals)))] = value;
        }
        return true;
    }
    std::string get(const std::string& section, const std::string& key, std::string fallback = {}) const {
        const auto s = sections.find(lower(section));
        if (s == sections.end()) return fallback;
        const auto k = s->second.find(lower(key));
        return k == s->second.end() ? fallback : k->second;
    }
    size_t number(const std::string& section, const std::string& key) const {
        return size_t(std::strtoull(get(section, key, "0").c_str(), nullptr, 10));
    }
};

fs::path utf8_path(const std::string& text) {
    std::string normal = text;
    std::replace(normal.begin(), normal.end(), '\\', '/');
    return fs::path(std::u8string(normal.begin(), normal.end()));
}

std::string key_of(std::string_view relative) {
    std::string key(relative);
    for (auto& c : key) {
        if (c == '\\') c = '/';
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!key.empty() && key.front() == '/') key.erase(0, 1);
    return key;
}

std::vector<fs::path> include_folders(const fs::path& mod_ini) {
    Ini ini;
    std::vector<fs::path> folders;
    if (!ini.read(mod_ini)) return folders;
    const size_t count = ini.number("Main", "IncludeDirCount");
    for (size_t i = 0; i < count; ++i) {
        const std::string folder = ini.get("Main", "IncludeDir" + std::to_string(i));
        if (!folder.empty()) folders.push_back((mod_ini.parent_path() / utf8_path(folder)).lexically_normal());
    }
    if (folders.empty()) folders.push_back(mod_ini.parent_path());
    return folders;
}

fs::path configuration_file() {
    if (const char* setting = std::getenv("SFR_MODS_INI"); setting && *setting) return utf8_path(setting);
    return "cpkredir.ini";
}

struct Index {
    std::unordered_map<std::string, fs::path> files;
    std::once_flag built;
};
Index& index() {
    static Index i;
    return i;
}

// cpkredir.ini's ModsDbIni. HedgeModManager writes it relative to the game's
// folder (mods\ModsDB.ini), the folder cpkredir.ini is in.
fs::path database_of(const Ini& redirect, const fs::path& config) {
    const std::string text = redirect.get("CPKREDIR", "ModsDbIni");
    if (text.empty()) return {};
    const fs::path db = utf8_path(text);
    return db.is_absolute() ? db : config.parent_path() / db;
}

void build_index() {
    auto& files = index().files;
    Ini redirect;
    const fs::path config = configuration_file();
    if (!redirect.read(config)) return;
    if (Ini::lower(redirect.get("CPKREDIR", "Enabled", "1")) == "0" ||
        Ini::lower(redirect.get("CPKREDIR", "Enabled", "1")) == "false")
        return;
    Ini database;
    const fs::path database_path = database_of(redirect, config);
    if (database_path.empty() || !database.read(database_path)) {
        const auto text = database_path.u8string();
        std::cerr << "MODS database=" << std::string(text.begin(), text.end()) << " readable=0\n";
        return;
    }
    const size_t active = database.number("Main", "ActiveModCount");
    for (size_t i = 0; i < active; ++i) {
        const std::string id = database.get("Main", "ActiveMod" + std::to_string(i));
        const std::string ini = id.empty() ? std::string() : database.get("Mods", id);
        if (ini.empty()) continue;
        size_t added = 0;
        for (const auto& folder : include_folders(utf8_path(ini))) {
            std::error_code error;
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error)) {
                if (!it->is_regular_file(error)) continue;
                const auto relative = fs::relative(it->path(), folder, error);
                if (error) continue;
                const auto text = relative.generic_u8string();
                const std::string key = key_of(std::string(text.begin(), text.end()));
                // An earlier mod (higher in the list) keeps its file.
                if (key != "mod.ini" && files.emplace(key, it->path()).second) ++added;
            }
        }
        std::cerr << "MODS mod=" << id << " files=" << added << '\n';
    }
    std::cerr << "MODS active=" << active << " files=" << files.size() << '\n';
}
}

std::optional<fs::path> mod_file(std::string_view relative) {
    auto& i = index();
    std::call_once(i.built, build_index);
    if (i.files.empty()) return std::nullopt;
    const auto found = i.files.find(key_of(relative));
    if (found == i.files.end()) return std::nullopt;
    // Each replaced file once, for a player checking that a mod applies.
    static std::mutex logged_mutex;
    static std::set<std::string> logged;
    std::lock_guard lock(logged_mutex);
    if (logged.insert(found->first).second) {
        const auto text = found->second.u8string();
        std::cerr << "MODS_FILE game=" << found->first << " from=" << std::string(text.begin(), text.end()) << '\n';
    }
    return found->second;
}

size_t mod_file_count() {
    auto& i = index();
    std::call_once(i.built, build_index);
    return i.files.size();
}

ModList scan_mods(const fs::path& launcher_directory) {
    ModList list;
    list.folder = launcher_directory / "mods";
    const fs::path own_db = list.folder / "ModsDB.ini";
    Ini redirect;
    if (redirect.read(launcher_directory / "cpkredir.ini")) {
        const fs::path db = database_of(redirect, launcher_directory / "cpkredir.ini");
        std::error_code error;
        // HedgeModManager's own (it adds a [HedgeModManager] section) is its
        // even when it uses mods\ModsDB.ini: its ids are not folder names.
        if (!db.empty() && (redirect.sections.count("hedgemodmanager") || !fs::equivalent(db, own_db, error))) {
            list.external = true;
            list.external_db = db;
        }
    }
    Ini database;
    database.read(own_db);
    std::vector<std::string> order;
    for (size_t i = 0, n = database.number("Main", "ActiveModCount"); i < n; ++i)
        order.push_back(database.get("Main", "ActiveMod" + std::to_string(i)));
    std::error_code error;
    for (fs::directory_iterator it(list.folder, error), end; !error && it != end; it.increment(error)) {
        const fs::path ini = it->path() / "mod.ini";
        if (!fs::is_regular_file(ini, error)) continue;
        Ini mod;
        mod.read(ini);
        ModEntry entry;
        const auto name = it->path().filename().u8string();
        entry.id = std::string(name.begin(), name.end());
        entry.title = mod.get("Desc", "Title", entry.id);
        entry.author = mod.get("Desc", "Author");
        entry.version = mod.get("Desc", "Version");
        entry.description = mod.get("Desc", "Description");
        entry.ini = ini;
        entry.enabled = std::find(order.begin(), order.end(), entry.id) != order.end();
        list.mods.push_back(std::move(entry));
    }
    // Enabled mods in their saved order first, then the others by name.
    std::stable_sort(list.mods.begin(), list.mods.end(), [&](const ModEntry& a, const ModEntry& b) {
        const auto pa = std::find(order.begin(), order.end(), a.id), pb = std::find(order.begin(), order.end(), b.id);
        if (pa != pb) return pa < pb;
        return a.title < b.title;
    });
    return list;
}

bool save_mods(const fs::path& launcher_directory, const ModList& list) {
    if (list.external) return false;
    std::error_code error;
    fs::create_directories(list.folder, error);
    const fs::path db = list.folder / "ModsDB.ini";
    std::ofstream out(db, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::vector<const ModEntry*> enabled;
    for (const auto& mod : list.mods)
        if (mod.enabled) enabled.push_back(&mod);
    out << "[Main]\nActiveModCount=" << enabled.size() << '\n';
    for (size_t i = 0; i < enabled.size(); ++i) out << "ActiveMod" << i << '=' << enabled[i]->id << '\n';
    out << "\n[Mods]\n";
    for (const auto* mod : enabled) {
        const auto path = mod->ini.u8string();
        out << mod->id << "=\"" << std::string(path.begin(), path.end()) << "\"\n";
    }
    std::ofstream redirect(launcher_directory / "cpkredir.ini", std::ios::binary | std::ios::trunc);
    if (!redirect) return false;
    const auto db_text = fs::absolute(db, error).u8string();
    redirect << "[CPKREDIR]\nEnabled=" << (enabled.empty() ? 0 : 1) << "\nModsDbIni=\""
             << std::string(db_text.begin(), db_text.end()) << "\"\n";
    return bool(out) && bool(redirect);
}
}
