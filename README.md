# SK8TRAINER

An in-game trainer for the **Skate 3 static recompilation** (rexglue). It's a
tuning and practice menu that runs inside the game: sliders for pop, gravity,
spin and bails, plus slow motion, five marker slots and auto-return after a bail.

It's inspired by [CH3AT](https://github.com/ckosmic/CH3AT), the PS3/RPCS3 Skate 3
trainer. CH3AT pokes fixed PS3 memory addresses from an external window.
SK8TRAINER is compiled into the recomp instead, and finds everything at runtime
from **your own game files**, so it works on every platform the recomp builds
for.

> Ships no game code or data. The trainer reads `data/big/db.big` from the game
> you installed, at runtime, on your device.

## Download

**[→ Latest release](https://github.com/andrewnakas/sk8trainer/releases/latest)**
has step-by-step instructions in **[INSTALL.md](INSTALL.md)**.

- **Windows:** download `sk8trainer-<version>-windows-x86_64.zip`, unzip,
  run `skate3.exe`, and point it at your Skate 3 ISO and Title Update 3 on
  first launch. Already have the engine, or the
  [Skate3Recomp v2.0.2](https://github.com/mchughalex/skate3recomp/releases/tag/v2.0.2)
  release? Just drag the two files over yours
  ([how](INSTALL.md#add-it-to-skate3recomp-v202-drag-and-drop)).
- **Linux, Steam Deck, macOS, Android, iOS:** build
  [SK8-Engine](https://github.com/andrewnakas/SK8-Engine) with the trainer
  switched on ([how](INSTALL.md#linux-steam-deck-macos-android-ios)).
- **Your own recomp fork:** see [INTEGRATION.md](INTEGRATION.md).

You need your own Skate 3 (Xbox 360) disc image and Title Update 3.

## Controls

| | Keyboard | Controller | Touch |
|---|---|---|---|
| Open / close | **Insert** | hold **Back + LB + RB** | **SK8** button (top left) |
| Switch tab | click | **LB / RB** | tap |
| Select / adjust | drag | **D-pad** (hold **LT** fine, **RT** coarse) | drag |
| Freeze value | tick box | **A** | tap box |
| Reset to stock | middle-click | **X** | — |
| Close | Insert | **B** | SK8 |

While the menu is open, the pad drives the menu instead of the skater. The SK8
button is on by default on Android and iOS (`skate3_trainer_button`).

### Practice keys (keyboard)
**Home** go to the selected slot · **Shift+Home** save the selected slot here ·
**PgUp/PgDn** pick a slot · **Delete** toggle half speed.

## What's in it

**Physics tabs** (Pop & Jump, Gravity & Speed, Spin, Flips, Bails, World): 178
values, every one the game's own AttribSys tuning, edited live:
- every ollie / jump height for each difficulty mode, pop bonus, hippy and
  off-board jump height
- slope gravity, push top speed and power, run speed, auto push, pump
- easy body spins (the real spin lever: +60% air rotation, measured), spin caps
- body-flip speed and the one-flip lock ("perfect body flips"), landing
  alignment
- every bail threshold, board-separation and hit tolerances, ragdoll gravity,
  the delay before the game resets you after a bail
- slow-motion tuning

An edit to a per-difficulty value is copied to every difficulty, so your tuning
survives a difficulty change.

Anything you change stays set (the trainer re-applies it every frame) until you
reset it to stock. Your changes are saved to `<user data>/trainer/user.toml` and come back next launch.

**Presets** stack, so you can combine them; Stock resets everything.

| Preset | What it does |
|---|---|
| **Never Bail** | You cannot bail. Thresholds out of reach, plus the bail itself is blocked |
| **Mega Pop** | Every ollie and jump height x3 |
| **Spin** | Easy body spins on, spin caps x3 |
| **Multi Flip** | Double / triple / quad body flips in every difficulty |
| **Land Any Angle** | Crooked landings get squared up and roll away |
| **Fast** | Push top speed x4, push power x4, run speed x3 |
| **Locked In** | Never Bail + Land Any Angle + easy spins + controllable multi-flips |
| **Big Air** | All of the above at once |
| **Moon**, **THPS** | Floaty low-gravity feel; arcade spins and auto push |

**Practice tab**
- **Game speed** 0.05x to 2x. It scales the sim tick rate, the same thing the
  game's own slow motion does, so physics, animation and audio stay in sync.
- **5 marker slots**: save here, go, copy the game's own marker, clear. "Go"
  runs the game's real session-marker return (fade, teleport, reset), not a raw
  position write. Placing a marker the normal way (LB + d-pad down) can also fill
  the selected slot.
- **Auto-return after a bail**, with an adjustable delay.
- **NEVER BAIL** checkbox (remembered between launches). It works on three
  levels: bail thresholds out of reach, detected bails dropped, and the skater's
  state machine not allowed to enter the bail state. AI skaters still fall.
  If the game demands a bail on every tick for 2 s, it is let through so you can
  never get stuck.
- **Perfect finish** (experimental): flips rotate only while a grab trigger is
  held.

## Installing it in a recomp build

SK8TRAINER is a source module for rexglue Skate 3 recompilations. You add it
when you build the recomp. The same module covers every platform the recomp
builds for: Windows, Linux and Steam Deck, macOS, Android and iOS.

- **[andrewnakas/SK8-Engine](https://github.com/andrewnakas/SK8-Engine)**
  (formerly skate3recomp): the integration is on the `sk8trainer` branch,
  [PR #1](https://github.com/andrewnakas/SK8-Engine/pull/1). Once it's merged,
  every build made from that source includes the trainer (on by default;
  `-DSKATE3_TRAINER=OFF` turns it off).
- **Any other fork:** follow [INTEGRATION.md](INTEGRATION.md). It's one CMake
  include plus a few `#if defined(SKATE3_TRAINER)` lines. The CMake snippet can
  fetch this repo by tag, so there's nothing to vendor.

Platform status:
- **Windows x64:** built and checked in game with the self-test: the vault is
  found (178/178 stock values) and the unattended audit passes: every slider
  writes, holds and restores, every preset applies exactly, saved values round
  trip, game speed, all marker-slot actions, forced bail + auto-return.
- **Linux, Steam Deck, macOS, Android, iOS:** the code only uses the runtime's
  portable APIs: the guest heap table for memory, the runtime input system for
  the pad, and ImGui. It has not been built on those platforms yet. Please open
  an issue with your build log if it doesn't compile.

## How it works

1. **Table.** At startup it opens `db.big` (EB BIG v3), decompresses the
   skater vault (`skaterschema` + `skatercollections`, RefPack), and reads the
   AttribSys schema. For each curated field it gets the byte offset inside
   `skatercollections.bin`, the type and the stock value. It also picks eight
   pointer-free 64-byte "anchor" windows. Field names are matched by their
   64-bit AttribSys hash.
2. **Locate.** A background thread walks the committed guest heaps and finds
   where the game loaded `skatercollections.bin` (the base where most anchors
   agree). It re-checks every ~2 s and rescans if the blob moves.
3. **Edit.** Values are big-endian writes into that blob. The game reads them
   from there, so changes are live.
4. **Practice.** Hooks on three lifted functions:
   - `sub_82966910`: the sim timer rate, used for game speed.
   - `sub_82898FC8`: `PlayerUI::UpdateSessionMarker`, used for the marker slots
     and auto-return.
   - `sub_8255DF90`: the input action query. Presses are forced only inside the
     marker update.

## Building the self-test

`tools/vault_selftest.cpp` runs the table builder against a game folder, with
no recomp involved:

```
clang-cl /std:c++20 /EHsc /O2 /Isrc tools/vault_selftest.cpp src/skate3_trainer_vault.cpp
vault_selftest.exe "<game folder containing data/big/db.big>"
```

## Credits
- CH3AT by ckosmic, for the idea and the list of values worth exposing.
- The AttribSys vault layout follows NFSTools/VaultLib (MIT).
- The rexglue SDK and the Skate 3 recompilation it plugs into.

MIT licensed. Skate 3 is © Electronic Arts; this project is not affiliated with EA.
