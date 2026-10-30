# Achievements in the game (Issue #288)

The game awards its 50 achievements through `XUserWriteAchievements`; since
v0.10.28 the port keeps them with the player's saves (`achievements.txt`) and
the launcher lists them. Issue #288 adds, in the game itself:

- **the notification**: the console's "Achievement unlocked" bar at the bottom
  centre, with the achievement's own icon (from the game's XDBF), its
  gamerscore and name. It rises in, stays about four seconds and fades
  (`src/achievement_toast.h`); several queue one after another. The launcher's
  **Achievement notifications** switch (Achievements page, on by default;
  `SFR_ACHIEVEMENT_NOTIFICATIONS`) turns it off. No sound is played: the
  console's sound is Microsoft's and none is shipped.
- **the achievements page**: every achievement with icon, name, description,
  gamerscore, the date it was unlocked or, for those that need a number of
  things, how far the player is.
- **progress** for those achievements, on the page and in the launcher.

## Opening the page

| | Open / close | Choose | Page | Close |
| --- | --- | --- | --- | --- |
| Controller | L3 + R3 (click both sticks in) | D-pad | LB / RB, D-pad left / right | B, BACK |
| Keyboard | F1 | Arrow keys | Page Up / Page Down (Home / End) | Esc, Backspace |
| Touch | tap the notification, or tap with three fingers | drag | | tap outside the page |

Clicking both sticks is used by nothing else in the game, and F1 is no
default key. While the page is open the game is told the system UI is showing
(`XN_SYS_UI` 1, as the console did for its Guide) and reads no input
(`set_input_held`, as for the Kinect adjustment window); in a race the game
pauses itself. On closing, the game gets its input back once every button is
up, so the B that closed the page does not reach it, and `XN_SYS_UI` 0 is sent
once the game has read the 1.

The pictures are drawn in software (`src/overlay_canvas.cpp`, text with the
host's own fonts through Dear ImGui's copy of stb_truetype: Segoe UI on
Windows, Roboto on Android, Arial on Apple, DejaVu / Noto on Linux, a CJK face
for Japanese names) and drawn over the frame by the presentation
(`src/screen_overlay_renderer.cpp`, `src/shaders/overlay.hlsl`), centred on the
game picture, so they stay in the middle at ultrawide and in two-player
split screen. They scale with the picture's height. The game's own font
cannot draw them: its text files carry fonts with only their own glyphs.

## Where the game keeps the counts

All counts are the game's own; nothing is counted by the port.

`XUserWriteAchievements` is called from one place, `82231150` (via the wrapper
`824D04B8`, `XMsgStartIORequest(0xFB, 0xB0008, ...)`), which sends the list
that `822310A0` (award: player, achievement id) fills. The callers of
`822310A0` are the checks:

`822310A0`'s third argument picks the list: 0 is an achievement, anything
else an Avatar award, which `82231228` sends as `XUserAwardAvatarAssets`
(`XMsgStartIORequest(0xFB, 0xB0071, ...)` via `824D0530`, the same
{count, (user, id)*} list). Its only caller is event 8 (award 8). There is no
dashboard to give Avatar items to, so the port logs
`RESULT XUserAwardAvatarAssets ... backend=none` and completes the request;
before 0.11.0 it stopped the game (Issue #317, at the end of a Tag or Relay
race). It can be checked without earning it: put one (user 0, id 8) entry in
the queue at `83E516D8` with
`SFR_POKE=7fea0000=0@3600,7fea0004=8@3600,83E516D8=2146041856@3600,83E516DC=2146041864@3600`
(the entry lives in the unused XMA register page; the title copies it out).

- `82231310(achievement object, event)`: a jump table over events 8..56. For
  most count achievements it adds one to a word and awards at 30
  (e.g. event 15: `lwz r10,0x19EC(r11); addi; stw; cmplwi r10,30; ... li r4,15`).
- `82231870(achievement object, record, race result)`: the checks after a
  race (wins, Rings, the per-course sets, character / gear wins).

The achievement object is at `entry + 0x1B60`; its word `+4` points back at the
player's entry, and both functions work on **S = entry + 0x20**. The entry is
found as `82231740` does: the player table's pointer at `0x83E52F6C` (G), the
signed-in player's index in the low byte of `G + 0x7F08` (-1: nobody), entries
of `0x1FB0` bytes from `G + 0x10`. So S = G + index * 0x1FB0 + 0x30.

