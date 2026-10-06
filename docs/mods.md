# Mods

Free Riders Recompiled loads **file-replacement mods** in HedgeModManager's
format, the same format Unleashed and Marathon Recompiled use. A mod replaces
the game's files with its own. Code mods (DLLs, patches to the original
executable) do not apply: the game is recompiled.

## Making a mod

A mod is a folder with a `mod.ini`:

```ini
[Desc]
Title="My mod"
Author="Me"
Version="1.0"
Description="What it changes"

[Main]
IncludeDirCount=1
IncludeDir0="."
```

- **Replacing files:** files under an include folder replace the game's files
  at the same path, relative to the game's root (the folder that holds `sound`,
  `movie`, `advE`, …):
  - `<mod>/sound/SRN_BGM.csb` replaces `sound/SRN_BGM.csb`;
  - letter case does not matter.
- **Adding files:** a mod can also add files the game does not have.
- **Load order:** when two mods replace the same file, the one higher in the
  list wins.

## Managing mods

**In the launcher:** the **Mods** tab.
- Put each mod in its own folder inside `mods` beside the launcher (on
  Android: the app's files folder).
- Switch mods on and off and move them up or down.
- The launcher writes `mods/ModsDB.ini` and `cpkredir.ini`; the game reads them
  when it starts.

**With HedgeModManager or another tool:** if `cpkredir.ini` beside the launcher
points to another tool's `ModsDB.ini`, the game uses that list. The launcher
shows the list as managed elsewhere and does not change it.

HedgeModManager itself does not list Free Riders yet. Adding the game there is
a separate change, made to HedgeModManager.

## Checking that a mod applies

`game.log` lists:

- the active mods and how many files each replaces:
  `MODS mod=<folder> files=N`;
- every replaced file the first time the game opens it: `MODS_FILE game=<path>
  from=<file>`.

`SFR_MODS_INI` names another `cpkredir.ini`, for testing. Mod files are read
when the game opens them (`src/mod_loader.cpp`, used by the asset file layer
on every platform).
