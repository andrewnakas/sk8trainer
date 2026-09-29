#include "skate3_trainer.h"
#include "skate3_trainer_practice.h"
#include "skate3_trainer_vault.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>

REXCVAR_DEFINE_BOOL(skate3_trainer, true, "Skate 3",
                    "SK8TRAINER: in-game trainer (Insert, or hold Back+LB+RB on the pad)");
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(SKATE3_IOS))
#define SK8TRAINER_TOUCH_DEFAULT true
#else
#define SK8TRAINER_TOUCH_DEFAULT false
#endif
REXCVAR_DEFINE_BOOL(skate3_trainer_button, SK8TRAINER_TOUCH_DEFAULT, "Skate 3",
                    "Show a small SK8 button that opens the trainer (for touch screens)");
REXCVAR_DEFINE_BOOL(skate3_trainer_apply_saved, true, "Skate 3",
                    "Re-apply the saved trainer values (<user data>/trainer/user.toml) whenever the vault is found");

namespace skate3::trainer {
namespace {

using Type = vault::Type;
using Anchor = vault::Anchor;

struct Entry {
  std::string label, group, source;
  Type type = Type::kF32;
  uint32_t offset = 0;
  double stock = 0, min = 0, max = 1;
  double value = 0;     // last value read from / written to guest memory
  bool frozen = false;  // rewritten every frame
  bool touched = false; // edited by the user this session (saved)
};

enum class Locate { kIdle, kScanning, kFound, kNotFound };

// Shared state. The guest thread (Tick) and UI thread (OnDraw) both touch it.
std::mutex g_mutex;
std::vector<Entry> g_entries;
std::vector<Anchor> g_anchors;
std::vector<std::string> g_groups;
uint32_t g_bin_size = 0;
std::string g_table_error;
bool g_table_loaded = false;

std::atomic<uint8_t*> g_base{nullptr};    // guest memory base (host)
std::atomic<uint8_t*> g_blob{nullptr};    // host address of the vault .bin
std::atomic<Locate> g_locate{Locate::kIdle};
std::atomic<uint32_t> g_scan_hits{0};
std::atomic<bool> g_menu_open{false};
std::function<void()> g_toggle_handler;
int g_stock_matches = 0;
std::string g_status;

Paths g_paths;
bool g_configured = false;
rex::input::InputSystem* g_input = nullptr;

// Writable on every platform: the recomp's user data folder (settings.toml
// lives there too); the exe folder only as a last resort.
std::filesystem::path TrainerFolder() {
  return (g_paths.user_data_root.empty() ? rex::filesystem::GetExecutableFolder()
                                         : g_paths.user_data_root) /
         "trainer";
}

std::filesystem::path UserPath() { return TrainerFolder() / "user.toml"; }

// ---------------------------------------------------------------- guest I/O
uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

double ReadEntry(const Entry& e, uint8_t* blob) {
  uint8_t* p = blob + e.offset;
  switch (e.type) {
    case Type::kF32: {
      uint32_t bits = LoadBE32(p);
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }
    case Type::kBool:
      return p[0] ? 1.0 : 0.0;
    case Type::kI32:
      return static_cast<int32_t>(LoadBE32(p));
    case Type::kU32:
      return LoadBE32(p);
  }
  return 0;
}

void WriteEntry(const Entry& e, uint8_t* blob, double v) {
  uint8_t* p = blob + e.offset;
  switch (e.type) {
    case Type::kF32: {
      float f = static_cast<float>(v);
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      StoreBE32(p, bits);
      break;
    }
    case Type::kBool:
      p[0] = v != 0 ? 1 : 0;
      break;
    case Type::kI32:
      StoreBE32(p, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v))));
      break;
    case Type::kU32:
      StoreBE32(p, static_cast<uint32_t>(std::max(0.0, std::round(v))));
      break;
  }
}

