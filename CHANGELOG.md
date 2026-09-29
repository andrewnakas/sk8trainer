# Changelog

## v0.2.1 — 2026-09-29
Fixes for v0.2.0, all checked in game by the new unattended self-test.
- **Fixed "db.big not found".** The trainer was given the app's
  pre-install default game folder, which is empty unless configured, instead
  of the folder the engine actually uses. It now gets the runtime's resolved
  folders, falls back to `game/` beside the exe, in the working folder and in
  user data, and lists every path it tried if it still fails.
- **Fixed a crash about 2 s after boot.** The memory scan read committed
  no-access guard pages (guest thread stacks). It now scans only read/write
  guest memory.
- **Fixed game speed doing nothing.** The game only sets its sim timer rate
  on slow-motion requests, so the hook never saw the timer. The trainer now
  finds it directly (`[[0x83083BCC]+16]`, checked against its vtable) once
  gameplay runs. Measured: 0.5x takes the sim from 60 to 30 updates/s.
- All game pointers the practice tools follow are checked against the
  runtime heap table before being read.
- New `skate3_trainer_selftest` setting. It exercises every feature in
  gameplay and logs `trainer selftest:` lines with OK / CHECK / FAIL.

## v0.2.0 — 2026-09-28
First public release.
- **Download:** `sk8trainer-0.2.0-windows-x86_64.zip`, the v0.1.6 engine with
  SK8TRAINER built in. Other platforms build from source (see INSTALL.md).
- The entry table is built at runtime from the player's own `db.big` (EB BIG v3
  + RefPack + AttribSys schema). Nothing game-derived ships, and there's no
  Python step.
- Portable: the guest-memory search uses the runtime heap table, and the pad
  uses the runtime input system. No Win32 or XInput calls.
- Optional on-screen **SK8** button for touch screens (on by default on
  Android and iOS).
- CMake module `sk8trainer.cmake` (`sk8trainer_add(target)`), fetchable by tag.
- Physics tabs: 81 values in 5 groups, with freeze, reset, saved values and
  presets.
- Practice tab: game speed, 5 marker slots through the game's own session
  marker, auto-return after a bail.

## v0.1.0 — 2026-09-28 (internal)
- First Windows build with a pre-generated table. Verified in game: vault
  found at boot, 81/81 stock values.
