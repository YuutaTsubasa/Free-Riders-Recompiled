# Online play: Xbox LIVE on this project's own server

Started 2026-10-06. The title's Xbox LIVE mode runs on a small server that this
project provides, not on Microsoft's service. Nothing here talks to Microsoft.

## Using it

On every device: **Launcher → General → Online play → Xbox LIVE**.

- **One device hosts.** Turn on **Host the server**. That game runs the server
  itself, on TCP and UDP port 47800. Any device can host: a PC, a handheld or a
  phone.
- **The others connect.** Enter the host's local IP address under **Server
  address**, for example `192.168.1.10` or `192.168.1.10:47800`.
- **Online name** is the gamertag the others see. The profile's own name, and
  with it the saves, stays as it is.

In the game, open **Main Menu → Xbox LIVE**:

- **Create Match** opens a lobby.
- **Quick Match** on another device finds that lobby.
- **A** on the found lobby joins it. The title only takes a hand for that
  entry, so A presses it the way the title's own press does
  (`NUI_MENU_PRESS` in the log). `SFR_HAND_SEEK=1` steers the emulated hand
  onto it instead (the older way).
- The Xbox LIVE menu opens on Quick Match; to the right are Leaderboards,
  Xbox LIVE Party, Create Match and Custom Match.

Checked with controller input only (no shortcut words): the guest entered
Xbox LIVE, chose Quick Match, pressed A on the host's lobby, and both
games raced Dolphin Resort to lap 3.

## Status (2026-10-06 afternoon)

**Works, on two games on one PC:**
- Create Match opens a lobby.
- Quick Match on another game finds it, with the host's name, course,
  players, laps and a 3-bar signal.
- A on the found lobby (or the cursor on its → arrow) joins. Both lobbies then show 2/8 with
  both players ready.
- The host starts the race, and both games race it together, each seeing
  the other rider.
- After the race both games go back to the same lobby.

**What was wrong:**
- **The host left its lobby right after a guest joined.** The title's
  XSession wrappers, called without an overlapped block, return
  `GetLastError()` as their result. The guest thread's last error was
  stale, so a successful XSessionJoin of the remote player read as a
  failure (lobby object reason 8, from 8248DCA8). The message dispatch now
  sets the last error, as XAM does: 0 on success or pending.
- **The race stopped at load.** Two recompiled functions were missing:
  - `stvehx` (sub_82263DB0): XenonRecomp's emission was already right, and
    the generator now accepts it;
  - `vupkd3d128` of half floats (sub_822A7400 and four others): XenonRecomp
    emits a debug trap for types 3 and 5, and the generator now emits
    `sfr::vector_unpack_half` (`src/vector_unpack.h`) instead.

**Not yet tested:** two separate devices, more than two players, a guest
leaving mid-race.

`SFR_LIVE_TRACE=1` logs every LIVE call and message (`LIVE_CALL`,
`LIVE_MESSAGE`) and the lobby's state machine (`LIVE_LOBBY_STATE`,
`LIVE_LOBBY_MESSAGE`, `src/live_hooks.cpp`).

A standalone server for a machine that does not play is in
`scripts/live_server.py`:

```bash
python scripts/live_server.py --port 47800
```

## How it works

**`src/live_client.cpp`: the connection.**

- `SFR_LIVE_SERVER=host[:port]` connects to a server. `SFR_LIVE_HOST=1` starts
  one in this process (`src/live_service.cpp`) and connects to it.
- The server puts every game on a virtual network. Each game gets an address
  `10.77.x.y`, a MAC address and a machine id.
- The title's sockets are virtual. Every datagram goes to the server over one
  host UDP socket, and the server hands it to the game that owns the destination
  address. Because of this:
  - two games on one PC can play together;
  - games behind NAT need no open ports, only the host does;
  - a game sending to its own address or 127.0.0.1 gets the datagram back
    locally.
