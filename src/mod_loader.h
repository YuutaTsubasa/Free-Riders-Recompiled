#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
// File-replacement mods in HedgeModManager's format, as Unleashed and Marathon
// Recompiled read them:
//
//   cpkredir.ini   [CPKREDIR] Enabled=1, ModsDbIni=<path to ModsDB.ini>
//                  (beside the game, or SFR_MODS_INI names it)
//   ModsDB.ini     [Main] ActiveModCount=N, ActiveMod0..N-1=<id>
//                  [Mods] <id>=<path to the mod's mod.ini>
//   mod.ini        [Desc] Title=..., [Main] IncludeDirCount=N,
//                  IncludeDir0..N-1=<folder, relative to mod.ini>
//
// A file in an include folder replaces the game file at the same path
// (game:\sound\SRN_BGM.csb <- <mod>\<IncludeDir>\sound\SRN_BGM.csb), letters
// in either case. The first active mod that has a file wins (ActiveMod0 is
// the top of the list). Code mods do not apply: the game is recompiled.

// The replacement for a game file, relative to the game's root with either
// separator ("sound/SRN_BGM.csb"), or nothing. Reads the configuration once.
std::optional<std::filesystem::path> mod_file(std::string_view relative);
// How many files the active mods replace (0 without mods), for the log.
size_t mod_file_count();

// The launcher's own list: the mods folder beside it, each subfolder with a
// mod.ini. It writes ModsDB.ini and cpkredir.ini for the game, unless
// cpkredir.ini already points elsewhere (HedgeModManager manages the mods).
struct ModEntry {
    std::string id;                 // the folder's name
    std::string title, author, version, description;
    std::filesystem::path ini;      // its mod.ini
    bool enabled = false;
};
struct ModList {
    std::filesystem::path folder;   // <launcher>/mods
    std::vector<ModEntry> mods;     // in priority order: the first wins
    bool external = false;          // another tool's cpkredir.ini is in charge
    std::filesystem::path external_db;
};
ModList scan_mods(const std::filesystem::path& launcher_directory);
// Writes the enabled mods, in order; false when the files cannot be written
// or another tool manages the mods.
bool save_mods(const std::filesystem::path& launcher_directory, const ModList& list);
}
