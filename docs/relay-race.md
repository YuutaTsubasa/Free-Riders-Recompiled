# Relay Race

Relay Race (Offline Mode → Relay Race) is one Kinect with the racers taking
turns: the title waits for the racer who is up to leave the sensor and for
somebody new to step in. With controllers that is a button press.

## Playing it

- **Who is up:** the controller that pressed A at the last swap drives the
  racer. Up to four controllers (the keyboard counts as the first); one
  controller passed round works too.
- **"Swap out with the next player"** (character select, and before the
  boards): **A on any controller** is the swap. The racer walks out, a new
  person walks in and the title identifies them as a guest.
- **In the race** the racer stops at a gate at the end of their leg: **A on
  any controller** there is the swap, and the next racer starts.
- A second controller is not a second person in Relay Race (it takes its
  turn instead), so the single-team types (2, 3 or 4 Player Team) work with
  any number of controllers.

## Not supported yet

- **2P vs 2P** (two teams, two people in front of the sensor at once, split
  screen): the second team's person is not shown, so the swaps do not start.

## How it works (`src/nui_hooks.cpp`)

- Team mode byte `83E515E7`: 0 normal, 2 Tag Race, 3 Relay Race.
- Menu swap: the menu manager's `+0x68` bit `0x8`.
- Race swap: the racer's state `[83E53160]+0x1C4` is 5 while racing, 3 at the
  gate and 4 while the next one comes in. One swap per arrival at the gate.
- The swap: no skeleton for 45 frames, then the same slot under a new tracking
  id (3, 4, ...) and not yet identified, so the title runs
  `NuiIdentityIdentify` on it. `nui_gamepad()` follows the controller that
  pressed A (`set_relay_pad`).
- Log: `NUI_RELAY_SWAP out/in`, `NUI_RELAY_GATE waiting`.

Checked with scripted input: a 2 Player Team relay from character select
through both legs to the results, and a 3 Player Team relay whose three
handoffs each swapped once at their gate.