The save `SfrAllData.sav` holds S from its byte `0x10` on (file offset =
S offset - 0x10): a run's S matched the save byte for byte over
`S + 0x10 .. S + 0x1A40` but for header words at `S + 0x14 .. 0x1B`. The
launcher reads progress from the save; the page reads S in memory.

Counters (big-endian words):

| Achievement | S offset | Target | Where |
| --- | --- | --- | --- |
| 13 Offline Champ License (offline Normal Race wins) | `0x1954` | 16 | `82231AD0` |
| 14 Rich King (Rings; capped at 99,999 by `822348B4`) | `0x001C` | 10,000 | `822318C8` |
| 15 Bomb-Happy (Target Torpedo hits) | `0x19EC` | 30 | `82231310` event 15 |
| 16 Rocket Star (Soda Rockets) | `0x19F0` | 30 | event 16 |
| 17 Punchout (Bowling Strike hits) | `0x19F4` | 30 | event 17 |
| 18 Masters (Tee Shot hits) | `0x19F8` | 30 | event 18 |
| 19 Troublemaker (Octo-Ink hits) | `0x19FC` | 30 | event 19 |
| 20 Clutch Shooter (Free Throws) | `0x1A00` | 30 | event 20 |
| 21 Ringmaker (Ring Cans) | `0x1A04` | 30 | event 21 |
| 22 Boost Star (Octo-Boosts) | `0x1A08` | 30 | event 22 |
| 53 Charred King (hit by Target Torpedoes) | `0x1A1C` | 30 | event 53 |
| 54 Gutter King (hit by Bowling Strikes) | `0x1A20` | 30 | event 54 |
| 56 Jolt King (shocked at the start line) | `0x1A34` | 30 | event 56 |

Sets (bytes set to 1; the achievement comes when all are set):

| Achievement | S offset | Bytes | Where |
| --- | --- | --- | --- |
| 24 Tag License (courses x standard/expert) | `0x1958` | 16 | `82231A10` |
| 25 Relay License | `0x1968` | 16 | `82231A54` |
| 27 Time Attack License | `0x1978` | 16 | `82231B10` |
| 28 Ring Collection License | `0x1988` | 16 | `82231FE0` |
| 29 Damage Survival License | `0x1998` | 16 | `82232040` |
| 26 Tutorial License | `0x09A8` | 27 | `822319BC` |
| 30 Gear Collector (bytes 11, 16, 39-45 not checked) | `0x09D8` | 46 (37) | `8223150C` |
| 31 Part Collector | `0x0A62` | 21 | `8223158C` |
| 49 Trick King (X-rank trick, standard courses) | `0x1A0C` | 8 | `822315DC` |
| 50 Expert Trick King | `0x1A14` | 8 | `82231624` |
| 51 Character Master (1st with each character) | `0x19A8` | 19 | `822320D0` |
| 52 Gear Master (1st with each gear) | `0x19BB` | 46 | `82232124` |
| 55 Dash King (UNBELIEVABLE start dash per course) | `0x1A24` | 16 | `822316AC` |

Not counted anywhere (one-off conditions, or a time / score in one race):
6, 11, 12, 23, 32-48, 57-59.

Seen in a run with the test save: before the save loads S holds defaults
(8 of 37 gears); after it, 3 wins, 234 Rings, Bomb-Happy 2/30, Ringmaker 10/30,
which the save's bytes agree with.

## Test switches

- `SFR_TEST_ACHIEVEMENT_AWARD=present:id,...` awards an achievement at a
  present, as the game would (notification only; the file is not written).
- `SFR_TEST_ACHIEVEMENT_PAGE=open:close[:rows]` opens the page at a present,
  moves the selection down `rows`, and closes it.
- `SFR_ACHIEVEMENT_TRACE=<file>` every 1200 presents prints
  `ACHIEVEMENT_PROGRESS` with S's address and every counter, and writes S's
  bytes to the file.

`SFR_SCREENSHOT` pictures include the notification and the page (blended in
software as the presentation draws them).