// ---------------------------------------------------------------- table
// Builds the table from the player's own db.big (title update first, then
// the disc copy), so nothing game-derived ships with the trainer.
// Runs on its own thread (reads and decompresses ~2.5 MB once).
void LoadTable() {
  vault::Table table;
  std::filesystem::path used;
  for (const auto& root : {g_paths.update_data_root, g_paths.game_data_root}) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root / "data" / "big" / "db.big", ec)) continue;
    table = vault::BuildFromGame(root);
    used = root;
    if (table.error.empty()) break;
  }
  std::lock_guard lock(g_mutex);
  if (used.empty()) {
    g_table_error = "db.big not found under the game data folder";
  } else if (!table.error.empty()) {
    g_table_error = "vault read failed: " + table.error;
  }
  if (!g_table_error.empty()) {
    REXLOG_WARN("trainer: {}", g_table_error);
    return;
  }
  g_bin_size = table.bin_size;
  g_anchors = std::move(table.anchors);
  for (vault::Field& f : table.fields) {
    Entry e;
    e.label = std::move(f.label);
    e.group = std::move(f.group);
    e.source = std::move(f.source);
    e.type = f.type;
    e.offset = f.offset;
    e.stock = e.value = f.stock;
    e.min = f.min;
    e.max = f.max;
    if (std::find(g_groups.begin(), g_groups.end(), e.group) == g_groups.end()) {
      g_groups.push_back(e.group);
    }
    g_entries.push_back(std::move(e));
  }
  REXLOG_INFO("trainer: {} entries, {} anchors built from {}", g_entries.size(), g_anchors.size(),
              (used / "data" / "big" / "db.big").string());
}

// Saved user values: source -> {value, frozen}.
void SaveUser() {
  toml::table root;
  for (const Entry& e : g_entries) {
    if (!e.touched && !e.frozen) continue;
    root.insert(e.source, toml::table{{"value", e.value}, {"frozen", e.frozen}});
  }
  std::error_code ec;
  std::filesystem::create_directories(TrainerFolder(), ec);
  std::ofstream(UserPath()) << root;
}

void ApplySavedLocked(uint8_t* blob) {
  std::error_code ec;
  if (!std::filesystem::exists(UserPath(), ec)) return;
  try {
    toml::table root = toml::parse_file(UserPath().string());
    for (Entry& e : g_entries) {
      auto* row = root[e.source].as_table();
      if (!row) continue;
      e.value = (*row)["value"].value_or(e.stock);
      e.frozen = (*row)["frozen"].value_or(false);
      e.touched = true;
      WriteEntry(e, blob, e.value);
    }
  } catch (const std::exception& ex) {
    REXLOG_WARN("trainer: ignoring {}: {}", UserPath().string(), ex.what());
  }
}

// ---------------------------------------------------------------- locator
// Anchors are pointer-free windows of skatercollections.bin; every alias of
// guest memory that holds the loaded blob matches all of them at the same
// relative offsets. Pick the candidate base where the most anchors agree.
bool AnchorsMatch(uint8_t* blob, int* count) {
  int ok = 0;
  for (const Anchor& a : g_anchors) {
    if (std::memcmp(blob + a.offset, a.bytes.data(), a.bytes.size()) == 0) ++ok;
  }
  if (count) *count = ok;
  return ok * 2 > static_cast<int>(g_anchors.size());
}

