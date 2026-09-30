# Settings grouping

The user approved seven categories, with Avatar models as a dedicated entry.

1. General: launcher/game language, skip movies, reset settings.
2. Graphics: window and rendering resolutions adjacent under Resolution (user refinement),
   then fullscreen/backend/VSync; performance
   tuning in an expandable section.
3. Audio: game sound/volume, launcher sound.
4. Controls: player 1 and player 2 source/device groups, bindings; Android touch/tilt.
5. Motion input: camera/Kinect mode, device/preview, mirror/debug, voice commands.
6. Avatar models: existing model picker and help.
7. Game files: existing installation, paths and shader pack.

Preserve settings keys, defaults, game environment and backend behavior. Give each
category its own scroll position. Keep keyboard/controller category navigation,
and focus the first control after changing category. Cancel pending binding capture
on category change. Labels and instructions must agree in English/Traditional Chinese.

Implementation: reorganize launcher_main.cpp, add section labels, update README
and current setup guides. Build Windows/Linux launchers, retain settings regression
tests, inspect the actual UI and verify mouse/keyboard category navigation. Physical
controller validation remains manual. Do not modify runtime or published releases.
