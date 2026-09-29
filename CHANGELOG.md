# Changelog

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
