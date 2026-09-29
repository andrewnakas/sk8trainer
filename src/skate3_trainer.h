#pragma once

// SK8TRAINER: in-game trainer for the Skate 3 recomp (CH3AT-style value
// editing, freezing and presets). Values live in the skater AttribSys vault
// blob the game loads into guest memory; the trainer finds that blob by
// signature (built at runtime from the player's own db.big) and edits the
// layout fields in place.
//
// Wiring (see INTEGRATION.md):
//   - Tick(base) from the guest Swap hook (frame boundary, guest thread)
//   - CapturesInput() in the InputSystem active callback
//   - Configure() once paths and the input system exist
//   - a TrainerDialog created in OnCreateDialogs, SetToggleHandler(), RegisterBinds()

#include <cstdint>
#include <filesystem>
#include <functional>

#include <rex/ui/imgui_dialog.h>

namespace rex::input {
class InputSystem;
}

namespace skate3::trainer {

struct Paths {
  std::filesystem::path game_data_root;    // holds data/big/db.big
  std::filesystem::path update_data_root;  // title update (checked first)
  std::filesystem::path user_data_root;    // writable: trainer/user.toml
};

// Call once the runtime exists (before the first Tick): where the game files
// are, where to save, and the runtime's input system (pad chord + menu nav).
void Configure(const Paths& paths, rex::input::InputSystem* input);

// Guest frame boundary: publishes the guest base, runs the vault locator,
// re-applies frozen values and polls the pad chord / menu navigation.
void Tick(uint8_t* base);

// True while the trainer menu is open and owns the pad.
bool CapturesInput();

// Called (from the guest thread) when the pad chord asks to toggle the menu;
// the app marshals it onto the UI thread.
void SetToggleHandler(std::function<void()> handler);

// Keyboard binds for the practice tools (Home, Shift+Home, PageUp/Down, Delete).
void RegisterBinds();
void UnregisterBinds();

// Always drawn (it polls the pad chord and can show a touch button); only
// repaints continuously while the menu is open.
class TrainerDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TrainerDialog(rex::ui::ImGuiDrawer* drawer);

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }
  bool WantsContinuousRepaint() const override { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
};

}  // namespace skate3::trainer