void ScanThread(uint8_t* base) {
  const Anchor& first = g_anchors.front();
  const auto searcher = std::boyer_moore_horspool_searcher(first.bytes.begin(), first.bytes.end());
  uint8_t* best = nullptr;
  int best_count = 0;
  uint32_t hits = 0;
  auto* memory = rex::system::kernel_memory();
  // Walk the guest address space through the runtime's own heap table, so
  // only committed pages are touched (portable: no OS memory queries).
  uint64_t a = 0x00010000;
  while (memory && a < 0x100000000ull) {
    auto* heap = memory->LookupHeap(static_cast<uint32_t>(a));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(static_cast<uint32_t>(a), &info) || !info.region_size) {
      a = (a + 0x10000) & ~0xFFFFull;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    if (info.state & rex::memory::kMemoryAllocationCommit) {
      uint8_t* region = memory->TranslateVirtual<uint8_t*>(info.base_address);
      uint8_t* region_end = region + info.region_size;
      for (auto it = region;; ++it) {
        it = std::search(it, region_end, searcher);
        if (it == region_end) break;
        uint8_t* blob = it - first.offset;
        int count = 0;
        if (blob >= region && blob + g_bin_size <= region_end) {
          ++hits;
          AnchorsMatch(blob, &count);
          if (count > best_count) {
            best_count = count;
            best = blob;
          }
        }
      }
    }
    a = next > a ? next : a + 0x1000;
  }
  g_scan_hits = hits;
  if (best && best_count * 2 > static_cast<int>(g_anchors.size())) {
    std::lock_guard lock(g_mutex);
    int matches = 0;
    for (Entry& e : g_entries) {
      e.value = ReadEntry(e, best);
      if (std::fabs(e.value - e.stock) < 1e-4 * std::max(1.0, std::fabs(e.stock))) ++matches;
    }
    g_stock_matches = matches;
    if (REXCVAR_GET(skate3_trainer_apply_saved)) ApplySavedLocked(best);
    g_blob = best;
    g_locate = Locate::kFound;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "vault at host +0x%llX (%d/%zu anchors, %d/%zu stock values)",
                  static_cast<unsigned long long>(best - base), best_count, g_anchors.size(),
                  matches, g_entries.size());
    g_status = buf;
    REXLOG_INFO("trainer: {}", g_status);
  } else {
    g_locate = Locate::kNotFound;
    g_status = "vault not in guest memory yet (" + std::to_string(hits) + " partial hits)";
  }
}

void StartScan(uint8_t* base) {
  Locate expected = g_locate.load();
  if (expected == Locate::kScanning || g_anchors.empty() || !g_bin_size) return;
  if (!g_locate.compare_exchange_strong(expected, Locate::kScanning)) return;
  std::thread(ScanThread, base).detach();
}

// ---------------------------------------------------------------- presets
struct PresetRule {
  const char* source_part;  // substring of Entry::source
  bool multiply;            // true: stock * value, false: set value
  double value;
};
struct Preset {
  const char* name;
  const char* note;
  std::vector<PresetRule> rules;
};

const std::vector<Preset>& Presets() {
  static const std::vector<Preset> presets = {
      {"Stock", "Every value back to the game's own tuning", {}},
      {"Mega Pop", "Ollie heights x3",
       {{"/JumpMinHeight", true, 3}, {"/JumpMaxHeight", true, 3}, {"AbsoluteMinHeight", true, 3}}},
      {"Moon", "Pop x2.5, floaty ragdolls, higher hippy jumps",
       {{"/JumpMinHeight", true, 2.5}, {"/JumpMaxHeight", true, 2.5},
        {"physics_biped/default/JumpHeight", true, 2.5},
        {"Wipeout_AirYAcceleration", true, 0.3}, {"Wipeout_GroundYAcceleration", true, 0.3}}},
      {"No Bail", "Landing / contact bail thresholds x100 (freezes them)",
       {{"Wipeout_AirMaxSpeedIntoGround", true, 100}, {"Wipeout_AirMaxSpeedIntoStairs", true, 100},
        {"Wipeout_AirSkeletonMaxContact", true, 100}, {"Wipeout_GroundSkeletonMaxContact", true, 100},
        {"Wipeout_GroundBalanceTotal", true, 100}}},
      {"THPS", "Fast spins, easy body spins, auto push",
       {{"MaxSpinSpeed", true, 3}, {"MaxAutoBodySpinSpeed", true, 3}, {"EasyBodySpins", false, 1},
        {"AutoPushEnabled", false, 1}, {"MaxPushableSpeed", true, 2}}},
  };
  return presets;
}

void ApplyPresetLocked(const Preset& preset) {
  uint8_t* blob = g_blob.load();
  for (Entry& e : g_entries) {
    double v = e.stock;
    bool hit = false;
    for (const PresetRule& r : preset.rules) {
      if (e.source.find(r.source_part) != std::string::npos) {
        v = r.multiply ? e.stock * r.value : r.value;
        hit = true;
      }
    }
    e.value = v;
    e.frozen = hit;
    e.touched = hit;
    if (blob) WriteEntry(e, blob, v);
  }
  SaveUser();
}

