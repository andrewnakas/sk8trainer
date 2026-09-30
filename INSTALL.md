# Installing SK8TRAINER

**Get it from [Releases](https://github.com/andrewnakas/sk8trainer/releases/latest).**

You need your own copy of **Skate 3 for Xbox 360** (a disc image / ISO) and
**Title Update 3**. Nothing from the game is included in any download.

| Your setup | What to download | Guide |
|---|---|---|
| Windows 10/11 x64 | `sk8trainer-<version>-windows-x86_64.zip` | [Windows](#windows) |
| Playing **Skate3Recomp v2.0.2** (mchughalex) on Windows | same zip, drag in two files | [Drag and drop](#add-it-to-skate3recomp-v202-drag-and-drop) |
| Already running the SK8-Engine / level-loader engine on Windows | same zip, swap two files | [Upgrade an existing install](#upgrade-an-existing-windows-install) |
| Linux, Steam Deck, macOS | build from source | [Other platforms](#linux-steam-deck-macos-android-ios) |
| Android, iOS | build from source (your usual app build) | [Other platforms](#linux-steam-deck-macos-android-ios) |
| Your own recomp fork | the source module | [INTEGRATION.md](INTEGRATION.md) |

---

## Windows

1. Download `sk8trainer-<version>-windows-x86_64.zip` from
   [Releases](https://github.com/andrewnakas/sk8trainer/releases/latest).
   Optionally, check it against `SHA256SUMS`:
   ```powershell
   Get-FileHash .\sk8trainer-0.2.0-windows-x86_64.zip -Algorithm SHA256
   ```
2. Unzip it into a folder you can write to, such as `Documents\Skate3`. Avoid
   `Program Files`.
3. Run **`skate3.exe`**. Windows SmartScreen may warn about an unsigned app;
   choose *More info → Run anyway*.
4. First launch only: pick your **Skate 3 ISO**, then **Title Update 3**. The
   engine installs them, which takes a few minutes. Later launches go straight
   to the game.
5. Start skating, then press **Insert** or hold **Back + LB + RB** on your
   controller. The SK8TRAINER panel opens, and its top line should turn green
   with "vault at … 81/81 stock values".

Folder contents:
```
skate3.exe                      the recomp engine with SK8TRAINER built in
rexruntime.dll                  the recomp runtime
dlc/                            optional converted map packs
SK8TRAINER - READ ME.txt        quick start
```

### Add it to Skate3Recomp v2.0.2 (drag and drop)
Already playing the
[Skate3Recomp v2.0.2](https://github.com/mchughalex/skate3recomp/releases/tag/v2.0.2)
Windows release? You don't need to reinstall anything.

1. Close the game.
2. Open your `Skate3Recomp-Windows` folder (the one with `skate3.exe`,
   `rexruntime.dll` and your `game` folder).
3. **Back up** `skate3.exe` and `rexruntime.dll`, for example by renaming them to
   `skate3.exe.bak` and `rexruntime.dll.bak`.
4. From the SK8TRAINER zip, **drag `skate3.exe` and `rexruntime.dll` into that
   folder** and choose *Replace*.
5. Run `skate3.exe` as usual, then press **Insert** in game.

Your installed game (`game\`), settings and saves are untouched. To undo it,
delete the two new files and rename the `.bak` files back.

Tested on a fresh v2.0.2 folder: every trainer check passed (values found
81/81, live edit, 0.5x speed, marker save and go). The exe you drop in is
SK8-Engine, which is v2.0.2 plus a few extras: an add-on picker for map packs in
`dlc\` and a video preset row in the settings. Everything else plays the same.

The Linux and macOS v2.0.2 zips can't take a Windows exe. For those, build
from source (see below).

### Upgrade an existing Windows install
Already have an engine folder from the level loader or an older SK8-Engine
release? Close the game, back up your `skate3.exe` and `rexruntime.dll`, and
copy the two from this zip over them. Your installed game, settings and map
packs stay as they are.

The trainer build is the v0.1.6 engine (New San Van spike fix included) plus
SK8TRAINER, so you don't lose any fixes by switching.

### Uninstall or turn it off
- Turn it off but keep the build: add `skate3_trainer = false` to `skate3.toml`
  beside the exe.
- Undo your tweaks: *Presets → Stock*, or delete
  `%APPDATA%\skate3\trainer\user.toml`.
- Go back to a plain engine: restore your backed-up `skate3.exe` and
  `rexruntime.dll`.

---

## Linux, Steam Deck, macOS, Android, iOS

The trainer is a source module in the same engine, so on these platforms you
build the engine with it switched on. There are no prebuilt trainer builds
for them yet.

1. Check out [SK8-Engine](https://github.com/andrewnakas/SK8-Engine) on the
   `sk8trainer` branch ([PR #1](https://github.com/andrewnakas/SK8-Engine/pull/1)),
   or `main` once it's merged.
2. Build exactly as you normally do for your platform, with your usual preset
   or app project. The trainer is on by default, and CMake fetches it by tag.
   - Offline or using a local checkout: add `-DSKATE3_TRAINER_DIR=/path/to/sk8trainer`
   - Without it: `-DSKATE3_TRAINER=OFF`
3. Launch as usual. Open the trainer with **Back + LB + RB** on a controller,
   or tap the **SK8** button in the top left. The button is on by default on
   Android and iOS; set `skate3_trainer_button = true` to show it elsewhere,
   for example in Steam Deck game mode.

These platforms haven't been built with the trainer yet (see the release
notes). If yours fails, please
[open an issue](https://github.com/andrewnakas/sk8trainer/issues) with the build
log.

---

## Using it

| | Keyboard | Controller | Touch |
|---|---|---|---|
| Open / close | **Insert** | hold **Back + LB + RB** | **SK8** button |
| Tabs | click | **LB / RB** | tap |
| Select / change | drag | **D-pad** (**LT** fine, **RT** coarse) | drag |
| Freeze value | tick box | **A** | tap box |
| Reset to stock | middle-click | **X** | — |

**Practice** tab: game speed (0.05x–2x), five marker slots (save here, go,
copy the game's marker, clear), and auto-return after a bail.

Keyboard, with the menu closed: **Home** go · **Shift+Home** save · **PgUp/PgDn**
slot · **Delete** half speed.

**Settings** go in `skate3.toml` beside the exe, or on the command line as
`--name=value`:

| Setting | Default | What it does |
|---|---|---|
| `skate3_trainer` | `true` | Turns the whole trainer on or off |
| `skate3_trainer_button` | `false` desktop / `true` mobile | Shows the on-screen **SK8** button |
| `skate3_trainer_apply_saved` | `true` | Re-applies your saved values at launch |
| `skate3_trainer_selftest` | `false` | In gameplay, tests every feature and writes `trainer selftest:` lines to the log (then restores stock) |

Your values are saved in `<user data>/trainer/user.toml`. On Windows that's
`%APPDATA%\skate3\trainer\user.toml`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Insert does nothing | Click the game window so it has focus. On laptops, Insert is often **Fn + Ins**. The controller chord always works. |
| Status line orange: "db.big not found; looked in: …" | Fixed in v0.2.1; update first. If it still appears, the message lists every folder tried. Add `game_data_root = "<folder with default.xex>"` to `skate3.toml` beside the exe, and please open an issue with that message. |
| Status line orange: "vault not in guest memory yet" | Normal for a few seconds at boot. If it stays orange in a skate session, run once with `--skate3_trainer_selftest=true` and attach the newest file from `logs/` to an issue. |
| A value changes nothing in game | Some values are only read at certain moments, such as a new session or the next bail. Try a session marker or a restart. Please report which one. |
| Black screen in fullscreen (systems with several display adapters or virtual monitors) | Add `fullscreen = false` to `skate3.toml` beside the exe. |
| Marker "Go" does nothing | The game only allows a marker return in free skate, not in some challenges. The Practice tab shows "return blocked" when that's the case. |
