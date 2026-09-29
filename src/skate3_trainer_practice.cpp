// SK8TRAINER practice tools. Every hook here passes straight through to the
// game unless the trainer asked for something.
//
// Evidence (lifted asm, generated/skate3_recomp.*.cpp):
//  - sub_82966910(timer, hz): stw r4,32(r3); std 10000000/hz,24(r3). The sim
//    ticks at this rate with a fixed 1/60 step (sub_82859E70 passes the
//    constant 1/60), and the game's own slow motion (sub_82857EC8
//    OnRequestSetSimRate) works by lowering it. Scaling hz slows everything
//    the sim drives, together.
//  - sub_82898FC8 PlayerUI::UpdateSessionMarker(this): marker = [this+20];
//    actions via sub_8255DF90(input, 42 = set, 43 = return); [this+204] is
//    set when the return teleport starts.
//  - marker vtable 0x82315940: has = sub_8289B130 (lbz 5450), get =
//    sub_8289B140, set = sub_8289B6D8. Layout: +5376 transform (64 bytes),
//    +5444 foot-forward word, +5448 has-marker byte, +5452 on-board byte.
//  - local skater state: actor = sub_82897730([this+4]); B = vfunc20 of
//    (actor+44); state id = [[B+28]+16] (300 = WipeoutGround).

#include "skate3_trainer_practice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include "generated/skate3_init.h"

#include <rex/logging.h>