// ---------------------------------------------------------------- pad
struct PadNav {
  int tab = 0;
  int row = 0;
  WORD prev = 0;
  int repeat_frames = 0;
  bool chord_was = false;
  bool scroll_to_row = false;
};
PadNav g_pad;

std::vector<int> RowsOfTabLocked(int tab) {
  std::vector<int> rows;
  if (tab < 0 || tab >= static_cast<int>(g_groups.size())) return rows;
  for (int i = 0; i < static_cast<int>(g_entries.size()); ++i) {
    if (g_entries[i].group == g_groups[tab]) rows.push_back(i);
  }
  return rows;
}

void NudgeLocked(Entry& e, int dir, bool fine, bool coarse) {
  if (e.type == Type::kBool) {
    e.value = e.value != 0 ? 0 : 1;
  } else {
    double step = std::max(std::fabs(e.stock) * 0.1, (e.max - e.min) / 200.0);
    if (fine) step *= 0.1;
    if (coarse) step *= 10;
    if (e.type != Type::kF32) step = std::max(1.0, std::round(step));
    e.value = std::clamp(e.value + dir * step, e.min, e.max);
  }
  e.touched = true;
  if (uint8_t* blob = g_blob.load()) WriteEntry(e, blob, e.value);
}

// XInput-style bit names on top of the runtime's portable pad state.
using WORD = uint16_t;
constexpr WORD XINPUT_GAMEPAD_DPAD_UP = rex::input::X_INPUT_GAMEPAD_DPAD_UP;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN = rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT = rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT = rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
constexpr WORD XINPUT_GAMEPAD_BACK = rex::input::X_INPUT_GAMEPAD_BACK;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER = rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
constexpr WORD XINPUT_GAMEPAD_A = rex::input::X_INPUT_GAMEPAD_A;
constexpr WORD XINPUT_GAMEPAD_B = rex::input::X_INPUT_GAMEPAD_B;
constexpr WORD XINPUT_GAMEPAD_X = rex::input::X_INPUT_GAMEPAD_X;
constexpr WORD XINPUT_GAMEPAD_Y = rex::input::X_INPUT_GAMEPAD_Y;

