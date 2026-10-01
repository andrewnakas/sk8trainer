#pragma once

// SK8TRAINER practice tools: game speed, marker slots, auto-return on bail.
// Hooks live in skate3_trainer_practice.cpp; the UI in skate3_trainer.cpp
// talks to them only through this interface.

#include <cstdint>
#include <string>

namespace skate3::trainer::practice {

constexpr int kSlots = 5;

struct SlotInfo {
  bool valid = false;
  float x = 0, y = 0, z = 0;  // translation row of the saved transform
};

struct Status {
  bool marker_seen = false;     // the session-marker update has run
  bool game_marker_set = false; // the game's own marker holds a position
  bool use_gate = false;        // marker+5450: the game allows returning
  int32_t player_state = -1;    // local skater physics state id (300 = ground bail)
  int sim_hz = 0;               // effective sim tick frequency
  int requested_hz = 0;         // what the game asked for
  int timer_hz_live = 0;        // read back from the guest timer object
  uint64_t marker_updates = 0;  // session-marker update calls (one per sim tick)
  uint8_t pad_lt = 0, pad_rt = 0;  // the triggers the game last read
  int bails_blocked = 0;        // bail transitions refused by Never bail
  int feet_kept = 0;            // airs in which a push button was ignored (feet stay on)
  int rescues = 0;              // times Never bail put the skater back to riding from a bail-out pose
  int bounds_ignored = 0;       // out-of-bounds signals ignored
  int auto_returns = 0;         // times a bail triggered auto-return
  bool restore_pending = false;
  bool have_position = false;   // the local skater's transform was read this tick
  float x = 0, y = 0, z = 0;    // world position (metres)
  float speed = 0;              // metres per second
  bool paused = false;
  std::string last_event;
};

// Game speed: 1 = normal. Scales the sim tick frequency; each tick is still
// the game's fixed 1/60 s step, so physics stay stable.
void SetGameSpeed(float speed);
float GameSpeed();

// Pause stops the sim clock (the picture keeps drawing); Step lets it run for
// that many sim ticks (1/60 s each) and pauses again.
void SetPaused(bool on);
bool Paused();
void Step(int ticks);
void BailNow();                     // wipe out on the next sim tick
// Put a position into a slot, keeping its saved facing (or the skater's
// current one when the slot is empty), so GoTo teleports there.
void SetSlotPosition(int slot, float x, float y, float z);

void SaveHere(int slot);            // set the game marker here and copy it to slot
void CaptureGameMarker(int slot);   // copy the game's current marker to slot
void GoTo(int slot);                // teleport through the game's own return path
void Clear(int slot);
// Self-test / audit helpers.
void DebugOffsetSlot(int slot, float dx, float dy = 0.0f);  // move a saved slot
// Scripted pad + skater measurement (spin test). Sticks are -1..1.
void DebugSetPad(bool active, float lx, float ly, float rx, float ry, uint16_t buttons = 0,
                 uint8_t lt = 0, uint8_t rt = 0);
struct Measure {
  bool tracking = false;  // the skater transform was found
  float yaw_total = 0;    // degrees turned (unwrapped, signed) since reset
  float marker_yaw_total = 0;  // same, from the game marker re-set every tick
  int marker_sets = 0;
  float tumble_total = 0; // degrees the skater's up axis moved (flips), unsigned
  float air_yaw = 0, air_tumble = 0;  // same, counted only while state != 100
  float max_rise = 0;     // metres above the height at reset
  int air_ticks = 0;      // sim ticks with state != 100 since reset
  int ticks = 0;
  std::string states;     // state ids seen since reset
};
// Never bail: the skater's state machine is never allowed to enter the bail
// state (300).
void SetNeverBail(bool on);
bool NeverBail();
// Skate / walk out of bounds: the "left the play area" respawn is not raised.
void SetNoBounds(bool on);
bool NoBounds();
// With Never bail: when the game starts its bail-out "running" pose anyway,
// put the skater straight back to riding where they are (the game's own
// skater reset, aimed just ahead of them, speed handed back). Off = that bail
// is let through after 2 s.
void SetRecover(bool on);
bool Recover();
// With Never bail: ignore the push buttons (A / X) while airborne, so a foot
// never comes off the board in the air (landing like that always bails). On
// by default; turn off to do footplants.
void SetKeepFeetOn(bool on);
bool KeepFeetOn();
void DebugFreezeOrientation(int ticks);  // experiment: hold the skater's rotation for N sim ticks
void DebugResetMeasure();
Measure DebugMeasure();
void DebugForceGameSet();  // the game places its own marker (like LB + d-pad down)
SlotInfo Slot(int slot);
int SelectedSlot();
void SelectSlot(int slot);

void SetAutoCapture(bool on);       // game LB+down also fills the selected slot
bool AutoCapture();
void SetAutoReturn(bool on, float delay_seconds);
bool AutoReturn();
float AutoReturnDelay();

Status GetStatus();

// Called once per guest frame from Tick (applies speed changes).
void Tick(uint8_t* base);

}  // namespace skate3::trainer::practice