namespace skate3::trainer::practice {
namespace {

constexpr uint32_t kMarkerXform = 5376;
constexpr uint32_t kMarkerFoot = 5444;
constexpr uint32_t kMarkerHas = 5448;
constexpr uint32_t kMarkerUseGate = 5450;
constexpr uint32_t kMarkerOnBoard = 5452;
constexpr uint32_t kActionSet = 42;
constexpr uint32_t kActionReturn = 43;
constexpr int32_t kStateWipeoutGround = 300;

struct SlotData {
  bool valid = false;
  std::array<uint8_t, 64> xform{};
  uint32_t foot = 0;
  uint8_t onboard = 0;
};

// Recursive: the marker hook calls guest code while holding it.
std::recursive_mutex g_mutex;
std::array<SlotData, kSlots> g_slots;
int g_selected = 0;
bool g_auto_capture = true;
bool g_auto_return = false;
float g_auto_return_delay = 1.0f;
Status g_status;

// Requests from the UI, consumed on the game thread.
int g_pending_save = -1;
int g_pending_capture = -1;
int g_pending_goto = -1;
int g_force_return_frames = 0;
bool g_force_set = false;
std::array<uint8_t, 64> g_last_seen{};  // marker transform last frame
int g_bail_frames = 0;
bool g_bail_handled = false;

// Game speed.
float g_speed = 1.0f;
float g_applied_speed = 1.0f;
uint32_t g_timer = 0;
int32_t g_requested_hz = 60;

// Set only while the game's own marker update runs, so forced actions never
// leak into any other input query.
thread_local bool t_in_marker_update = false;

int EffectiveHz(int32_t requested) {
  return std::max(1, static_cast<int>(std::lround(requested * g_speed)));
}

uint32_t CallIndirect(PPCContext ctx, uint8_t* base, uint32_t target) {
  REX_CALL_INDIRECT_FUNC(target);
  return ctx.r3.u32;
}

void CopyOut(uint8_t* base, uint32_t marker, SlotData& s) {
  for (uint32_t i = 0; i < 64; ++i) s.xform[i] = REX_LOAD_U8(marker + kMarkerXform + i);
  s.foot = REX_LOAD_U32(marker + kMarkerFoot);
  s.onboard = REX_LOAD_U8(marker + kMarkerOnBoard);
  s.valid = true;
}

void CopyIn(uint8_t* base, uint32_t marker, const SlotData& s) {
  for (uint32_t i = 0; i < 64; ++i) REX_STORE_U8(marker + kMarkerXform + i, s.xform[i]);
  REX_STORE_U32(marker + kMarkerFoot, s.foot);
  REX_STORE_U8(marker + kMarkerOnBoard, s.onboard);
  REX_STORE_U8(marker + kMarkerHas, 1);
}

std::array<uint8_t, 64> ReadXform(uint8_t* base, uint32_t marker) {
  std::array<uint8_t, 64> out{};
  for (uint32_t i = 0; i < 64; ++i) out[i] = REX_LOAD_U8(marker + kMarkerXform + i);
  return out;
}

float Be32f(const uint8_t* p) {
  uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

int32_t LocalPlayerState(PPCContext& ctx, uint8_t* base, uint32_t self) {
  PPCContext call = ctx;
  call.r1.u64 = uint64_t(ctx.r1.u32 - 0x1000);
  call.r3.u64 = REX_LOAD_U32(self + 4);
  sub_82897730(call, base);
  const uint32_t actor = call.r3.u32;
  if (!actor) return -1;
  const uint32_t iface = actor + 44;
  const uint32_t vtable = REX_LOAD_U32(iface);
  if (!vtable) return -1;
  call.r3.u64 = iface;
  const uint32_t bundle = CallIndirect(call, base, REX_LOAD_U32(vtable + 20));
  if (!bundle) return -1;
  const uint32_t state = REX_LOAD_U32(bundle + 28);
  return state ? static_cast<int32_t>(REX_LOAD_U32(state + 16)) : -1;
}

// Runs on the game thread just before the game's own marker update.
void BeforeMarkerUpdate(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t marker) {
  std::lock_guard lock(g_mutex);
  g_status.marker_seen = true;
  g_status.game_marker_set = REX_LOAD_U8(marker + kMarkerHas) != 0;
  g_status.use_gate = REX_LOAD_U8(marker + kMarkerUseGate) != 0;

  if (g_auto_return) {
    g_status.player_state = LocalPlayerState(ctx, base, self);
    if (g_status.player_state == kStateWipeoutGround) {
      ++g_bail_frames;
      if (!g_bail_handled && g_bail_frames >= static_cast<int>(g_auto_return_delay * 60.0f) &&
          g_slots[g_selected].valid && g_pending_goto < 0 && g_force_return_frames == 0) {
        g_pending_goto = g_selected;
        g_bail_handled = true;
        g_status.last_event = "bail -> returning to slot " + std::to_string(g_selected + 1);
      }
    } else {
      g_bail_frames = 0;
      g_bail_handled = false;
    }
  }

  if (g_pending_goto >= 0) {
    const SlotData& s = g_slots[g_pending_goto];
    if (s.valid && g_status.use_gate) {
      CopyIn(base, marker, s);
      g_last_seen = s.xform;
      g_force_return_frames = 240;  // the game waits a distance-scaled moment
      g_status.last_event = "going to slot " + std::to_string(g_pending_goto + 1);
    } else {
      g_status.last_event = s.valid ? "the game blocks marker use right now"
                                    : "slot " + std::to_string(g_pending_goto + 1) + " is empty";
    }
    g_pending_goto = -1;
  }
  if (g_pending_save >= 0) g_force_set = true;
}

// Runs on the game thread just after the game's own marker update.
void AfterMarkerUpdate(uint8_t* base, uint32_t self, uint32_t marker) {
  std::lock_guard lock(g_mutex);
  const bool has = REX_LOAD_U8(marker + kMarkerHas) != 0;
  if (g_force_return_frames > 0) {
    // [this+204] goes to 1 once the game starts the teleport.
    if (REX_LOAD_U8(self + 204) != 0 || --g_force_return_frames == 0) {
      if (g_force_return_frames == 0) g_status.last_event += " (timed out)";
      g_force_return_frames = 0;
    }
  }
  if (g_force_set) {
    g_force_set = false;
    if (has && g_pending_save >= 0) {
      CopyOut(base, marker, g_slots[g_pending_save]);
      g_last_seen = g_slots[g_pending_save].xform;
      g_status.last_event = "saved slot " + std::to_string(g_pending_save + 1);
    } else if (g_pending_save >= 0) {
      g_status.last_event = "the game refused to set a marker here";
    }
    g_pending_save = -1;
  }
  if (g_pending_capture >= 0) {
    if (has) {
      CopyOut(base, marker, g_slots[g_pending_capture]);
      g_status.last_event = "copied game marker to slot " + std::to_string(g_pending_capture + 1);
    } else {
      g_status.last_event = "no game marker set yet (LB + d-pad down)";
    }
    g_pending_capture = -1;
  }
  if (has) {
    const auto now = ReadXform(base, marker);
    if (now != g_last_seen) {
      // The player set a marker with the game's own control.
      if (g_auto_capture && g_force_return_frames == 0) {
        CopyOut(base, marker, g_slots[g_selected]);
        g_status.last_event = "game marker -> slot " + std::to_string(g_selected + 1);
      }
      g_last_seen = now;
    }
  }
  g_status.restore_pending = g_force_return_frames > 0;
}

}  // namespace

// ------------------------------------------------------------------ API
void SetGameSpeed(float speed) {
  std::lock_guard lock(g_mutex);
  g_speed = std::clamp(speed, 0.02f, 4.0f);
}
float GameSpeed() {
  std::lock_guard lock(g_mutex);
  return g_speed;
}
void SaveHere(int slot) {
  std::lock_guard lock(g_mutex);
  g_pending_save = std::clamp(slot, 0, kSlots - 1);
}
void CaptureGameMarker(int slot) {
  std::lock_guard lock(g_mutex);
  g_pending_capture = std::clamp(slot, 0, kSlots - 1);
}
void GoTo(int slot) {
  std::lock_guard lock(g_mutex);
  g_pending_goto = std::clamp(slot, 0, kSlots - 1);
}
void Clear(int slot) {
  std::lock_guard lock(g_mutex);
  g_slots[std::clamp(slot, 0, kSlots - 1)] = SlotData{};
}
SlotInfo Slot(int slot) {
  std::lock_guard lock(g_mutex);
  const auto& s = g_slots[std::clamp(slot, 0, kSlots - 1)];
  SlotInfo info;
  info.valid = s.valid;
  if (s.valid) {
    info.x = Be32f(&s.xform[48]);
    info.y = Be32f(&s.xform[52]);
    info.z = Be32f(&s.xform[56]);
  }
  return info;
}
int SelectedSlot() {
  std::lock_guard lock(g_mutex);
  return g_selected;
}
void SelectSlot(int slot) {
  std::lock_guard lock(g_mutex);
  g_selected = (slot % kSlots + kSlots) % kSlots;
}
void SetAutoCapture(bool on) {
  std::lock_guard lock(g_mutex);
  g_auto_capture = on;
}
bool AutoCapture() {
  std::lock_guard lock(g_mutex);
  return g_auto_capture;
}
void SetAutoReturn(bool on, float delay_seconds) {
  std::lock_guard lock(g_mutex);
  g_auto_return = on;
  g_auto_return_delay = std::clamp(delay_seconds, 0.0f, 10.0f);
  if (!on) g_status.player_state = -1;
}
bool AutoReturn() {
  std::lock_guard lock(g_mutex);
  return g_auto_return;
}
float AutoReturnDelay() {
  std::lock_guard lock(g_mutex);
  return g_auto_return_delay;
}
Status GetStatus() {
  std::lock_guard lock(g_mutex);
  Status s = g_status;
  s.requested_hz = g_requested_hz;
  s.sim_hz = g_timer ? EffectiveHz(g_requested_hz) : 0;
  return s;
}

void Tick(uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_timer || g_speed == g_applied_speed) return;
  // Re-program the live timer the same way sub_82966910 does.
  const int hz = EffectiveHz(g_requested_hz);
  REX_STORE_U32(g_timer + 32, static_cast<uint32_t>(hz));
  REX_STORE_U64(g_timer + 24, static_cast<uint64_t>(10000000 / hz));
  g_applied_speed = g_speed;
  REXLOG_INFO("trainer: game speed {:.2f} -> sim {} Hz", g_speed, hz);
}

}  // namespace skate3::trainer::practice

