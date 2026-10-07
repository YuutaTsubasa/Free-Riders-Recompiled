#include "mod_loader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
}

int main() {
    try {
        const fs::path root = fs::temp_directory_path() / "sfr_mod_loader_test";
        fs::remove_all(root);
        const fs::path launcher = root / "launcher";
        // Two mods in HedgeModManager's format; "high" is listed first and wins.
        write(launcher / "mods/high/mod.ini", "[Desc]\nTitle=\"High mod\"\nAuthor=Someone\n[Main]\nIncludeDirCount=1\nIncludeDir0=\".\"\n");
        write(launcher / "mods/high/sound/SRN_BGM.csb", "high");
        write(launcher / "mods/low/mod.ini", "[Desc]\nTitle=Low\n[Main]\nIncludeDirCount=1\nIncludeDir0=\"files\"\n");
        write(launcher / "mods/low/files/Sound/srn_bgm.csb", "low");
        write(launcher / "mods/low/files/new/added.bin", "added");
        write(launcher / "mods/off/mod.ini", "[Desc]\nTitle=Off\n");

        auto list = sfr::scan_mods(launcher);
        require(list.mods.size() == 3 && !list.external, "three mods found");
        for (auto& mod : list.mods) mod.enabled = mod.id != "off";
        // Order: high above low (by id: high, low, off).
        std::stable_sort(list.mods.begin(), list.mods.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        require(list.mods[0].id == "high" && list.mods[0].title == "High mod" && list.mods[0].author == "Someone",
                "mod.ini details");
        require(sfr::save_mods(launcher, list), "the launcher writes ModsDB.ini and cpkredir.ini");
        const auto again = sfr::scan_mods(launcher);
        require(again.mods.size() == 3 && again.mods[0].id == "high" && again.mods[0].enabled &&
                again.mods[1].id == "low" && again.mods[1].enabled && !again.mods[2].enabled,
                "the saved order and choices come back");

        const std::string ini = utf8(launcher / "cpkredir.ini");
#ifdef _WIN32
        _putenv_s("SFR_MODS_INI", ini.c_str());
#else
        setenv("SFR_MODS_INI", ini.c_str(), 1);
#endif
        const auto bgm = sfr::mod_file("sound\\SRN_BGM.csb");
        require(bgm && fs::equivalent(*bgm, launcher / "mods/high/sound/SRN_BGM.csb"), "the first mod wins, any case");
        const auto added = sfr::mod_file("NEW/Added.bin");
        require(added && fs::equivalent(*added, launcher / "mods/low/files/new/added.bin"), "a mod can add a file");
        require(!sfr::mod_file("sound/other.csb"), "other files stay the game's");
        require(!sfr::mod_file("mod.ini"), "mod.ini itself replaces nothing");
        require(sfr::mod_file_count() == 2, "two files replaced or added");

        // Another tool's cpkredir.ini is left alone.
        write(launcher / "cpkredir.ini", "[CPKREDIR]\nEnabled=1\nModsDbIni=\"C:/elsewhere/ModsDB.ini\"\n");
        const auto external = sfr::scan_mods(launcher);
        require(external.external && !sfr::save_mods(launcher, external), "HedgeModManager's configuration is kept");
        // HedgeModManager's own file: mods\ModsDB.ini relative to the game's
        // folder, ids that are not folder names. Still HedgeModManager's.
        write(launcher / "cpkredir.ini",
              "[CPKREDIR]\nEnabled=true\nModsDbIni=mods\\ModsDB.ini\n[HedgeModManager]\nModProfile=Default\n");
        const auto managed = sfr::scan_mods(launcher);
        std::error_code error;
        require(managed.external && fs::equivalent(managed.external_db, launcher / "mods/ModsDB.ini", error) &&
                !sfr::save_mods(launcher, managed),
                "HedgeModManager's relative ModsDbIni is the launcher's folder, and it stays in charge");
        fs::remove_all(root);
        std::cout << "mod loader tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
