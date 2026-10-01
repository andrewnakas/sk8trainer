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
#include <set>

#include "generated/skate3_init.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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
int g_timer_hz_live = 0;
uint64_t g_marker_updates = 0;

// Scripted pad (spin test).
struct TestPad {
  bool active = false;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
};
TestPad g_pad;
float g_last_up[3] = {0, 1, 0};
uint32_t g_pad_packet = 1;

// Skater transform tracking (spin test): the 4x4 matrix inside the actor,
// found by matching the position the game writes into its session marker.
bool g_measure_on = false;
uint32_t g_track_actor = 0, g_track_addr = 0;
bool g_track_search = false;
Measure g_measure;
float g_last_yaw = 0, g_base_y = 0;
bool g_have_yaw = false;
std::set<int32_t> g_measure_states;

// Set only while the game's own marker update runs, so forced actions never
// leak into any other input query.
thread_local bool t_in_marker_update = false;

int EffectiveHz(int32_t requested) {
  return std::max(1, static_cast<int>(std::lround(requested * g_speed)));
}

// True when [addr, addr+size) is committed, readable guest memory. Pointers
// read from the game early in boot can be stale or not yet set up.
bool GuestReadable(uint32_t addr, uint32_t size) {
  // The loaded XEX image (code + .data/.rdata) is always mapped.
  constexpr uint32_t kImageBase = 0x82000000, kImageEnd = 0x831B0000;
  if (addr >= kImageBase && uint64_t(addr) + size <= kImageEnd) return true;
  auto* memory = rex::system::kernel_memory();
  if (!memory || !addr) return false;
  auto* heap = memory->LookupHeap(addr);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(addr, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) &&
         (info.protect & rex::memory::kMemoryProtectRead) &&
         uint64_t(addr) + size <= uint64_t(info.base_address) + info.region_size;
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

uint32_t g_last_actor = 0;
bool g_never_bail = false;
int g_detect_streak = 0;
uint32_t g_local_state = 0;  // the local player's state object (AI skaters have their own)
int g_block_streak = 0;  // consecutive ticks a bail was refused
// vfunc20(actor+44): [+0] -> object with the skater's 4x4 world matrix at +64
// (sub_82592A00), [+4] -> object with the velocity at +80, [+28] -> state.
uint32_t g_bundle = 0;
int g_exp_ticks = 0;
bool g_exp_have = false;
std::array<uint8_t, 48> g_exp_rot{};

float LoadF(uint8_t* base, uint32_t addr) {
  const uint32_t v = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The state machine's body pointer ([state+4]) points 0xDD0 into the big
// physics object at [bundle+24]; AI skaters have their own.
bool IsLocalBody(uint8_t* base, uint32_t body) {
  if (!g_bundle || !GuestReadable(g_bundle + 24, 4)) return false;
  const uint32_t physics = REX_LOAD_U32(g_bundle + 24);
  return physics && body >= physics && body < physics + 0x2000;
}

int32_t LocalPlayerState(PPCContext& ctx, uint8_t* base, uint32_t self) {
  PPCContext call = ctx;
  call.r1.u64 = uint64_t(ctx.r1.u32 - 0x1000);
  call.r3.u64 = REX_LOAD_U32(self + 4);
  sub_82897730(call, base);
  const uint32_t actor = call.r3.u32;
  g_last_actor = actor;
  if (!GuestReadable(actor + 44, 4)) return -1;
  const uint32_t iface = actor + 44;
  const uint32_t vtable = REX_LOAD_U32(iface);
  if (vtable < 0x82000000 || vtable >= 0x831B0000) return -1;
  call.r3.u64 = iface;
  const uint32_t bundle = CallIndirect(call, base, REX_LOAD_U32(vtable + 20));
  if (!GuestReadable(bundle + 28, 4)) return -1;
  g_bundle = bundle;
  const uint32_t state = REX_LOAD_U32(bundle + 28);
  g_local_state = state;
  return GuestReadable(state + 16, 4) ? static_cast<int32_t>(REX_LOAD_U32(state + 16)) : -1;
}

// Runs on the game thread just before the game's own marker update.
void BeforeMarkerUpdate(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t marker) {
  std::lock_guard lock(g_mutex);
  g_status.marker_seen = true;
  ++g_marker_updates;
  g_status.game_marker_set = REX_LOAD_U8(marker + kMarkerHas) != 0;
  g_status.use_gate = REX_LOAD_U8(marker + kMarkerUseGate) != 0;

  if (g_measure_on && !g_auto_return) g_status.player_state = LocalPlayerState(ctx, base, self);
  {
    // State timeline (cheap): one log line per change.
    static int32_t last_state = -2;
    const int32_t now_state = (g_measure_on || g_auto_return) ? g_status.player_state : LocalPlayerState(ctx, base, self);
    g_status.player_state = now_state;
    if (now_state != last_state) {
      REXLOG_INFO("trainer: skater state {} -> {}", last_state, now_state);
      last_state = now_state;
    }
  }
  if (g_measure_on) {
    ++g_measure.ticks;
    if (g_status.player_state != 100) ++g_measure.air_ticks;
    g_measure_states.insert(g_status.player_state);
    g_track_addr = 0;
    if (g_bundle && GuestReadable(g_bundle, 8)) {
      const uint32_t holder = REX_LOAD_U32(g_bundle);
      if (GuestReadable(holder + 64, 64)) g_track_addr = holder + 64;
    }
    g_track_actor = g_last_actor;
  }
  if (g_exp_ticks > 0 && g_track_addr) {
    const uint32_t body = REX_LOAD_U32(g_bundle + 4);
    const float yaw = std::atan2(LoadF(base, g_track_addr + 8), LoadF(base, g_track_addr)) * 57.29578f;
    if (!g_exp_have) {
      for (uint32_t i = 0; i < 48; ++i) g_exp_rot[i] = REX_LOAD_U8(g_track_addr + i);
      g_exp_have = true;
    }
    std::string v;
    if (GuestReadable(body + 64, 96)) {
      char buf[24];
      for (uint32_t o = 64; o < 160; o += 4) {
        std::snprintf(buf, sizeof(buf), " %.2f", LoadF(base, body + o));
        v += buf;
      }
    }
    REXLOG_INFO("trainer: exp tick {} yaw before write {:.1f} | body+64..:{}", g_exp_ticks, yaw, v);
    for (uint32_t i = 0; i < 48; ++i) REX_STORE_U8(g_track_addr + i, g_exp_rot[i]);
    if (--g_exp_ticks == 0) g_exp_have = false;
  }
  if (g_measure_on && g_track_addr && g_last_actor == g_track_actor && GuestReadable(g_track_addr, 64)) {
    const float yaw = std::atan2(LoadF(base, g_track_addr + 8), LoadF(base, g_track_addr)) * 57.29578f;
    const float y = LoadF(base, g_track_addr + 52);
    const float up[3] = {LoadF(base, g_track_addr + 16), LoadF(base, g_track_addr + 20), LoadF(base, g_track_addr + 24)};
    if (g_have_yaw) {
      const float dot = std::clamp(up[0] * g_last_up[0] + up[1] * g_last_up[1] + up[2] * g_last_up[2], -1.0f, 1.0f);
      g_measure.tumble_total += std::acos(dot) * 57.29578f;
      if (g_status.player_state != 100) g_measure.air_tumble += std::acos(dot) * 57.29578f;
    }
    std::memcpy(g_last_up, up, sizeof(up));
    if (g_have_yaw) {
      float d = yaw - g_last_yaw;
      while (d > 180) d -= 360;
      while (d < -180) d += 360;
      g_measure.yaw_total += d;
      if (g_status.player_state != 100) g_measure.air_yaw += d;
      g_measure.max_rise = std::max(g_measure.max_rise, y - g_base_y);
    } else {
      g_base_y = y;
    }
    g_last_yaw = yaw;
    g_have_yaw = true;
    g_measure.tracking = true;
  } else if (g_measure_on && g_last_actor != g_track_actor) {
    g_track_addr = 0;
    g_measure.tracking = false;
  }

  if (g_auto_return) {
    g_status.player_state = LocalPlayerState(ctx, base, self);
    if (g_status.player_state == kStateWipeoutGround) {
      ++g_bail_frames;
      if (!g_bail_handled && g_bail_frames >= static_cast<int>(g_auto_return_delay * 60.0f) &&
          g_slots[g_selected].valid && g_pending_goto < 0 && g_force_return_frames == 0) {
        g_pending_goto = g_selected;
        g_bail_handled = true;
        g_status.last_event = "bail -> returning to slot " + std::to_string(g_selected + 1);
        ++g_status.auto_returns;
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
      g_status.last_event = (g_bail_handled && g_bail_frames > 0 ? "bail -> going to slot " : "going to slot ") +
                            std::to_string(g_pending_goto + 1);
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
  if (g_force_set && g_measure_on && has && g_pending_save < 0) {
    static float last = 0;
    static bool have = false;
    const float yaw = std::atan2(LoadF(base, marker + kMarkerXform + 8), LoadF(base, marker + kMarkerXform)) * 57.29578f;
    if (have && g_measure.marker_sets > 0) {
      float d = yaw - last;
      while (d > 180) d -= 360;
      while (d < -180) d += 360;
      g_measure.marker_yaw_total += d;
    }
    last = yaw;
    have = true;
    ++g_measure.marker_sets;
    g_last_seen = ReadXform(base, marker);
  }
  if (g_force_set) {
    g_force_set = false;
    if (has && g_measure_on && !g_track_addr && g_last_actor && g_force_return_frames == 0) {
      // Find the skater matrix: the marker's position, 48 bytes into a 4x4
      // whose first row is a unit vector. Actor first, then one pointer deep.
      const float px = LoadF(base, marker + kMarkerXform + 48), py = LoadF(base, marker + kMarkerXform + 52),
                  pz = LoadF(base, marker + kMarkerXform + 56);
      auto scan = [&](uint32_t from, uint32_t size) -> uint32_t {
        if (!GuestReadable(from, size)) return 0;
        for (uint32_t o = 48; o + 16 <= size; o += 4) {
          if (std::fabs(LoadF(base, from + o) - px) > 0.3f || std::fabs(LoadF(base, from + o + 4) - py) > 1.5f ||
              std::fabs(LoadF(base, from + o + 8) - pz) > 0.3f) {
            continue;
          }
          const float a = LoadF(base, from + o - 48), b = LoadF(base, from + o - 44), c = LoadF(base, from + o - 40);
          const float len = a * a + b * b + c * c;
          if (len > 0.9f && len < 1.1f) return from + o - 48;
        }
        return 0;
      };
      uint32_t found = scan(g_last_actor, 0x2000);
      auto is_ptr = [](uint32_t p) { return p >= 0x40000000 && p < 0x7F000000 && !(p & 3); };
      for (uint32_t o = 0; !found && o < 0x800; o += 4) {
        if (!GuestReadable(g_last_actor + o, 4)) break;
        const uint32_t ptr = REX_LOAD_U32(g_last_actor + o);
        if (!is_ptr(ptr) || !GuestReadable(ptr, 0x800)) continue;
        found = scan(ptr, 0x800);
        for (uint32_t o2 = 0; !found && o2 < 0x200; o2 += 4) {
          const uint32_t p2 = REX_LOAD_U32(ptr + o2);
          if (is_ptr(p2)) found = scan(p2, 0x400);
        }
      }
      g_track_addr = found;
      g_track_actor = g_last_actor;
      REXLOG_INFO("trainer: skater matrix {} (actor 0x{:08X}, addr 0x{:08X})", found ? "found" : "NOT found",
                  g_last_actor, found);
    }
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
void DebugOffsetSlot(int slot, float dx, float dy) {
  std::lock_guard lock(g_mutex);
  auto& s = g_slots[std::clamp(slot, 0, kSlots - 1)];
  if (!s.valid) return;
  auto add = [&](int at, float d) {
    const float f = Be32f(&s.xform[at]) + d;
    uint32_t v;
    std::memcpy(&v, &f, 4);
    s.xform[at] = uint8_t(v >> 24), s.xform[at + 1] = uint8_t(v >> 16),
    s.xform[at + 2] = uint8_t(v >> 8), s.xform[at + 3] = uint8_t(v);
  };
  add(48, dx);
  add(52, dy);
}

void DebugSetPad(bool active, float lx, float ly, float rx, float ry, uint16_t buttons, uint8_t lt, uint8_t rt) {
  std::lock_guard lock(g_mutex);
  auto s16 = [](float v) { return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
  g_pad = {active, s16(lx), s16(ly), s16(rx), s16(ry), buttons, lt, rt};
}

void SetNeverBail(bool on) {
  std::lock_guard lock(g_mutex);
  g_never_bail = on;
}
bool NeverBail() {
  std::lock_guard lock(g_mutex);
  return g_never_bail;
}

void DebugFreezeOrientation(int ticks) {
  std::lock_guard lock(g_mutex);
  g_exp_ticks = ticks;
  g_exp_have = false;
}

void DebugResetMeasure() {
  std::lock_guard lock(g_mutex);
  g_measure_on = true;
  const bool tracking = g_measure.tracking;
  g_measure = {};
  g_measure.tracking = tracking;
  g_have_yaw = false;
  g_measure_states.clear();
}

Measure DebugMeasure() {
  std::lock_guard lock(g_mutex);
  Measure m = g_measure;
  for (int32_t s : g_measure_states) m.states += " " + std::to_string(s);
  return m;
}

void DebugForceGameSet() {
  std::lock_guard lock(g_mutex);
  g_force_set = true;  // the game places its own marker, as if LB + d-pad down
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
  s.timer_hz_live = g_timer_hz_live;
  s.marker_updates = g_marker_updates;
  return s;
}

// The sim timer is also reachable without waiting for the game to change its
// rate: [[0x83083BCC]+16], whose vtable slot 52 is sub_82966910.
uint32_t FindTimer(uint8_t* base) {
  const uint32_t root = REX_LOAD_U32(0x83083BCC);
  if (!GuestReadable(root + 16, 4)) return 0;
  const uint32_t timer = REX_LOAD_U32(root + 16);
  if (!GuestReadable(timer, 40)) return 0;
  const uint32_t vtable = REX_LOAD_U32(timer);
  if (vtable < 0x82000000 || vtable >= 0x831B0000 || !GuestReadable(vtable + 52, 4)) return 0;
  return REX_LOAD_U32(vtable + 52) == 0x82966910 ? timer : 0;
}

void Tick(uint8_t* base) {
  std::lock_guard lock(g_mutex);
  // Only once gameplay runs (the marker update has been seen): the global is
  // not a valid pointer during early boot.
  if (!g_timer && g_status.marker_seen) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      const uint32_t root = REX_LOAD_U32(0x83083BCC);
      const bool r16 = GuestReadable(root + 16, 4);
      const uint32_t timer = r16 ? REX_LOAD_U32(root + 16) : 0;
      const bool rt = GuestReadable(timer, 40);
      const uint32_t vt = rt ? REX_LOAD_U32(timer) : 0;
      const uint32_t fn = GuestReadable(vt + 52, 4) ? REX_LOAD_U32(vt + 52) : 0;
      REXLOG_INFO("trainer: timer chain root={:08X} ok={} timer={:08X} ok={} vtable={:08X} slot52={:08X} hz={}",
                  root, r16, timer, rt, vt, fn, rt ? REX_LOAD_U32(timer + 32) : 0);
    }
    if (const uint32_t timer = FindTimer(base)) {
      g_timer = timer;
      g_requested_hz = static_cast<int32_t>(REX_LOAD_U32(timer + 32));
      if (g_requested_hz <= 0) g_requested_hz = 60;
      g_applied_speed = 1.0f;  // whatever is live now is the game's own rate
      REXLOG_INFO("trainer: sim timer at 0x{:08X}, {} Hz", timer, g_requested_hz);
    }
  }
  if (g_timer) g_timer_hz_live = static_cast<int>(REX_LOAD_U32(g_timer + 32));
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

// Skater state machine: next = Decide(state object, current id). It returns
// the current id for "no change" and 300 to start a bail; the bail request is
// bit 0x00040000 of [[state+4]+2468]. With Never bail on, the request is
// cleared and a 300 result becomes "no change". If that holds for 3 s in a row
// the bail is let through, so the skater can never get stuck.
extern "C" REX_FUNC(__imp__sub_82D8ADE8);
extern "C" REX_FUNC(sub_82D8ADE8) {
  const uint32_t self = ctx.r3.u32, current = ctx.r4.u32;
  bool on;
  {
    std::lock_guard lock(g_mutex);
    on = g_never_bail && current != 300 && GuestReadable(self + 4, 4) && IsLocalBody(base, REX_LOAD_U32(self + 4));
  }
  bool requested = false;
  if (on && GuestReadable(self + 4, 4)) {
    const uint32_t body = REX_LOAD_U32(self + 4);
    if (GuestReadable(body + 2468, 4)) {
      const uint32_t flags = REX_LOAD_U32(body + 2468);
      requested = (flags & 0x00040000u) != 0;
      std::lock_guard lock(g_mutex);
      // A bail asked for on every tick for 2 s is let through (never stuck).
      if (requested && g_block_streak < 120) REX_STORE_U32(body + 2468, flags & ~0x00040000u);
    }
  }
  __imp__sub_82D8ADE8(ctx, base);
  std::lock_guard lock(g_mutex);
  if (!on) return;
  const bool wants_bail = ctx.r3.u32 == 300;
  if (wants_bail && g_block_streak < 120) ctx.r3.u64 = current;
  if (requested || wants_bail) {
    if (g_block_streak++ == 0) {
      ++g_status.bails_blocked;
      REXLOG_INFO("trainer: never bail - refused a bail REQUEST in state {}", current);
    }
    if (g_block_streak == 120) REXLOG_INFO("trainer: never bail - bail asked for 2 s straight, letting it through");
  } else {
    g_block_streak = 0;
  }
}

// Bail detector hand-off: sub_82D86DE8(owner, w) turns "a bail condition was
// detected" (byte w+384, set by the contact / landing / balance checks) into
// the bail request above. With Never bail on, the detection is dropped here
// for the local skater, before anything else reacts to it.
extern "C" REX_FUNC(__imp__sub_82D86DE8);
extern "C" REX_FUNC(sub_82D86DE8) {
  const uint32_t owner = ctx.r3.u32, w = ctx.r4.u32;
  bool drop = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_never_bail && GuestReadable(owner, 4) && GuestReadable(w + 384, 1)) {
      drop = IsLocalBody(base, REX_LOAD_U32(owner)) && REX_LOAD_U8(w + 384) != 0;
      if (drop) {
        REX_STORE_U8(w + 384, 0);
        if (g_detect_streak++ == 0) {
          ++g_status.bails_blocked;
          REXLOG_INFO("trainer: never bail - dropped a detected bail (state {})", g_status.player_state);
        }
      } else {
        g_detect_streak = 0;
      }
    }
  }
  __imp__sub_82D86DE8(ctx, base);
}

// XInputGetState(user, state*) wrapper: scripted pad for the spin test.
extern "C" REX_FUNC(__imp__sub_82EE22C0);
extern "C" REX_FUNC(sub_82EE22C0) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82EE22C0(ctx, base);
  std::lock_guard lock(g_mutex);
  if (user == 0 && state && ctx.r3.u32 == 0) {
    g_status.pad_lt = REX_LOAD_U8(state + 6);
    g_status.pad_rt = REX_LOAD_U8(state + 7);
  }
  if (!g_pad.active || user != 0 || !state) return;
  REX_STORE_U32(state, ++g_pad_packet);
  REX_STORE_U16(state + 4, g_pad.buttons);
  REX_STORE_U8(state + 6, g_pad.lt);
  REX_STORE_U8(state + 7, g_pad.rt);
  REX_STORE_U16(state + 8, static_cast<uint16_t>(g_pad.lx));
  REX_STORE_U16(state + 10, static_cast<uint16_t>(g_pad.ly));
  REX_STORE_U16(state + 12, static_cast<uint16_t>(g_pad.rx));
  REX_STORE_U16(state + 14, static_cast<uint16_t>(g_pad.ry));
  ctx.r3.u64 = 0;
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