// UI thread (TrainerDialog::OnDraw runs on every paint, shown or not).
void PollPad() {
  if (!g_input) return;
  rex::input::X_INPUT_GAMEPAD pad{};
  if (!g_input->GetUiGamepadState(&pad)) return;
  struct {
    WORD wButtons;
    uint8_t bLeftTrigger, bRightTrigger;
  } gp{static_cast<WORD>(pad.buttons), pad.left_trigger, pad.right_trigger};
  struct {
    decltype(gp) Gamepad;
  } xs{gp};
  const WORD b = xs.Gamepad.wButtons;
  const WORD chord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
  const bool chord_now = (b & chord) == chord;
  if (chord_now && !g_pad.chord_was && g_toggle_handler) g_toggle_handler();
  g_pad.chord_was = chord_now;
  if (!g_menu_open || chord_now) {
    g_pad.prev = b;
    return;
  }
  const WORD pressed = b & ~g_pad.prev;
  // Held d-pad repeats after ~0.3 s.
  const WORD dpad = b & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                         XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
  WORD act = pressed;
  if (dpad && dpad == (g_pad.prev & dpad)) {
    if (++g_pad.repeat_frames > 18 && g_pad.repeat_frames % 3 == 0) act |= dpad;
  } else {
    g_pad.repeat_frames = 0;
  }
  g_pad.prev = b;
  const bool fine = xs.Gamepad.bLeftTrigger > 100;
  const bool coarse = xs.Gamepad.bRightTrigger > 100;

  std::lock_guard lock(g_mutex);
  const int tabs = static_cast<int>(g_groups.size()) + 2;  // + Practice, Presets
  if (act & XINPUT_GAMEPAD_RIGHT_SHOULDER) g_pad.tab = (g_pad.tab + 1) % tabs, g_pad.row = 0;
  if (act & XINPUT_GAMEPAD_LEFT_SHOULDER) g_pad.tab = (g_pad.tab + tabs - 1) % tabs, g_pad.row = 0;
  if ((act & XINPUT_GAMEPAD_B) && g_toggle_handler) g_toggle_handler();
  if (g_pad.tab == tabs - 2) {
    // Practice: row 0 speed, 1..5 slots, 6 auto-return, 7 auto-capture.
    namespace pr = practice;
    constexpr int kRows = 3 + pr::kSlots;
    if (act & XINPUT_GAMEPAD_DPAD_DOWN) g_pad.row = (g_pad.row + 1) % kRows;
    if (act & XINPUT_GAMEPAD_DPAD_UP) g_pad.row = (g_pad.row + kRows - 1) % kRows;
    const int row = g_pad.row;
    if (row == 0) {
      const float step = coarse ? 0.25f : 0.05f;
      if (act & XINPUT_GAMEPAD_DPAD_RIGHT) pr::SetGameSpeed(pr::GameSpeed() + step);
      if (act & XINPUT_GAMEPAD_DPAD_LEFT) pr::SetGameSpeed(pr::GameSpeed() - step);
      if (act & XINPUT_GAMEPAD_A) pr::SetGameSpeed(pr::GameSpeed() < 0.99f ? 1.0f : 0.5f);
      if (act & XINPUT_GAMEPAD_X) pr::SetGameSpeed(1.0f);
    } else if (row <= pr::kSlots) {
      const int slot = row - 1;
      if (act & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) pr::SelectSlot(slot);
      if (act & XINPUT_GAMEPAD_A) pr::GoTo(slot);
      if (act & XINPUT_GAMEPAD_X) pr::SaveHere(slot);
      if (act & XINPUT_GAMEPAD_Y) pr::CaptureGameMarker(slot);
      if (act & XINPUT_GAMEPAD_DPAD_LEFT) pr::Clear(slot);
    } else if (row == pr::kSlots + 1 && (act & XINPUT_GAMEPAD_A)) {
      pr::SetAutoReturn(!pr::AutoReturn(), pr::AutoReturnDelay());
    } else if (row == pr::kSlots + 2 && (act & XINPUT_GAMEPAD_A)) {
      pr::SetAutoCapture(!pr::AutoCapture());
    }
    return;
  }
  const auto rows = RowsOfTabLocked(g_pad.tab);
  if (rows.empty()) {
    if ((act & XINPUT_GAMEPAD_A) && g_pad.tab == tabs - 1) {
      // Tools tab: A cycles presets via row index.
      const auto& presets = Presets();
      ApplyPresetLocked(presets[std::clamp(g_pad.row, 0, static_cast<int>(presets.size()) - 1)]);
    }
    if (act & XINPUT_GAMEPAD_DPAD_DOWN) g_pad.row = std::min(g_pad.row + 1, static_cast<int>(Presets().size()) - 1);
    if (act & XINPUT_GAMEPAD_DPAD_UP) g_pad.row = std::max(g_pad.row - 1, 0);
    return;
  }
  if (act & XINPUT_GAMEPAD_DPAD_DOWN) g_pad.row = (g_pad.row + 1) % rows.size(), g_pad.scroll_to_row = true;
  if (act & XINPUT_GAMEPAD_DPAD_UP) g_pad.row = (g_pad.row + rows.size() - 1) % rows.size(), g_pad.scroll_to_row = true;
  g_pad.row = std::clamp(g_pad.row, 0, static_cast<int>(rows.size()) - 1);
  Entry& e = g_entries[rows[g_pad.row]];
  if (act & XINPUT_GAMEPAD_DPAD_RIGHT) NudgeLocked(e, +1, fine, coarse);
  if (act & XINPUT_GAMEPAD_DPAD_LEFT) NudgeLocked(e, -1, fine, coarse);
  if (act & XINPUT_GAMEPAD_A) e.frozen = !e.frozen, e.touched = true;
  if (act & XINPUT_GAMEPAD_X) {
    e.value = e.stock;
    e.frozen = false;
    if (uint8_t* blob = g_blob.load()) WriteEntry(e, blob, e.value);
  }
  if (act & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A |
             XINPUT_GAMEPAD_X)) {
    SaveUser();
  }
}

