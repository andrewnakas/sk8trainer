# Adding SK8TRAINER to a Skate 3 recomp

This works for any rexglue-based Skate 3 recompilation:
[SK8-Engine](https://github.com/andrewnakas/SK8-Engine) (formerly
skate3recomp), its Android and iOS builds, or your own fork. There are four
steps. Each code change is behind `#if defined(SKATE3_TRAINER)`, so building
with `-DSKATE3_TRAINER=OFF` gives back the untouched game.

## 1. CMake: add the module

After the `skate3` target exists (after `add_executable` / `add_library`):

```cmake
option(SKATE3_TRAINER "Build the SK8TRAINER in-game trainer" ON)
set(SKATE3_TRAINER_DIR "" CACHE PATH "Local SK8TRAINER checkout (empty = fetch from GitHub)")
set(SKATE3_TRAINER_TAG "v0.2.1" CACHE STRING "SK8TRAINER git tag to fetch")
if(SKATE3_TRAINER)
    if(NOT SKATE3_TRAINER_DIR)
        include(FetchContent)
        FetchContent_Declare(sk8trainer
            GIT_REPOSITORY https://github.com/andrewnakas/sk8trainer.git
            GIT_TAG ${SKATE3_TRAINER_TAG}
            GIT_SHALLOW TRUE)
        FetchContent_GetProperties(sk8trainer)
        if(NOT sk8trainer_POPULATED)
            FetchContent_Populate(sk8trainer)
        endif()
        set(SKATE3_TRAINER_DIR "${sk8trainer_SOURCE_DIR}")
    endif()
    include("${SKATE3_TRAINER_DIR}/sk8trainer.cmake")
    sk8trainer_add(skate3)
endif()
```

`sk8trainer_add` adds the three `.cpp` files and the include path, and defines
`SKATE3_TRAINER=1`. The trainer sources include `"generated/skate3_init.h"`, so
the recomp's root folder must already be on the target's include path, as it is
in SK8-Engine. For offline builds, pass
`-DSKATE3_TRAINER_DIR=<checkout>`.

## 2. App class: dialog, binds, configuration

In the app header (next to the other dialogs):

```cpp
#if defined(SKATE3_TRAINER)
#include "skate3_trainer.h"
#endif
// ...
#if defined(SKATE3_TRAINER)
  std::unique_ptr<skate3::trainer::TrainerDialog> trainer_dialog_;
#endif
```

In `OnCreateDialogs(rex::ui::ImGuiDrawer* drawer)`:

```cpp
#if defined(SKATE3_TRAINER)
  trainer_dialog_ = std::make_unique<skate3::trainer::TrainerDialog>(drawer);
  skate3::trainer::SetToggleHandler([this] {
    app_context().CallInUIThreadDeferred([this] {
      if (trainer_dialog_) trainer_dialog_->Toggle();
    });
  });
  rex::ui::RegisterBind("bind_skate3_trainer", "Insert", "SK8TRAINER menu", [this] {
    if (trainer_dialog_) trainer_dialog_->Toggle();
  });
  skate3::trainer::RegisterBinds();
#endif
```

In `OnPostSetup()`, before the input system's active callback is set. Also
AND `CapturesInput()` into that callback so the pad drives the menu and not the
skater:

```cpp
#if defined(SKATE3_TRAINER)
  // Use the runtime's folders: the app's own game_data_root() is the
  // pre-install default and is empty unless configured.
  skate3::trainer::Configure(
      {runtime()->game_data_root(), runtime()->update_data_root(), runtime()->user_data_root()},
      static_cast<rex::input::InputSystem*>(runtime()->input_system()));
#endif
  // ...inside SetActiveCallback's lambda:
  bool trainer_captures = false;
#if defined(SKATE3_TRAINER)
  trainer_captures = skate3::trainer::CapturesInput();
#endif
  return /* existing conditions */ && !trainer_captures;
```

On shutdown, before the drawer goes away:

```cpp
#if defined(SKATE3_TRAINER)
  rex::ui::UnregisterBind("bind_skate3_trainer");
  skate3::trainer::UnregisterBinds();
  skate3::trainer::SetToggleHandler({});
  trainer_dialog_.reset();
#endif
```

## 3. Frame hook: one call

In the recomp's existing guest Swap override (`sub_82B82E08`, the frame
boundary), before calling the original:

```cpp
#if defined(SKATE3_TRAINER)
  skate3::trainer::Tick(base);
#endif
```

If your fork has no Swap override yet, add one:

```cpp
extern "C" REX_FUNC(sub_82B82E08) {
#if defined(SKATE3_TRAINER)
  skate3::trainer::Tick(base);
#endif
  __imp__sub_82B82E08(ctx, base);
}
```

## 4. Check for hook clashes

The trainer defines strong overrides for three lifted functions. A lifted
function can only have one strong override, so make sure your fork doesn't
already override these:

| Function | What it is | Used for |
|---|---|---|
| `sub_82966910` | sim timer: set tick frequency | game speed |
| `sub_82898FC8` | `PlayerUI::UpdateSessionMarker` | marker slots, auto-return |
| `sub_8255DF90` | input action query (bool) | forcing set/return inside the marker update only |

If one clashes, the link fails with a duplicate symbol. Move that override's
body into your existing one, calling the matching trainer logic.

## Which game build

The addresses above are for the Xbox 360 retail XEX that SK8-Engine
lifts, with the title update applied. The physics values don't depend on
addresses at all (they're found by signature in your own `db.big`). If your
recomp lifts a different XEX, only the three practice hooks need new addresses.
