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
  first launch. Already have the engine? Just swap in the two files.
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

**Physics tabs** (Pop & Jump, Gravity & Speed, Spin, Bails, World). Every value is
the game's own AttribSys tuning, edited live:
- ollie min/max height for each difficulty mode (normal / easy / hardcore /
  motorized), pop bonus, hippy-jump height
- slope gravity, push speed, auto push, pump
- air spin speed, auto body-spin speed, easy body spins
- bail thresholds (landing speed into ground or stairs, contact, balance),
  ragdoll gravity, the delay before the game resets you after a bail
- slow-motion tuning

Any value can be **frozen** (rewritten every frame) or reset to stock. Your
changes are saved to `<user data>/trainer/user.toml` and come back next launch.

**Presets:** Stock, Mega Pop, Moon, No Bail, THPS.

**Practice tab**
- **Game speed** 0.05x to 2x. It scales the sim tick rate, the same thing the
  game's own slow motion does, so physics, animation and audio stay in sync.
- **5 marker slots**: save here, go, copy the game's own marker, clear. "Go"
  runs the game's real session-marker return (fade, teleport, reset), not a raw
  position write. Placing a marker the normal way (LB + d-pad down) can also fill
  the selected slot.
- **Auto-return after a bail**, with an adjustable delay.

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

Platform status for v0.2.0:
- **Windows x64:** built and played. Values are found at boot (81/81 stock
  values match).
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