void DrawPracticeLocked(int pad_row) {
  namespace pr = practice;
  const pr::Status st = pr::GetStatus();
  auto mark = [&](int row) {
    if (row == pad_row) {
      ImGui::TextColored(ImVec4(1, 1, 0.4f, 1), ">");
      ImGui::SameLine();
    }
  };
  // Game speed.
  mark(0);
  float speed = pr::GameSpeed();
  ImGui::SetNextItemWidth(200);
  if (ImGui::SliderFloat("Game speed", &speed, 0.05f, 2.0f, "%.2fx")) pr::SetGameSpeed(speed);
  for (float preset : {0.1f, 0.25f, 0.5f, 1.0f}) {
    ImGui::SameLine();
    char label[16];
    std::snprintf(label, sizeof(label), "%gx", preset);
    if (ImGui::SmallButton(label)) pr::SetGameSpeed(preset);
  }
  if (st.sim_hz) {
    ImGui::TextDisabled("sim %d Hz (game asked %d)   Delete: toggle half speed", st.sim_hz,
                        st.requested_hz);
  } else {
    ImGui::TextDisabled("sim timer not seen yet (applies when the game next sets its rate)");
  }
  ImGui::Separator();
  // Slots.
  ImGui::Text("Marker slots   Home: go   Shift+Home: save here   PgUp/PgDn: select");
  const int selected = pr::SelectedSlot();
  for (int i = 0; i < pr::kSlots; ++i) {
    ImGui::PushID(i);
    mark(i + 1);
    const pr::SlotInfo info = pr::Slot(i);
    if (ImGui::RadioButton("##sel", selected == i)) pr::SelectSlot(i);
    ImGui::SameLine();
    if (info.valid) {
      ImGui::Text("Slot %d  (%.1f, %.1f, %.1f)", i + 1, info.x, info.y, info.z);
    } else {
      ImGui::TextDisabled("Slot %d  empty", i + 1);
    }
    ImGui::SameLine(300);
    if (ImGui::SmallButton("Go")) {
      pr::SelectSlot(i);
      pr::GoTo(i);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save here")) {
      pr::SelectSlot(i);
      pr::SaveHere(i);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy game marker")) pr::CaptureGameMarker(i);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) pr::Clear(i);
    ImGui::PopID();
  }
  ImGui::TextDisabled("Pad on a slot: A go  X save here  Y copy game marker  Left clear");
  ImGui::Separator();
  mark(pr::kSlots + 1);
  bool auto_return = pr::AutoReturn();
  float delay = pr::AutoReturnDelay();
  if (ImGui::Checkbox("Auto-return to selected slot after a bail", &auto_return)) {
    pr::SetAutoReturn(auto_return, delay);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  if (ImGui::DragFloat("delay s", &delay, 0.05f, 0.0f, 10.0f, "%.2f")) {
    pr::SetAutoReturn(auto_return, delay);
  }
  mark(pr::kSlots + 2);
  bool capture = pr::AutoCapture();
  if (ImGui::Checkbox("Game marker (LB + d-pad down) also fills the selected slot", &capture)) {
    pr::SetAutoCapture(capture);
  }
  ImGui::Separator();
  ImGui::TextDisabled("marker %s | game marker %s | return %s | state %d%s",
                      st.marker_seen ? "hooked" : "not seen yet",
                      st.game_marker_set ? "set" : "empty",
                      st.use_gate ? "allowed" : "blocked", st.player_state,
                      st.restore_pending ? " | teleporting" : "");
  if (!st.last_event.empty()) ImGui::Text("%s", st.last_event.c_str());
}

}  // namespace

// ================================================================ public
void SetToggleHandler(std::function<void()> handler) { g_toggle_handler = std::move(handler); }

void Configure(const Paths& paths, rex::input::InputSystem* input) {
  std::lock_guard lock(g_mutex);
  g_paths = paths;
  g_input = input;
  g_configured = true;
}

bool CapturesInput() { return g_menu_open.load(); }

void RegisterBinds() {
  namespace pr = practice;
  rex::ui::RegisterBind("bind_skate3_trainer_goto", "Home", "SK8TRAINER: go to selected slot",
                        [] { pr::GoTo(pr::SelectedSlot()); });
  rex::ui::RegisterBind("bind_skate3_trainer_save", "Shift+Home",
                        "SK8TRAINER: save selected slot here",
                        [] { pr::SaveHere(pr::SelectedSlot()); });
  rex::ui::RegisterBind("bind_skate3_trainer_prev", "PageUp", "SK8TRAINER: previous slot",
                        [] { pr::SelectSlot(pr::SelectedSlot() - 1); });
  rex::ui::RegisterBind("bind_skate3_trainer_next", "PageDown", "SK8TRAINER: next slot",
                        [] { pr::SelectSlot(pr::SelectedSlot() + 1); });
  rex::ui::RegisterBind("bind_skate3_trainer_slowmo", "Delete", "SK8TRAINER: toggle half speed",
                        [] { pr::SetGameSpeed(pr::GameSpeed() < 0.99f ? 1.0f : 0.5f); });
}

void UnregisterBinds() {
  for (const char* b : {"bind_skate3_trainer_goto", "bind_skate3_trainer_save",
                        "bind_skate3_trainer_prev", "bind_skate3_trainer_next",
                        "bind_skate3_trainer_slowmo"}) {
    rex::ui::UnregisterBind(b);
  }
}

void Tick(uint8_t* base) {
  if (!REXCVAR_GET(skate3_trainer)) return;
  g_base = base;
  {
    std::lock_guard lock(g_mutex);
    if (!g_table_loaded && g_configured) {
      g_table_loaded = true;
      std::thread(LoadTable).detach();
    }
  }
  // Locate: retry every ~2 s until found; re-verify the anchors every ~2 s
  // after that, since a world change can reload the vault elsewhere.
  static uint32_t frame = 0;
  if (++frame % 120 == 0) {
    uint8_t* blob = g_blob.load();
    if (blob && !AnchorsMatch(blob, nullptr)) {
      REXLOG_INFO("trainer: vault moved or unloaded, rescanning");
      g_blob = nullptr;
      g_locate = Locate::kIdle;
    }
    if (!g_blob.load()) StartScan(base);
  }
  if (uint8_t* blob = g_blob.load()) {
    std::lock_guard lock(g_mutex);
    for (Entry& e : g_entries) {
      if (e.frozen) {
        WriteEntry(e, blob, e.value);
      } else if (frame % 30 == 0) {
        e.value = ReadEntry(e, blob);  // follow the game's own changes
      }
    }
  }
  practice::Tick(base);
}

TrainerDialog::TrainerDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

void TrainerDialog::Show() {
  visible_ = true;
  g_menu_open = true;
}

void TrainerDialog::Hide() {
  visible_ = false;
  g_menu_open = false;
}

void TrainerDialog::Toggle() { visible_ ? Hide() : Show(); }

void TrainerDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!REXCVAR_GET(skate3_trainer)) return;
  PollPad();
  if (!visible_) {
    if (REXCVAR_GET(skate3_trainer_button)) {
      ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.35f);
      if (ImGui::Begin("##sk8trainer_button", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav)) {
        if (ImGui::Button("SK8", ImVec2(64, 40))) Show();
      }
      ImGui::End();
    }
    return;
  }
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("SK8TRAINER (Insert / Back+LB+RB)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) Hide();
    return;
  }
  std::lock_guard lock(g_mutex);
  // Status line.
  switch (g_locate.load()) {
    case Locate::kFound:
      ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", g_status.c_str());
      break;
    case Locate::kScanning:
      ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "scanning guest memory for the vault...");
      break;
    default:
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                         g_table_error.empty()
                             ? (g_status.empty() ? "waiting for gameplay" : g_status.c_str())
                             : g_table_error.c_str());
  }
  ImGui::TextDisabled("Pad: LB/RB tab  Dpad select/adjust  LT fine RT coarse  A freeze  X reset  B close");
  const bool live = g_blob.load() != nullptr;
  uint8_t* blob = g_blob.load();

  const int tabs = static_cast<int>(g_groups.size()) + 2;
  if (ImGui::BeginTabBar("trainer_tabs")) {
    for (int t = 0; t < tabs; ++t) {
      const bool tools = t == tabs - 1;
      const bool practice_tab = t == tabs - 2;
      const char* name = tools ? "Presets" : practice_tab ? "Practice" : g_groups[t].c_str();
      ImGuiTabItemFlags flags = 0;
      static int last_pad_tab = -1;
      if (g_pad.tab != last_pad_tab && g_pad.tab == t) {
        flags |= ImGuiTabItemFlags_SetSelected;
      }
      if (!ImGui::BeginTabItem(name, nullptr, flags)) continue;
      if (g_pad.tab == t) last_pad_tab = t;
      if (ImGui::IsItemClicked()) g_pad.tab = t, g_pad.row = 0;
      if (practice_tab) {
        DrawPracticeLocked(g_pad.tab == t ? g_pad.row : -1);
      } else if (tools) {
        const auto& presets = Presets();
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
          const bool sel = g_pad.tab == t && g_pad.row == i;
          if (ImGui::Button(presets[i].name, ImVec2(140, 0)) && live) ApplyPresetLocked(presets[i]);
          ImGui::SameLine();
          ImGui::TextColored(sel ? ImVec4(1, 1, 0.4f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1), "%s%s",
                             sel ? "> " : "", presets[i].note);
        }
        ImGui::Separator();
        if (ImGui::Button("Rescan vault")) {
          g_blob = nullptr;
          g_locate = Locate::kIdle;
          if (uint8_t* base = g_base.load()) StartScan(base);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("saved values: %s", (TrainerFolder() / "user.toml").string().c_str());
        ImGui::TextDisabled("Presets are untested guesses until verified in play.");
      } else {
        const auto rows = RowsOfTabLocked(t);
        ImGui::BeginChild("rows");
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
          Entry& e = g_entries[rows[r]];
          ImGui::PushID(rows[r]);
          const bool sel = g_pad.tab == t && g_pad.row == r;
          if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0.4f, 1));
            if (g_pad.scroll_to_row) ImGui::SetScrollHereY(0.5f), g_pad.scroll_to_row = false;
          }
          if (ImGui::Checkbox("##freeze", &e.frozen)) e.touched = true, SaveUser();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Freeze (rewrite every frame)");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(170);
          bool changed = false;
          if (e.type == Type::kBool) {
            bool on = e.value != 0;
            changed = ImGui::Checkbox(e.label.c_str(), &on);
            e.value = on ? 1 : 0;
          } else {
            float v = static_cast<float>(e.value);
            const float speed = static_cast<float>(std::max(std::fabs(e.stock) * 0.01, (e.max - e.min) / 1000.0));
            changed = ImGui::DragFloat(e.label.c_str(), &v, speed, static_cast<float>(e.min),
                                       static_cast<float>(e.max), "%.3f");
            e.value = v;
          }
          if (changed) {
            e.touched = true;
            if (blob) WriteEntry(e, blob, e.value);
            SaveUser();
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nstock %.4g  (middle-click resets)", e.source.c_str(), e.stock);
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) {
            e.value = e.stock;
            e.frozen = false;
            if (blob) WriteEntry(e, blob, e.value);
            SaveUser();
          }
          if (sel) ImGui::PopStyleColor();
          if (e.value != e.stock) {
            ImGui::SameLine();
            ImGui::TextDisabled("(stock %.3g)", e.stock);
          }
          ImGui::PopID();
        }
        ImGui::EndChild();
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
  if (!open) Hide();
}

}  // namespace skate3::trainer
