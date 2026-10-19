# Online play: Xbox LIVE on this project's own server

Started 2026-10-06. The title's Xbox LIVE mode runs on a small server that this
project provides, not on Microsoft's service. Nothing here talks to Microsoft.

## Using it

On every device: **Launcher → General → Online play → Xbox LIVE**.

- **One device hosts.** Turn on **Host the server**. That game runs the server
  itself, on TCP and UDP port 47800. Any device can host: a PC, a handheld or a
  phone.
  The launcher shows this device's address for the others ("Friends enter:
  192.168.1.23"). The server exists only while the host's game runs.
  The first time a Windows PC hosts, Windows asks whether to let the game
  through its firewall. Allow it (private networks): if the question is
  dismissed, Windows blocks the others and they get no answer. The rule can be
  changed later in Windows Security > Firewall & network protection > Allow an
  app through firewall.
- **The others connect.** Enter the host's local IP address under **Server
  address**, for example `192.168.1.10` or `192.168.1.10:47800`.
  **Test connection** checks it from the launcher (up to 3 s), and
  **Start game** runs the same check first. The answers:
  - *Connected*: a server of this version answered.
  - *Address not found*: the name or address could not be looked up.
  - *... is there, but no server runs on port 47800*: the device answered,
    but the host's game is not running with Host the server on.
  - *No answer*: the host's game is not running, the devices are on
    different networks, or the host's firewall drops port 47800.
  - *Not a Free Riders server, or a different version*.
  - *... needs a password*: the server has a room password and the
    **Password** box is empty.
  - *... the password is wrong*.

  When the check fails, Start asks: **Start anyway** or **Cancel**. A game
  that starts before the host's keeps trying (below), so starting first is
  fine; a missing or wrong password is not tried again.
- **Password** (optional) is the room password. While hosting, the others must
  enter it to join; while joining, it is the server's. Empty means none. It
  travels unencrypted (below), so it should not be one used anywhere else.
- **Online name** is the gamertag the others see. The profile's own name, and
  with it the saves, stays as it is.

### A server in the cloud

`server/` holds the same server as a Docker image,
`ghcr.io/yuutatsubasa/free-riders-server`, published with each release, for
a machine that does not play: a VPS or a free cloud VM. Every player then
enters that machine's address, nobody hosts, and nobody opens ports at home;
only the server needs TCP and UDP port 47800. Every packet of a race goes
through it, so it should be in a region near the players.
[server/README.md](../server/README.md) has the commands, the settings
(password, limits), and guides for a VPS, Oracle Cloud's free VM and Fly.io.

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
`server/live_server.py` (also as a Docker image, [server/README.md](../server/README.md);
`scripts/live_server.py` still runs it):

```bash
python server/live_server.py --port 47800
```

## How it works

**`src/live_client.cpp`: the connection.**

- `SFR_LIVE_SERVER=host[:port]` connects to a server. `SFR_LIVE_HOST=1` starts
  one in this process (`src/live_service.cpp`) and connects to it.
- The first use connects at once (3 s at most; `LIVE_SERVER unreachable=...
  reason=refused|no-answer|address|unexpected` in game.log when it fails).
  After a failure, a thread tries again every 5 s (`LIVE_SERVER retry=N
  connected=...`). The title has read its sign-in state by then, so the next
  `XNotifyGetNext` sends it `XN_SYS_SIGNINCHANGED`; it reads the state again,
  now signed in to LIVE, and Xbox LIVE opens from the Main Menu (checked: a
  server started after the guest reached the Main Menu).
- `src/live_connect.cpp` holds what the game and the launcher share: reading
  `host[:port]`, a connect with a time limit, the request exchange, and the
  launcher's check. The check sends `PING`, which a server answers
  (`OK freeriders <version>`) without making the caller a console, so it
  does not show to the others. A server from before PING answers it with an
  error, which the check takes as "an older release" and lets the game start.
- **The room password** (`SFR_LIVE_PASSWORD`, the launcher's Password; the
  same variable sets the password of `server/live_server.py` and of the
  in-game host). A server with one answers PING with
  `OK freeriders 1 password`; a game then sends `PASS <password as hex>`
  before `HELLO`, and the server answers `OK` or `ERR wrong password` and
  closes the connection. `HELLO` without it gets `ERR password required`.
  A server without a password answers `PASS` with `OK`, so a game with a
  password set still joins an open server, and games from before the
  password (no PASS) join it as before. The launcher's check sends the same
  PASS after PING (`password-required` and `wrong-password` in
  `LAUNCHER_ONLINE_CHECK` and `LIVE_SERVER unreachable=`); the game does not
  retry after either. The password is compared in constant time and never
  written to a log, and it is not in an Android diagnostic ZIP. It travels as
  plain hex over TCP: a room key, not account security. The protocol version
  stays 1: older launchers read only the first three words of PING's answer.
- **The UDP token.** HELLO's answer ends with an 8-byte token (16 hex), which
  older games ignore. A game registers its UDP address with `0x00`, its
  console number and that token, again every 10 s (which also keeps a NAT
  mapping open and shows the server the game is alive). Without the token,
  anybody who guessed a console number could take its datagrams; a
  registration without one (a game from before it) is taken only by a server
  without a password, and never for a console that sent the token. The
  server relays a datagram only from a registered UDP address and only when
  its source field is that console's own virtual address.
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
