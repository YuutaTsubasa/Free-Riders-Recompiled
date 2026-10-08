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

## Mods made for the original game

Sound, texture and text mods for Sonic Free Riders on Xenia or the Xbox 360
usually say to copy their files over the game's (for example into `sound`).
They work here unchanged, without touching the installed game: make a folder
in `mods`, put the `mod.ini` above in it, and copy the mod's files under it at
the same paths, so that `sound/...` sits next to `mod.ini`.

- **Voices:** with **Voice language** in the launcher set to English or
  Japanese, the game opens that language's voice files (`SRN_Eact_*`,
  `SRN_Jact_*`, `SRN_stream_voice_e/j`). A voice mod replaces the files of the
  language it was made for, so it applies when that language is the one heard.
- **Text:** the text files are per language (`advE`, `advJ`, …); a text mod
  applies when the game's language matches the files it replaces. The
  [Traditional Chinese mod](https://github.com/YuutaTsubasa/Free-Riders-Recompiled-Traditional-Chinese-Mod)
  replaces the Japanese files, so it needs the game language set to Japanese.
- **Not applicable:** patches to the original executable (`default.xex`),
  such as the No Kinect Patch, and Xenia-specific patches.

## Managing mods

**In the launcher:** the **Mods** tab.
- Put each mod in its own folder inside `mods` beside the launcher (on
  Android: the app's files folder).
- Switch mods on and off and move them up or down.
- The launcher writes `mods/ModsDB.ini` and `cpkredir.ini`; the game reads them
  when it starts.

**With HedgeModManager or another tool:** if `cpkredir.ini` beside the launcher
belongs to another tool, the game uses that tool's list, and the launcher shows
it as managed elsewhere and does not change it. That is the case when
`cpkredir.ini` points to a `ModsDB.ini` other than the launcher's, or has a
`[HedgeModManager]` section (HedgeModManager names mods by ID, not by folder,
even in the launcher's own `mods` folder). A relative `ModsDbIni`, as
HedgeModManager writes it (`mods\ModsDB.ini`), is relative to the folder of
`cpkredir.ini`. To manage mods in the launcher again, delete `cpkredir.ini`.

On Windows the launcher records where it is installed each time it starts, in
`HKEY_CURRENT_USER\SOFTWARE\FreeRidersRecompiled` (`ExecutableFilePath`,
`RootDirectoryPath`): the same values HedgeModManager reads to find
Unleashed Recompiled. HedgeModManager itself does not list Free Riders yet;
adding the game there is a separate change, made to HedgeModManager.

## Checking that a mod applies

`game.log` lists:

- the active mods and how many files each replaces:
  `MODS mod=<folder> files=N`;
- every replaced file the first time the game opens it: `MODS_FILE game=<path>
  from=<file>`.

`SFR_MODS_INI` names another `cpkredir.ini`, for testing. Mod files are read
when the game opens them (`src/mod_loader.cpp`, used by the asset file layer
on every platform).