using namespace skate3::trainer::practice;

// Sim timer rate (see header comment).
extern "C" REX_FUNC(sub_82966910) {
  {
    std::lock_guard lock(g_mutex);
    g_timer = ctx.r3.u32;
    g_requested_hz = static_cast<int32_t>(ctx.r4.u32);
    ctx.r4.u64 = static_cast<uint64_t>(EffectiveHz(g_requested_hz));
    g_applied_speed = g_speed;
  }
  __imp__sub_82966910(ctx, base);
}

// PlayerUI::UpdateSessionMarker.
extern "C" REX_FUNC(sub_82898FC8) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t marker = REX_LOAD_U32(self + 20);
  if (marker) BeforeMarkerUpdate(ctx, base, self, marker);
  t_in_marker_update = true;
  __imp__sub_82898FC8(ctx, base);
  t_in_marker_update = false;
  if (marker) AfterMarkerUpdate(base, self, marker);
}

// Input action query (bool). Forced only inside the marker update.
extern "C" REX_FUNC(sub_8255DF90) {
  const uint32_t action = ctx.r4.u32;
  __imp__sub_8255DF90(ctx, base);
  if (!t_in_marker_update) return;
  std::lock_guard lock(g_mutex);
  if ((action == kActionReturn && g_force_return_frames > 0) ||
      (action == kActionSet && g_force_set)) {
    ctx.r3.u64 = 1;
  }
}