- Each install has its own random online id, kept in `live-id.txt` beside the
  game (`SFR_LIVE_ID` overrides it). The online XUID (0x0009…) comes from that
  id, so two players who are both called "Player" still differ.

**`src/live_imports.cpp`: what the title calls.** Layouts follow Xenia Canary's
netplay build (AdrianCassar/xenia-canary, `netplay_canary_experimental`, BSD
licence).

- **Sign-in:** `XamUserGetSigninState` reports 2 (signed in to LIVE).
  Privileges are granted. `XamUserGetXUID` returns the online XUID.
  `XamUserGetName` returns the online name.
- **XNet:**
  - `XNetGetTitleXnAddr`, `XnAddrToInAddr`, `InAddrToXnAddr`,
    `XnAddrToMachineId` work on the virtual addresses.
  - `XNetQosListen` stores the host's QoS data on the server.
  - `XNetQosLookup` answers at once (complete, 20/25 ms) with that data.
- **Sockets:** `socket`, `bind`, `sendto`, `recvfrom`, `ioctlsocket` and
  `closesocket` are virtual. The title opens one VDP socket (protocol 254) on
  port 1000.
- **Sessions:** `XamSessionCreateHandle` and `XamSessionRefObjByHandle` create
  the session object. The XGI (app 0xFB) session messages it then sends:

  | Message | Call |
  |---|---|
  | 0xB0010 | XSessionCreate |
  | 0xB0011 | delete |
  | 0xB0012 | join |
  | 0xB0013 | leave |
  | 0xB0014 | start |
  | 0xB0015 | end |
  | 0xB0016, 0xB001C | search |
  | 0xB0018 | modify |
  | 0xB001D | details |

  These go to the server.
- **Properties:** user contexts and properties (0xB0006, 0xB0007) are kept and
  travel with a hosted session. That includes strings, and the host's gamertag
  as `X_PROPERTY_GAMER_HOSTNAME` (0x40008109), which the title reads from a
  search result.
- **Stubs:**
  - voice: no headset;
  - XeCryptRandom;
  - XeKeysGetConsoleID;
  - XamUserGetDeviceContext.
- **Not yet:**
  - leaderboards: the stats enumerator fails, so the title shows "0 Entries";
  - ranked arbitration;
  - reading another user's profile settings.

**The runtime gained, for this:**

- waitable timers (`src/guest_timers.cpp`: NtCreateTimer, NtSetTimerEx,
  NtCancelTimer);
- duplicated thread handles (NtDuplicateObject);
- XAM export 0x48C reported as absent.

## What the title does (call map)

**Quick Match:**
1. Privilege check 0xFE.
2. `XSessionSearchEx`: matchmaking query 0, up to 10 results, 2 properties.
3. `XNetQosLookup` for each result. Its poll at `8248B560` sorts by
   `rtt_med`, skips "target disabled", and reads the hostname property.
4. The lobby entry page shows the host, the course and the player count.

**Create Match:**
1. Pick the mode (Normal Race and others).
2. Settings: player count up to 8, COM, laps, private slots, then Select.
3. Course, character and gear.
4. Then:
   - XNet address;
   - VDP socket bound to port 1000;
   - XSessionCreate, with flags 0x42F and 7+1 slots;
   - QoS listen with 6 bytes;
   - XSessionJoin of the local player;
   - a 40-byte datagram to itself.

## Testing on one PC

- The scratch runner starts the server and two games, each with its own save
  copy (under the offline XUID of its gamertag), gamertag and online id.
- Menu words help scripts: `networkmode`, `onmode_quick`, `onmode_create` and
  `lobby`. A word only moves the focus; `ok` presses.
- Two games on one PC run the menus slower, so scripts repeat their words.
- The env template the smoke tests use holds B from frame 12000
  (`SFR_INPUT_SCRIPT=b@0+10000`). Drop it, or the games back out of the LIVE
  menu.
