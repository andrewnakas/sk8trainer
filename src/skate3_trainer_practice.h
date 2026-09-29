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
  bool restore_pending = false;
  std::string last_event;
};

// Game speed: 1 = normal. Scales the sim tick frequency; each tick is still
// the game's fixed 1/60 s step, so physics stay stable.
void SetGameSpeed(float speed);
float GameSpeed();

void SaveHere(int slot);            // set the game marker here and copy it to slot
void CaptureGameMarker(int slot);   // copy the game's current marker to slot
void GoTo(int slot);                // teleport through the game's own return path
void Clear(int slot);
void DebugOffsetSlot(int slot, float dx);  // self-test: move a saved slot along x
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
