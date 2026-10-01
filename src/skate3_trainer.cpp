#include "skate3_trainer.h"
#include "skate3_trainer_practice.h"
#include "skate3_trainer_vault.h"
#include "skate3_trainer_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
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
REXCVAR_DEFINE_BOOL(skate3_trainer_selftest, false, "Skate 3",
                    "SK8TRAINER self-test: once in gameplay, exercise every feature and log "
                    "'trainer selftest:' lines (for unattended checks; restores stock after)");
REXCVAR_DEFINE_BOOL(skate3_trainer_audit, false, "Skate 3",
                    "SK8TRAINER full audit: once in gameplay, test every slider, freeze, preset, "
                    "saved values, game speed, marker slot and auto-return; logs 'trainer audit:' "
                    "lines and writes <user data>/trainer/audit-report.txt. Restores everything after.");
REXCVAR_DEFINE_BOOL(skate3_trainer_spintest, false, "Skate 3",
                    "SK8TRAINER diagnostic: scripted ollie + spin per value group; logs 'trainer "
                    "spintest:' lines with degrees turned.");
REXCVAR_DEFINE_BOOL(skate3_trainer_watch, false, "Skate 3",
                    "SK8TRAINER diagnostic (Windows): log which game functions read the Spin and "
                    "Flips values while you play ('trainer watch:' lines every 2 s). Slows the game.");
REXCVAR_DEFINE_BOOL(skate3_trainer_apply_saved, true, "Skate 3",
                    "Re-apply the saved trainer values (<user data>/trainer/user.toml) whenever the vault is found");

namespace skate3::trainer {
namespace {

using Type = vault::Type;
using Anchor = vault::Anchor;

struct Entry {
  std::string label, group, source;
  Type type = Type::kF32;
  vault::Blob blob = vault::kBin;
  uint32_t offset = 0;
  uint64_t key = 0;          // inline (.vlt) fields: field hash
  uint8_t* direct = nullptr; // inline fields: host address of the live value
  std::vector<uint8_t*> copies;  // inline fields: every copy the game keeps (written together)
  std::vector<uint32_t> graph_y_offsets;  // kGraphScale
  std::vector<float> graph_y_stock;
  double stock = 0, min = 0, max = 1;
  double value = 0;     // last value read from / written to guest memory
  bool frozen = false;  // rewritten every frame
  bool touched = false; // edited by the user this session (saved)
  bool overridden = false;    // a live feature (perfect finish) holds another value right now
  double override_value = 0;
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
std::atomic<uint8_t*> g_vlt{nullptr};     // host address of the vault .vlt (inline fields)
std::vector<Anchor> g_vlt_anchors;
uint32_t g_vlt_size = 0;
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

float LoadBEf(const uint8_t* p) {
  const uint32_t bits = LoadBE32(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreBEf(uint8_t* p, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  StoreBE32(p, bits);
}

bool HostReadable(const uint8_t* p, size_t n);

// `bin` is the located .bin blob; entries that live in the .vlt use g_vlt.
// Returns nullptr when that blob has not been found.
uint8_t* BaseFor(const Entry& e, uint8_t* bin) {
  // Inline fields are located one by one (see LocateInline); the game does
  // not keep the .vlt as one contiguous copy.
  return e.blob == vault::kVlt ? (e.direct ? e.direct - e.offset : nullptr) : bin;
}

double ReadEntry(const Entry& e, uint8_t* bin) {
  uint8_t* base = BaseFor(e, bin);
  if (!base) return e.stock;
  uint8_t* p = base + e.offset;
  if (e.blob == vault::kVlt && !HostReadable(p, 4)) return e.stock;  // partly resident
  switch (e.type) {
    case Type::kF32: return LoadBEf(p);
    case Type::kBool: return p[0] ? 1.0 : 0.0;
    case Type::kI32: return static_cast<int32_t>(LoadBE32(p));
    case Type::kU32: return LoadBE32(p);
    case Type::kGraphScale:
      // The multiplier is the ratio of the first non-zero y to its stock.
      for (size_t k = 0; k < e.graph_y_offsets.size(); ++k) {
        if (e.graph_y_stock[k] != 0) return LoadBEf(p + e.graph_y_offsets[k]) / e.graph_y_stock[k];
      }
      return 1.0;
  }
  return 0;
}

void WriteEntry(const Entry& e, uint8_t* bin, double v) {
  uint8_t* base = BaseFor(e, bin);
  if (!base) return;
  if (e.blob == vault::kVlt && e.copies.size() > 1) {
    // The game keeps more than one copy of some inline rows: write them all.
    for (uint8_t* copy : e.copies) {
      if (copy == e.direct || !HostReadable(copy, 4)) continue;
      if (e.type == Type::kF32) StoreBEf(copy, static_cast<float>(v));
      else if (e.type == Type::kBool) copy[0] = v != 0 ? 1 : 0;
      else StoreBE32(copy, static_cast<uint32_t>(static_cast<int64_t>(std::llround(v))));
    }
  }
  uint8_t* p = base + e.offset;
  if (e.blob == vault::kVlt && !HostReadable(p, 4)) return;
  switch (e.type) {
    case Type::kF32: StoreBEf(p, static_cast<float>(v)); break;
    case Type::kBool: p[0] = v != 0 ? 1 : 0; break;
    case Type::kI32: StoreBE32(p, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v)))); break;
    case Type::kU32: StoreBE32(p, static_cast<uint32_t>(std::max(0.0, std::round(v)))); break;
    case Type::kGraphScale:
      for (size_t k = 0; k < e.graph_y_offsets.size(); ++k) {
        StoreBEf(p + e.graph_y_offsets[k], static_cast<float>(e.graph_y_stock[k] * v));
      }
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
  // The recomp resolves the real game folder late (after config, installers
  // and working-directory checks), so besides the configured roots try the
  // places the engine itself falls back to.
  std::vector<std::filesystem::path> roots = {g_paths.update_data_root, g_paths.game_data_root};
  std::error_code ec;
  roots.push_back(rex::filesystem::GetExecutableFolder() / "game");
  roots.push_back(std::filesystem::current_path(ec) / "game");
  if (!g_paths.user_data_root.empty()) roots.push_back(g_paths.user_data_root / "game");
  std::string tried;
  for (const auto& root : roots) {
    if (root.empty()) continue;
    const auto big = root / "data" / "big" / "db.big";
    tried += "\n  " + big.string();
    if (!std::filesystem::exists(big, ec)) continue;
    table = vault::BuildFromGame(root);
    used = root;
    if (table.error.empty()) break;
  }
  std::lock_guard lock(g_mutex);
  if (used.empty()) {
    g_table_error = "db.big not found; looked in:" + tried;
  } else if (!table.error.empty()) {
    g_table_error = "vault read failed: " + table.error;
  }
  if (!g_table_error.empty()) {
    REXLOG_WARN("trainer: {}", g_table_error);
    return;
  }
  g_bin_size = table.bin_size;
  g_anchors = std::move(table.anchors);
  g_vlt_anchors = std::move(table.vlt_anchors);
  g_vlt_size = table.vlt_size;
  for (vault::Field& f : table.fields) {
    Entry e;
    e.label = std::move(f.label);
    e.group = std::move(f.group);
    e.source = std::move(f.source);
    e.type = f.type;
    e.blob = f.blob;
    e.offset = f.offset;
    e.key = f.key;
    e.graph_y_offsets = std::move(f.graph_y_offsets);
    e.graph_y_stock = std::move(f.graph_y_stock);
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
// True when [p, p+n) is committed, readable guest memory (p is a host
// pointer inside the guest address space).
bool HostReadable(const uint8_t* p, size_t n) {
  uint8_t* base = g_base.load();
  auto* memory = rex::system::kernel_memory();
  if (!base || !memory || p < base || p + n > base + 0x100000000ull) return false;
  const uint32_t guest = static_cast<uint32_t>(p - base);
  auto* heap = memory->LookupHeap(guest);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(guest, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) &&
         (info.protect & rex::memory::kMemoryProtectRead) &&
         uint64_t(guest) + n <= uint64_t(info.base_address) + info.region_size;
}

bool AnchorsMatch(uint8_t* blob, int* count, const std::vector<Anchor>& anchors = g_anchors) {
  int ok = 0;
  for (const Anchor& a : anchors) {
    if (HostReadable(blob + a.offset, a.bytes.size()) &&
        std::memcmp(blob + a.offset, a.bytes.data(), a.bytes.size()) == 0) {
      ++ok;
    }
  }
  if (count) *count = ok;
  return !anchors.empty() && ok * 2 > static_cast<int>(anchors.size());
}

// Finds the host address of a blob whose anchors are `anchors` (best match
// over all read/write guest memory). Returns nullptr when not found.
uint8_t* FindBlob(const std::vector<Anchor>& anchors, uint32_t size, int* best_count_out,
                  uint32_t* hits_out) {
  uint8_t* best = nullptr;
  int best_count = 0;
  uint32_t hits = 0;
  if (anchors.empty() || !size) return nullptr;
  const Anchor& first = anchors.front();
  const auto searcher = std::boyer_moore_horspool_searcher(first.bytes.begin(), first.bytes.end());
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
    // Committed is not enough: thread stacks carry committed no-access guard
    // pages, and touching one faults. The vault is plain read/write heap data.
    constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    if ((info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & kReadWrite) == kReadWrite) {
      uint8_t* region = memory->TranslateVirtual<uint8_t*>(info.base_address);
      uint8_t* region_end = region + info.region_size;
      for (auto it = region;; ++it) {
        it = std::search(it, region_end, searcher);
        if (it == region_end) break;
        uint8_t* blob = it - first.offset;
        int count = 0;
        // Only the anchor windows must be inside this region: the game may
        // keep just part of a blob (the .vlt's entry tables) resident.
        {
          ++hits;
          for (const Anchor& an : anchors) {
            uint8_t* at = blob + an.offset;
            if (at >= region && at + an.bytes.size() <= region_end &&
                std::memcmp(at, an.bytes.data(), an.bytes.size()) == 0) {
              ++count;
            }
          }
          if (count > best_count) {
            best_count = count;
            best = blob;
          }
        }
      }
    }
    a = next > a ? next : a + 0x1000;
  }
  if (best_count_out) *best_count_out = best_count;
  if (hits_out) *hits_out = hits;
  return best && best_count * 2 > static_cast<int>(anchors.size()) ? best : nullptr;
}

// Every read/write guest location holding `pattern` (at most `limit`).
std::vector<uint8_t*> FindAll(const std::vector<uint8_t>& pattern, size_t limit) {
  std::vector<uint8_t*> out;
  const auto searcher = std::boyer_moore_horspool_searcher(pattern.begin(), pattern.end());
  auto* memory = rex::system::kernel_memory();
  uint64_t a = 0x00010000;
  while (memory && a < 0x100000000ull && out.size() < limit) {
    auto* heap = memory->LookupHeap(static_cast<uint32_t>(a));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(static_cast<uint32_t>(a), &info) || !info.region_size) {
      a = (a + 0x10000) & ~0xFFFFull;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & kReadWrite) == kReadWrite) {
      uint8_t* region = memory->TranslateVirtual<uint8_t*>(info.base_address);
      uint8_t* region_end = region + info.region_size;
      for (auto it = region; out.size() < limit; ++it) {
        it = std::search(it, region_end, searcher);
        if (it == region_end) break;
        out.push_back(it);
      }
    }
    a = next > a ? next : a + 0x1000;
  }
  return out;
}

// Finds each inline (.vlt) field's live value: the game keeps collection
// entries as [8-byte field key][4-byte value][type][flags], so the key
// followed by the stock value identifies the row. One pass, 4-byte aligned.
int LocateInline(bool quiet = false) {
  struct Want {
    Entry* e;
    uint8_t key[8];
    uint8_t value[4];   // stock value
    uint8_t value2[4];  // the value the trainer last wrote
    int hits = 0;
    std::vector<uint8_t*> found;
  };
  std::vector<Want> wants;
  {
    std::lock_guard lock(g_mutex);
    for (Entry& e : g_entries) {
      if (e.blob != vault::kVlt) continue;
      Want w{&e};
      {
        uint32_t b2 = 0;
        if (e.type == Type::kF32) {
          float f = static_cast<float>(e.value);
          std::memcpy(&b2, &f, 4);
        } else {
          b2 = static_cast<uint32_t>(static_cast<int64_t>(e.value));
        }
        for (int k = 0; k < 4; ++k) w.value2[k] = uint8_t(b2 >> (24 - 8 * k));
      }
      for (int k = 0; k < 8; ++k) w.key[k] = uint8_t(e.key >> (56 - 8 * k));
      uint32_t bits = 0;
      if (e.type == Type::kF32) {
        float f = static_cast<float>(e.stock);
        std::memcpy(&bits, &f, 4);
      } else {
        bits = static_cast<uint32_t>(static_cast<int64_t>(e.stock));
      }
      for (int k = 0; k < 4; ++k) w.value[k] = uint8_t(bits >> (24 - 8 * k));
      wants.push_back(w);
    }
  }
  if (wants.empty()) return 0;
  auto* memory = rex::system::kernel_memory();
  uint64_t a = 0x00010000;
  while (memory && a < 0x100000000ull) {
    auto* heap = memory->LookupHeap(static_cast<uint32_t>(a));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(static_cast<uint32_t>(a), &info) || !info.region_size) {
      a = (a + 0x10000) & ~0xFFFFull;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & kReadWrite) == kReadWrite &&
        info.region_size >= 12) {
      const uint8_t* region = memory->TranslateVirtual<uint8_t*>(info.base_address);
      const uint8_t* end = region + info.region_size - 12;
      for (const uint8_t* p = region; p <= end; p += 4) {
        for (Want& w : wants) {
          if (p[0] == w.key[0] && std::memcmp(p, w.key, 8) == 0 &&
              (std::memcmp(p + 8, w.value, 4) == 0 || std::memcmp(p + 8, w.value2, 4) == 0)) {
            ++w.hits;
            w.found.push_back(const_cast<uint8_t*>(p + 8));
          }
        }
      }
    }
    a = next > a ? next : a + 0x1000;
  }
  int found = 0;
  std::lock_guard lock(g_mutex);
  uint8_t* bin = g_blob.load();
  for (Want& w : wants) {
    if (w.found.empty()) continue;  // keep what we had
    const bool changed = w.found != w.e->copies;
    w.e->copies = w.found;
    w.e->direct = w.found.front();
    // The game makes fresh copies of these rows (e.g. after a session-marker
    // return): push the player's value into every copy again.
    if (changed && (w.e->frozen || w.e->touched)) WriteEntry(*w.e, bin ? bin : w.e->direct, w.e->value);
    if (changed && quiet) REXLOG_INFO("trainer: inline {} now has {} cop{}", w.e->source, w.hits, w.hits == 1 ? "y" : "ies");
  }
  for (const Want& w : wants) {
    found += w.e->direct != nullptr;
    if (!quiet) {
      REXLOG_INFO("trainer: inline {} -> {} ({} match{})", w.e->source,
                  w.e->direct ? "found" : "NOT found", w.hits, w.hits == 1 ? "" : "es");
    }
  }
  return found;
}

void ScanThread(uint8_t* base) {
  int best_count = 0, vlt_count = 0;
  uint32_t hits = 0;
  uint8_t* best = FindBlob(g_anchors, g_bin_size, &best_count, &hits);
  (void)vlt_count;
  const int inline_found = best ? LocateInline() : 0;
  int inline_total = 0;
  {
    std::lock_guard lock(g_mutex);
    for (const Entry& e : g_entries) inline_total += e.blob == vault::kVlt;
  }
  uint8_t* vlt = inline_found > 0 ? best : nullptr;  // "flips found" marker only
  g_vlt = vlt;
  g_scan_hits = hits;
  if (best) {
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
    std::snprintf(buf, sizeof(buf), "vault at host +0x%llX (%d/%zu anchors, %d/%zu stock values%s found)",
                  static_cast<unsigned long long>(best - base), best_count, g_anchors.size(),
                  matches, g_entries.size(),
                  (", flips " + std::to_string(inline_found) + "/" + std::to_string(inline_total)).c_str());
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
// Perfect finish: the multi-flip lock is off while a grab trigger is held and
// on the moment both are released, so the game completes the flip and lands it.
bool g_perfect_finish = false;

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
  // Presets stack: each one changes only its own values. Stock resets all.
  static const std::vector<Preset> presets = {
      {"Stock", "Every value back to the game's own tuning", {}},
      {"Mega Pop", "Every ollie / jump height x3", {{"/JumpMinHeight", true, 3}, {"/JumpMaxHeight", true, 3}, {"AbsoluteMinHeight", true, 3}, {"#0F2473E9125079F0", true, 3}, {"#1B3E9F9C836D287D", true, 3}, {"#703829BD711E54DE", true, 3}, {"#B2B1170AFFC8AC69", true, 3}, {"#BE3F74F978D777E5", true, 3}}},
      {"Moon", "Pop x2.5, floaty ragdolls, higher hippy jumps",
       {{"/JumpMinHeight", true, 2.5}, {"/JumpMaxHeight", true, 2.5},
        {"physics_biped/default/JumpHeight", true, 2.5},
        {"Wipeout_AirYAcceleration", true, 0.3}, {"Wipeout_GroundYAcceleration", true, 0.3}}},
      {"Never Bail", "Every bail threshold out of reach, bad-landing check off", {{"Wipeout_AirMaxSquash", true, 1000}, {"Wipeout_AirMaxSpeedIntoCollisionNearGrind", true, 1000}, {"Wipeout_GroundMaxSquashCoffin", true, 1000}, {"Wipeout_GroundMaxSquash", true, 1000}, {"Wipeout_OB_MaxSquash", true, 1000}, {"Wipeout_AirSkeletonMaxContactArms", true, 1000}, {"Wipeout_GroundSkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_SkeletonMaxContact", true, 1000}, {"Wipeout_OB_SkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_Air_SkelMaxContact", true, 1000}, {"Wipeout_AirSkeletonMaxDisp", true, 1000}, {"Wipeout_GroundSkeletonMaxDisp", true, 1000}, {"Wipeout_OB_SkeletonMaxDisp", true, 1000}, {"Wipeout_OB_Air_SkelMaxDisp", true, 1000}, {"Wipeout_GroundBalanceBase", true, 1000}, {"Wipeout_GroundOpposingContact", true, 1000}, {"Wipeout_OB_VehicleContact", true, 1000}, {"Wipeout_GroundSkitchingContact", true, 1000}, {"Wipeout_GroundMaxAngularDeckError", true, 1000}, {"Wipeout_GroundLeanContactYThresh", true, 1000}, {"#EE81DD78506E4A2D", true, 1000}, {"#F784AC3BA4422FFD", true, 1000}, {"#C7DDE25FF0D72DA0", true, 1000}, {"#9F1C2EF30C749332", true, 1000}, {"Wipeout_AirMaxSpeedIntoGround", true, 1000}, {"Wipeout_AirMaxSpeedIntoStairs", true, 1000}, {"Wipeout_AirSkeletonMaxContact", true, 1000}, {"Wipeout_GroundSkeletonMaxContact", true, 1000}, {"Wipeout_GroundBalanceTotal", true, 1000}, {"Wipeout_GroundVehicleContact", true, 1000}, {"Wipeout_AirFallingMinUpY", false, -5}, {"Wipeout_AirFallingMaxAngle", false, -5}, {"WipeoutCheckForBadLanding", false, 0}, {"Wipeout_AirBodyFlipScalar", true, 1000}, {"DeckAccBodyFlipScalar", true, 1000}, {"DeckAccPlayerScalar", true, 1000}, {"Wipeout_AirXZTrick", true, 1000}, {"Wipeout_AirYTrick", true, 1000}, {"SkaterSkaterThresholdScalar", true, 1000}, {"Wipeout_GroundVehicleScalar", true, 1000}, {"Wipeout_OB_VehicleScalar", true, 1000}, {"Wipeout_GroundSkitchingScalar", true, 1000}, {"WipeoutGroundLightDMOScalar", true, 1000}, {"WipeoutAirLightDMOScalar", true, 1000}, {"DeckAccSkitchScalar", true, 1000}, {"DeckAccAIScalar", true, 1000}}},
      {"Spin & Flip", "Easy body spins on (the real spin lever, +60%), higher spin caps", {{"PropBodySpinVsTime", false, 2.5}, {"#D7C6855B7814D048", false, 2.5}, {"MaxSpinSpeed", true, 3}, {"MaxAutoBodySpinSpeed", true, 3}, {"EasyBodySpins", false, 1}, {"FlipMaxSpeed", true, 3}, {"FlipScalar", true, 2}}},
      {"Multi Flip", "Double / triple flips in every difficulty: perfect-flip lock off, flip speed x3.3",
       {{"PerfectBodyFlips", false, 0}, {"FlipMaxSpeed", false, 16.5}, {"FlipScalar", false, 4.375},
        {"FlipSpeedSmoothingFactor", false, 1}}},
      {"Auto Land", "Always pulled upright onto the ground, bad landings never bail",
       {{"BodyFlipMinGrabTimeFraction", false, 0}, {"DontAlignAnglePhysicsAir", false, 3.2}, {"SpeedToAlignToGround_PhysAir", true, 5}, {"Wipeout_AirMaxSquash", true, 1000}, {"Wipeout_AirMaxSpeedIntoCollisionNearGrind", true, 1000}, {"Wipeout_GroundMaxSquashCoffin", true, 1000}, {"Wipeout_GroundMaxSquash", true, 1000}, {"Wipeout_OB_MaxSquash", true, 1000}, {"Wipeout_AirSkeletonMaxContactArms", true, 1000}, {"Wipeout_GroundSkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_SkeletonMaxContact", true, 1000}, {"Wipeout_OB_SkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_Air_SkelMaxContact", true, 1000}, {"Wipeout_AirSkeletonMaxDisp", true, 1000}, {"Wipeout_GroundSkeletonMaxDisp", true, 1000}, {"Wipeout_OB_SkeletonMaxDisp", true, 1000}, {"Wipeout_OB_Air_SkelMaxDisp", true, 1000}, {"Wipeout_GroundBalanceBase", true, 1000}, {"Wipeout_GroundOpposingContact", true, 1000}, {"Wipeout_OB_VehicleContact", true, 1000}, {"Wipeout_GroundSkitchingContact", true, 1000}, {"Wipeout_GroundMaxAngularDeckError", true, 1000}, {"Wipeout_GroundLeanContactYThresh", true, 1000}, {"#EE81DD78506E4A2D", true, 1000}, {"#F784AC3BA4422FFD", true, 1000}, {"#C7DDE25FF0D72DA0", true, 1000}, {"#9F1C2EF30C749332", true, 1000}, {"Wipeout_AirMaxSpeedIntoGround", true, 1000}, {"Wipeout_AirMaxSpeedIntoStairs", true, 1000}, {"Wipeout_AirSkeletonMaxContact", true, 1000}, {"Wipeout_GroundSkeletonMaxContact", true, 1000}, {"Wipeout_GroundBalanceTotal", true, 1000}, {"Wipeout_GroundVehicleContact", true, 1000}, {"Wipeout_AirFallingMinUpY", false, -5}, {"Wipeout_AirFallingMaxAngle", false, -5}, {"WipeoutCheckForBadLanding", false, 0}, {"Wipeout_AirBodyFlipScalar", true, 1000}, {"DeckAccBodyFlipScalar", true, 1000}, {"DeckAccPlayerScalar", true, 1000}, {"Wipeout_AirXZTrick", true, 1000}, {"Wipeout_AirYTrick", true, 1000}, {"SkaterSkaterThresholdScalar", true, 1000}, {"Wipeout_GroundVehicleScalar", true, 1000}, {"Wipeout_OB_VehicleScalar", true, 1000}, {"Wipeout_GroundSkitchingScalar", true, 1000}, {"WipeoutGroundLightDMOScalar", true, 1000}, {"WipeoutAirLightDMOScalar", true, 1000}, {"DeckAccSkitchScalar", true, 1000}, {"DeckAccAIScalar", true, 1000}}},
      {"Locked In", "Never bail, never lose the board, auto-land; flips and spins fast but controllable",
       {{"PerfectBodyFlips", false, 0}, {"FlipMaxSpeed", false, 10}, {"FlipScalar", false, 2.5}, {"MaxHeadingAdjustVsUpY", false, 2}, {"MaxAllowedGroundNormalFromUp", false, 180},
        {"FlipSpeedSmoothingFactor", false, 0.5}, {"EasyBodySpins", false, 1}, {"MaxSpinSpeed", true, 2}, {"BodyFlipMinGrabTimeFraction", false, 0}, {"DontAlignAnglePhysicsAir", false, 3.2}, {"SpeedToAlignToGround_PhysAir", true, 5}, {"Wipeout_AirMaxSquash", true, 1000}, {"Wipeout_AirMaxSpeedIntoCollisionNearGrind", true, 1000}, {"Wipeout_GroundMaxSquashCoffin", true, 1000}, {"Wipeout_GroundMaxSquash", true, 1000}, {"Wipeout_OB_MaxSquash", true, 1000}, {"Wipeout_AirSkeletonMaxContactArms", true, 1000}, {"Wipeout_GroundSkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_SkeletonMaxContact", true, 1000}, {"Wipeout_OB_SkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_Air_SkelMaxContact", true, 1000}, {"Wipeout_AirSkeletonMaxDisp", true, 1000}, {"Wipeout_GroundSkeletonMaxDisp", true, 1000}, {"Wipeout_OB_SkeletonMaxDisp", true, 1000}, {"Wipeout_OB_Air_SkelMaxDisp", true, 1000}, {"Wipeout_GroundBalanceBase", true, 1000}, {"Wipeout_GroundOpposingContact", true, 1000}, {"Wipeout_OB_VehicleContact", true, 1000}, {"Wipeout_GroundSkitchingContact", true, 1000}, {"Wipeout_GroundMaxAngularDeckError", true, 1000}, {"Wipeout_GroundLeanContactYThresh", true, 1000}, {"#EE81DD78506E4A2D", true, 1000}, {"#F784AC3BA4422FFD", true, 1000}, {"#C7DDE25FF0D72DA0", true, 1000}, {"#9F1C2EF30C749332", true, 1000}, {"Wipeout_AirMaxSpeedIntoGround", true, 1000}, {"Wipeout_AirMaxSpeedIntoStairs", true, 1000}, {"Wipeout_AirSkeletonMaxContact", true, 1000}, {"Wipeout_GroundSkeletonMaxContact", true, 1000}, {"Wipeout_GroundBalanceTotal", true, 1000}, {"Wipeout_GroundVehicleContact", true, 1000}, {"Wipeout_AirFallingMinUpY", false, -5}, {"Wipeout_AirFallingMaxAngle", false, -5}, {"WipeoutCheckForBadLanding", false, 0}, {"Wipeout_AirBodyFlipScalar", true, 1000}, {"DeckAccBodyFlipScalar", true, 1000}, {"DeckAccPlayerScalar", true, 1000}, {"Wipeout_AirXZTrick", true, 1000}, {"Wipeout_AirYTrick", true, 1000}, {"SkaterSkaterThresholdScalar", true, 1000}, {"Wipeout_GroundVehicleScalar", true, 1000}, {"Wipeout_OB_VehicleScalar", true, 1000}, {"Wipeout_GroundSkitchingScalar", true, 1000}, {"WipeoutGroundLightDMOScalar", true, 1000}, {"WipeoutAirLightDMOScalar", true, 1000}, {"DeckAccSkitchScalar", true, 1000}, {"DeckAccAIScalar", true, 1000}}},
      {"Land Any Angle", "Sideways or crooked landings still roll away: heading auto-correct x2, landing-angle bails off",
       {{"MaxHeadingAdjustVsUpY", false, 2}, {"MaxAllowedGroundNormalFromUp", false, 180}, {"DontAlignAnglePhysicsAir", false, 3.2}, {"SpeedToAlignToGround_PhysAir", true, 5}, {"WipeoutCheckForBadLanding", false, 0}, {"Wipeout_GroundMaxAngularDeckError", true, 1000}, {"Wipeout_AirFallingMinUpY", false, -5}, {"Wipeout_AirFallingMaxAngle", false, -5}, {"Wipeout_GroundBalanceTotal", true, 1000}, {"Wipeout_GroundBalanceBase", true, 1000}, {"Wipeout_GroundLeanContactYThresh", true, 1000}}},
      {"Fast", "Push top speed x4, push power x4, run speed x3", {{"MaxPushableSpeed", true, 4}, {"#501D5581043D7D3C", true, 4}, {"MaxPushDVStart", true, 4}, {"MaxPushDVEnd", true, 4}, {"SpeedVsInput", false, 3}, {"#CE8C0D4C6B92FD27", false, 3}}},
      {"Big Air", "Mega Pop + Spin & Flip + Multi Flip + Fast + Never Bail", {{"PerfectBodyFlips", false, 0}, {"FlipSpeedSmoothingFactor", false, 1}, {"/JumpMinHeight", true, 3}, {"/JumpMaxHeight", true, 3}, {"AbsoluteMinHeight", true, 3}, {"#0F2473E9125079F0", true, 3}, {"#1B3E9F9C836D287D", true, 3}, {"#703829BD711E54DE", true, 3}, {"#B2B1170AFFC8AC69", true, 3}, {"#BE3F74F978D777E5", true, 3}, {"PropBodySpinVsTime", false, 2.5}, {"#D7C6855B7814D048", false, 2.5}, {"MaxSpinSpeed", true, 3}, {"MaxAutoBodySpinSpeed", true, 3}, {"EasyBodySpins", false, 1}, {"FlipMaxSpeed", true, 3}, {"FlipScalar", true, 2}, {"MaxPushableSpeed", true, 4}, {"#501D5581043D7D3C", true, 4}, {"MaxPushDVStart", true, 4}, {"MaxPushDVEnd", true, 4}, {"SpeedVsInput", false, 3}, {"#CE8C0D4C6B92FD27", false, 3}, {"Wipeout_AirMaxSquash", true, 1000}, {"Wipeout_AirMaxSpeedIntoCollisionNearGrind", true, 1000}, {"Wipeout_GroundMaxSquashCoffin", true, 1000}, {"Wipeout_GroundMaxSquash", true, 1000}, {"Wipeout_OB_MaxSquash", true, 1000}, {"Wipeout_AirSkeletonMaxContactArms", true, 1000}, {"Wipeout_GroundSkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_SkeletonMaxContact", true, 1000}, {"Wipeout_OB_SkeletonMaxContactArms", true, 1000}, {"Wipeout_OB_Air_SkelMaxContact", true, 1000}, {"Wipeout_AirSkeletonMaxDisp", true, 1000}, {"Wipeout_GroundSkeletonMaxDisp", true, 1000}, {"Wipeout_OB_SkeletonMaxDisp", true, 1000}, {"Wipeout_OB_Air_SkelMaxDisp", true, 1000}, {"Wipeout_GroundBalanceBase", true, 1000}, {"Wipeout_GroundOpposingContact", true, 1000}, {"Wipeout_OB_VehicleContact", true, 1000}, {"Wipeout_GroundSkitchingContact", true, 1000}, {"Wipeout_GroundMaxAngularDeckError", true, 1000}, {"Wipeout_GroundLeanContactYThresh", true, 1000}, {"#EE81DD78506E4A2D", true, 1000}, {"#F784AC3BA4422FFD", true, 1000}, {"#C7DDE25FF0D72DA0", true, 1000}, {"#9F1C2EF30C749332", true, 1000}, {"Wipeout_AirMaxSpeedIntoGround", true, 1000}, {"Wipeout_AirMaxSpeedIntoStairs", true, 1000}, {"Wipeout_AirSkeletonMaxContact", true, 1000}, {"Wipeout_GroundSkeletonMaxContact", true, 1000}, {"Wipeout_GroundBalanceTotal", true, 1000}, {"Wipeout_GroundVehicleContact", true, 1000}, {"Wipeout_AirFallingMinUpY", false, -5}, {"Wipeout_AirFallingMaxAngle", false, -5}, {"WipeoutCheckForBadLanding", false, 0}, {"Wipeout_AirBodyFlipScalar", true, 1000}, {"DeckAccBodyFlipScalar", true, 1000}, {"DeckAccPlayerScalar", true, 1000}, {"Wipeout_AirXZTrick", true, 1000}, {"Wipeout_AirYTrick", true, 1000}, {"SkaterSkaterThresholdScalar", true, 1000}, {"Wipeout_GroundVehicleScalar", true, 1000}, {"Wipeout_OB_VehicleScalar", true, 1000}, {"Wipeout_GroundSkitchingScalar", true, 1000}, {"WipeoutGroundLightDMOScalar", true, 1000}, {"WipeoutAirLightDMOScalar", true, 1000}, {"DeckAccSkitchScalar", true, 1000}, {"DeckAccAIScalar", true, 1000}}},
      {"THPS", "Fast spins, easy body spins, auto push",
       {{"MaxSpinSpeed", true, 3}, {"MaxAutoBodySpinSpeed", true, 3}, {"EasyBodySpins", false, 1},
        {"AutoPushEnabled", false, 1}, {"MaxPushableSpeed", true, 2}}},
  };
  return presets;
}

void ApplyPresetLocked(const Preset& preset) {
  uint8_t* blob = g_blob.load();
  if (preset.rules.empty()) g_perfect_finish = false;
  if (std::string(preset.name) == "Locked In") g_perfect_finish = true;
  for (Entry& e : g_entries) {
    double v = e.stock;
    bool hit = false;
    for (const PresetRule& r : preset.rules) {
      if (e.source.find(r.source_part) != std::string::npos) {
        v = r.multiply ? e.stock * r.value : r.value;
        hit = true;
      }
    }
    if (!hit && !preset.rules.empty()) continue;  // presets stack; Stock resets
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

// The game keeps one physics_mode collection per difficulty (easy / normal /
// hardcore / ...) and switches between them, so an edit to one is copied to
// the same field of every mode: the tuning survives a difficulty change.
bool g_link_modes = true;

void LinkModesLocked(const Entry& src) {
  static const std::string kClass = "physics_mode/";
  if (!g_link_modes || src.source.compare(0, kClass.size(), kClass) != 0) return;
  const std::string field = src.source.substr(src.source.rfind('/'));
  uint8_t* blob = g_blob.load();
  for (Entry& e : g_entries) {
    if (&e == &src || e.source.compare(0, kClass.size(), kClass) != 0) continue;
    if (e.source.size() < field.size() || e.source.compare(e.source.size() - field.size(), field.size(), field) != 0) continue;
    e.value = src.value;
    e.frozen = src.frozen;
    e.touched = true;
    if (blob) WriteEntry(e, blob, e.value);
  }
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
    LinkModesLocked(e);
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
  ImGui::Checkbox("Perfect finish: flip only while grabbing; release any time = stop + square up", &g_perfect_finish);
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

// Unattended check, driven from Tick once the vault is found and gameplay
// (the session-marker update) is running. Every step logs one line.
void SelfTest() {
  namespace pr = practice;
  using clock = std::chrono::steady_clock;
  static int step = 0;
  static clock::time_point at;
  static uint64_t updates_at = 0;
  static double rate_normal = 0;
  const auto now = clock::now();
  const double waited = std::chrono::duration<double>(now - at).count();
  const pr::Status st = pr::GetStatus();
  auto next = [&](int n) {
    step = n;
    at = now;
    updates_at = st.marker_updates;
  };
  auto find = [](const char* source) -> Entry* {
    for (Entry& e : g_entries) {
      if (e.source == source) return &e;
    }
    return nullptr;
  };
  switch (step) {
    case 0:
      if (g_blob.load() && st.marker_seen) {
        REXLOG_INFO("trainer selftest: start - {} | {} entries", g_status, g_entries.size());
        next(1);
      }
      break;
    case 1:  // live edit + readback
      if (waited < 3) break;
      if (Entry* e = find("physics_mode/normal/JumpMaxHeight")) {
        std::lock_guard lock(g_mutex);
        const double before = ReadEntry(*e, g_blob.load());
        WriteEntry(*e, g_blob.load(), e->stock * 3);
        const double after = ReadEntry(*e, g_blob.load());
        REXLOG_INFO("trainer selftest: edit JumpMaxHeight[normal] {:.3f} -> {:.3f} (stock {:.3f}) {}",
                    before, after, e->stock,
                    std::fabs(after - e->stock * 3) < 1e-3 ? "OK" : "FAIL");
        WriteEntry(*e, g_blob.load(), e->stock);
      } else {
        REXLOG_INFO("trainer selftest: edit FAIL (JumpMaxHeight[normal] not in table)");
      }
      next(2);
      break;
    case 2:  // update rate at normal speed
      if (waited < 4) break;
      rate_normal = (st.marker_updates - updates_at) / waited;
      REXLOG_INFO("trainer selftest: speed 1.00 -> marker updates {:.1f}/s, timer {} Hz (game asked {})",
                  rate_normal, st.timer_hz_live, st.requested_hz);
      pr::SetGameSpeed(0.5f);
      next(3);
      break;
    case 3:  // update rate at half speed
      if (waited < 4) break;
      {
        const double rate = (st.marker_updates - updates_at) / waited;
        REXLOG_INFO("trainer selftest: speed 0.50 -> marker updates {:.1f}/s, timer {} Hz ({:.0f}% of normal) {}",
                    rate, st.timer_hz_live, rate_normal > 0 ? 100 * rate / rate_normal : 0.0,
                    st.timer_hz_live == 30 && rate < rate_normal * 0.75 ? "OK" : "CHECK");
      }
      pr::SetGameSpeed(1.0f);
      pr::SetAutoReturn(true, 1.0f);  // exercises the local-player state read
      pr::SaveHere(0);
      next(4);
      break;
    case 4:  // marker slot save
      if (waited < 2) break;
      {
        const pr::SlotInfo slot = pr::Slot(0);
        REXLOG_INFO("trainer selftest: save slot 1 -> {} valid={} pos=({:.2f}, {:.2f}, {:.2f}) | state {} | return {} {}",
                    st.last_event, slot.valid, slot.x, slot.y, slot.z, st.player_state,
                    st.use_gate ? "allowed" : "blocked", slot.valid ? "OK" : "FAIL");
      }
      pr::DebugOffsetSlot(0, 4.0f);  // returning to where you stand is a no-op
      pr::GoTo(0);
      next(5);
      break;
    case 5:  // go to slot
      if (waited < 5) break;
      REXLOG_INFO("trainer selftest: go slot 1 -> '{}' pending={} {}", st.last_event,
                  st.restore_pending,
                  st.last_event.find("going to slot") != std::string::npos &&
                          st.last_event.find("timed out") == std::string::npos
                      ? "OK"
                      : "CHECK");
      pr::SetAutoReturn(false, 1.0f);
      pr::Clear(0);
      REXLOG_INFO("trainer selftest: done (stock values restored)");
      next(6);
      break;
    default:
      break;
  }
}


// Full audit (skate3_trainer_audit). Runs from Tick on the guest thread, one
// step at a time, and leaves the game and user.toml as it found them.
void Audit() {
  namespace pr = practice;
  using clock = std::chrono::steady_clock;
  struct Result {
    std::string name;
    bool pass;
    std::string detail;
  };
  static std::vector<Result> results;
  static std::vector<std::string> skipped;
  static int step = 0;
  static clock::time_point at;
  static uint64_t updates_at = 0;
  static std::vector<double> test_values;
  static std::string user_backup;
  static bool had_user = false;
  static size_t preset_index = 0;
  static size_t speed_index = 0;
  static std::set<int32_t> states_seen;
  static bool bail_seen = false;
  const auto now = clock::now();
  const double waited = std::chrono::duration<double>(now - at).count();
  const pr::Status st = pr::GetStatus();
  uint8_t* blob = g_blob.load();
  auto next = [&](int n) {
    step = n;
    at = now;
    updates_at = st.marker_updates;
  };
  auto record = [&](std::string name, bool pass, std::string detail) {
    REXLOG_INFO("trainer audit: {} {} {}", pass ? "PASS" : "FAIL", name, detail);
    results.push_back({std::move(name), pass, std::move(detail)});
  };
  auto close_to = [](double a, double b) { return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b)); };
  auto test_value = [](const Entry& e) {
    switch (e.type) {
      case Type::kBool: return e.stock != 0 ? 0.0 : 1.0;
      case Type::kI32:
      case Type::kU32: return e.stock + 1;
      default: return e.stock == 0 ? 0.5 : e.stock * 1.5;
    }
  };
  auto restore_all_locked = [&]() {
    for (Entry& e : g_entries) {
      e.frozen = false;
      e.touched = false;
      e.value = e.stock;
      if (blob) WriteEntry(e, blob, e.stock);
    }
  };
  if (st.player_state >= 0) {
    states_seen.insert(st.player_state);
    if (st.player_state == 300) bail_seen = true;
  }

  switch (step) {
    case 0:  // wait for the vault and gameplay
      if (blob && st.marker_seen) {
        REXLOG_INFO("trainer audit: start - {} | {} entries", g_status, g_entries.size());
        std::error_code ec;
        had_user = std::filesystem::exists(UserPath(), ec);
        if (had_user) {
          std::ifstream in(UserPath());
          user_backup.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        next(1);
      }
      break;

    case 1: {  // every slider: start at stock, write a test value, read it back
      if (waited < 3) break;
      std::lock_guard lock(g_mutex);
      int at_stock = 0, readback = 0;
      std::string bad;
      test_values.clear();
      for (Entry& e : g_entries) {
        // Stock, or the player's own saved value re-applied from user.toml.
        if (close_to(ReadEntry(e, blob), e.stock) || (e.touched && close_to(ReadEntry(e, blob), e.value))) ++at_stock;
        const double v = test_value(e);
        test_values.push_back(v);
        WriteEntry(e, blob, v);
        if (close_to(ReadEntry(e, blob), v)) ++readback;
        else bad += " " + e.source;
      }
      const int n = static_cast<int>(g_entries.size());
      int vlt_entries = 0;
      for (const Entry& e : g_entries) vlt_entries += e.blob == vault::kVlt;
      record("flip/off-board values located (.vlt)", g_vlt.load() != nullptr,
             std::to_string(vlt_entries) + " entries live there");
      record("sliders start at stock (or your saved value)", at_stock == n, std::to_string(at_stock) + "/" + std::to_string(n));
      record("sliders write + read back", readback == n, std::to_string(readback) + "/" + std::to_string(n) + bad);
      next(2);
      break;
    }

    case 2: {  // the game must not overwrite them, then restore stock
      if (waited < 1.5) break;
      std::lock_guard lock(g_mutex);
      int kept = 0, restored = 0;
      std::string bad;
      for (size_t i = 0; i < g_entries.size(); ++i) {
        Entry& e = g_entries[i];
        if (close_to(ReadEntry(e, blob), test_values[i])) ++kept;
        else bad += " " + e.source;
        WriteEntry(e, blob, e.stock);
        if (close_to(ReadEntry(e, blob), e.stock)) ++restored;
      }
      const int n = static_cast<int>(g_entries.size());
      record("sliders hold for 1.5 s in gameplay", kept == n, std::to_string(kept) + "/" + std::to_string(n) + bad);
      record("sliders restore to stock", restored == n, std::to_string(restored) + "/" + std::to_string(n));
      // Freeze: freeze one value, then let "the game" overwrite it.
      Entry& e = g_entries.front();
      e.frozen = true;
      e.value = test_value(e);
      WriteEntry(e, blob, e.stock);  // simulated overwrite
      next(3);
      break;
    }

    case 3: {  // freeze rewrites within a frame
      if (waited < 0.3) break;
      std::lock_guard lock(g_mutex);
      Entry& e = g_entries.front();
      const double v = ReadEntry(e, blob);
      record("freeze re-applies after an overwrite", close_to(v, e.value), e.source + " = " + std::to_string(v));
      e.frozen = false;
      e.value = e.stock;
      WriteEntry(e, blob, e.stock);
      preset_index = 0;
      next(4);
      break;
    }

    case 4: {  // every preset matches its rules
      if (waited < 0.5) break;
      std::lock_guard lock(g_mutex);
      const auto& presets = Presets();
      if (preset_index < presets.size()) {
        const Preset& preset = presets[preset_index];
        ApplyPresetLocked(presets.front());
        ApplyPresetLocked(preset);
        int ok = 0, hits = 0;
        std::string bad;
        for (Entry& e : g_entries) {
          double expect = e.stock;
          bool hit = false;
          for (const PresetRule& r : preset.rules) {
            if (e.source.find(r.source_part) != std::string::npos) {
              expect = r.multiply ? e.stock * r.value : r.value;
              hit = true;
            }
          }
          hits += hit;
          if (close_to(ReadEntry(e, blob), expect) && e.frozen == hit) ++ok;
          else bad += " " + e.source;
        }
        const int n = static_cast<int>(g_entries.size());
        record(std::string("preset ") + preset.name, ok == n && (hits > 0 || preset.rules.empty()),
               std::to_string(hits) + " values changed, " + std::to_string(ok) + "/" + std::to_string(n) + " correct" + bad);
        ++preset_index;
        at = now;
        break;
      }
      ApplyPresetLocked(presets.front());  // Stock
      int stock = 0;
      for (Entry& e : g_entries) stock += close_to(ReadEntry(e, blob), e.stock) && !e.frozen;
      record("Stock preset resets everything", stock == static_cast<int>(g_entries.size()),
             std::to_string(stock) + "/" + std::to_string(g_entries.size()));
      next(5);
      break;
    }

    case 5: {  // saved values: save, reset, re-apply
      std::lock_guard lock(g_mutex);
      Entry& e = g_entries.front();
      const double v = test_value(e);
      e.value = v;
      e.touched = true;
      e.frozen = true;
      SaveUser();
      e.value = e.stock;
      e.touched = false;
      e.frozen = false;
      WriteEntry(e, blob, e.stock);
      ApplySavedLocked(blob);
      const bool ok = close_to(ReadEntry(e, blob), v) && e.frozen;
      record("saved values round-trip (user.toml)", ok, e.source + " -> " + std::to_string(ReadEntry(e, blob)) +
                                                           (e.frozen ? " frozen" : " not frozen"));
      restore_all_locked();
      speed_index = 0;
      pr::SetGameSpeed(0.25f);
      next(6);
      break;
    }

    case 6: {  // game speed: measured sim update rate
      static const float speeds[] = {0.25f, 0.5f, 1.0f, 2.0f};
      static int retries = 0;
      if (waited < 4) break;
      const double rate = (st.marker_updates - updates_at) / waited;
      const double expect = 60.0 * speeds[speed_index];
      // The marker update pauses while the game is busy (a reset after the
      // slider test, a teleport): re-measure instead of failing on a stall.
      // Zero updates = the game is paused (e.g. its window lost focus): wait
      // that out (up to ~30 s) without spending a retry.
      static int paused_waits = 0;
      if (rate == 0 && paused_waits < 7) {
        ++paused_waits;
        REXLOG_INFO("trainer audit: speed {:.2f}x: game paused, waiting", speeds[speed_index]);
        next(6);
        break;
      }
      paused_waits = 0;
      if (std::fabs(rate - expect) > expect * 0.15 && retries < 3) {
        ++retries;
        REXLOG_INFO("trainer audit: speed {:.2f}x measured {:.1f}/s, re-measuring", speeds[speed_index], rate);
        next(6);
        break;
      }
      retries = 0;
      char detail[96];
      std::snprintf(detail, sizeof(detail), "%.2fx: %.1f updates/s (expected %.0f), timer %d Hz",
                    speeds[speed_index], rate, expect, st.timer_hz_live);
      record(std::string("game speed ") + detail, std::fabs(rate - expect) <= expect * 0.15, "");
      if (++speed_index < 4) {
        pr::SetGameSpeed(speeds[speed_index]);
        next(6);
      } else {
        pr::SetGameSpeed(1.0f);
        for (int i = 0; i < pr::kSlots; ++i) pr::Clear(i);
        pr::SetAutoCapture(false);
        pr::SaveHere(0);
        next(7);
      }
      break;
    }

    case 7:  // save here
      if (waited < 1) break;
      record("slot save here", pr::Slot(0).valid, st.last_event);
      pr::CaptureGameMarker(2);  // the game marker now holds slot 1's spot
      next(8);
      break;

    case 8:  // copy game marker
      if (waited < 1) break;
      record("slot copy game marker", pr::Slot(2).valid, st.last_event);
      pr::Clear(2);
      record("slot clear", !pr::Slot(2).valid, "");
      pr::SelectSlot(1);
      pr::SetAutoCapture(true);
      pr::DebugForceGameSet();  // the game places its own marker (LB + d-pad down)
      next(9);
      break;

    case 9:  // auto-capture into the selected slot
      if (waited < 1) break;
      record("slot auto-capture from the game marker", pr::Slot(1).valid, st.last_event);
      pr::SetAutoCapture(false);
      pr::SelectSlot(0);
      pr::DebugOffsetSlot(0, 4.0f);
      pr::GoTo(0);
      next(10);
      break;

    case 10:  // go to a slot 4 m away
      if (waited < 5) break;
      record("slot go (teleport through the game's return)",
             st.last_event.find("going to slot 1") != std::string::npos &&
                 st.last_event.find("timed out") == std::string::npos,
             st.last_event);
      pr::SaveHere(0);  // re-save where we landed: the auto-return target
      next(11);
      break;

    case 11: {  // force a real bail by zeroing the bail thresholds (frozen)
      if (waited < 1.5) break;
      std::lock_guard lock(g_mutex);
      int zeroed = 0;
      for (Entry& e : g_entries) {
        if (e.group != "Bails" || e.source.find("Wipeout_") == std::string::npos ||
            e.source.find("FallingMinUpY") != std::string::npos ||
            e.source.find("YAcceleration") != std::string::npos) {
          continue;
        }
        e.value = 0;
        e.frozen = true;
        WriteEntry(e, blob, 0);
        ++zeroed;
      }
      REXLOG_INFO("trainer audit: zeroed {} bail thresholds to force a bail", zeroed);
      pr::SelectSlot(0);
      pr::SetAutoReturn(true, 1.0f);
      states_seen.clear();
      bail_seen = false;
      next(13);
      break;
    }

    case 13: {  // bail, then auto-return
      {
        static clock::time_point last_trace;
        static uint64_t last_updates = 0;
        if (std::chrono::duration<double>(now - last_trace).count() >= 0.5) {
          REXLOG_INFO("trainer audit: trace t={:.1f}s state={} marker_updates+{} event='{}'", waited,
                      st.player_state, st.marker_updates - last_updates, st.last_event);
          last_trace = now;
          last_updates = st.marker_updates;
        }
      }
      const bool returned = st.auto_returns > 0;
      {
        // Standing still does not always bail: provoke one with a short
        // teleport every 5 s while the thresholds are zeroed.
        static double kicked_at = 0;
        if (!bail_seen && waited - kicked_at >= 5) {
          kicked_at = waited;
          pr::DebugOffsetSlot(0, 3.0f);
          pr::GoTo(0);
        }
        if (waited < 1) kicked_at = 0;
      }
      if (!returned && waited < 30) break;
      std::string seen;
      for (int32_t s : states_seen) seen += " " + std::to_string(s);
      if (!bail_seen) {
        // Not a trainer failure: the audit could not make the skater bail.
        REXLOG_INFO("trainer audit: SKIP bail + auto-return (no bail happened in 30 s; states seen:{})", seen);
        skipped.push_back("bail + auto-return (no bail happened in 30 s)");
      } else {
        record("Bails sliders cause a bail at 0 (state 300 seen)", true, "states seen:" + seen);
        record("auto-return to the selected slot after the bail", returned,
               std::to_string(st.auto_returns) + " auto-return(s), last: " + st.last_event);
      }
      next(14);
      break;
    }

    case 14: {  // clean up and report
      if (waited < 3) break;
      pr::SetAutoReturn(false, 1.0f);
      for (int i = 0; i < pr::kSlots; ++i) pr::Clear(i);
      pr::SetGameSpeed(1.0f);
      {
        std::lock_guard lock(g_mutex);
        restore_all_locked();
        std::error_code ec;
        if (had_user) {
          std::ofstream(UserPath()) << user_backup;
        } else {
          std::filesystem::remove(UserPath(), ec);
        }
        if (REXCVAR_GET(skate3_trainer_apply_saved) && had_user) ApplySavedLocked(blob);
      }
      int pass = 0;
      std::string report = "SK8TRAINER audit\n";
      for (const Result& r : results) {
        pass += r.pass;
        report += std::string(r.pass ? "PASS  " : "FAIL  ") + r.name + (r.detail.empty() ? "" : "  -- " + r.detail) + "\n";
      }
      for (const std::string& sk : skipped) report += "SKIP  " + sk + "\n";
      report += std::to_string(pass) + "/" + std::to_string(results.size()) + " checks passed\n";
      std::error_code ec;
      std::filesystem::create_directories(TrainerFolder(), ec);
      std::ofstream(TrainerFolder() / "audit-report.txt") << report;
      REXLOG_INFO("trainer audit: done {}/{} passed (report: {})", pass, results.size(),
                  (TrainerFolder() / "audit-report.txt").string());
      next(15);
      break;
    }

    default:
      break;
  }
}


// Spin test (skate3_trainer_spintest): scripted ollie + held left stick, with
// one group of values changed per trial; logs degrees turned in the air.
void SpinTest() {
  namespace pr = practice;
  struct Change {
    const char* part;
    bool multiply;
    double v;
  };
  struct Trial {
    const char* name;
    float lx, ly;     // left stick held in the air
    uint8_t lt, rt;   // triggers held in the air
    uint16_t buttons;
    std::vector<Change> changes;
    bool pre = false;  // also hold the stick / buttons through the crouch and pop
  };
#define POP {"/JumpMinHeight", true, 3}, {"/JumpMaxHeight", true, 3}, {"AbsoluteMinHeight", true, 3}
#define SPIN {"PropBodySpinVsTime", false, 3}, {"#D7C6855B7814D048", false, 3}, {"MaxSpinSpeed", true, 3}
#define FLIP {"FlipMaxSpeed", true, 4}, {"FlipScalar", true, 4}
#define UFLIP {"FlipMaxSpeed", false, 16.5}, {"FlipScalar", false, 4.375}, {"FlipSpeedSmoothingFactor", false, 1}, {"FlipBodySpinScalar", false, 1.1}
  static const std::vector<Trial> trials = {
      {"1 no teleport, flip values", 0, -1, 0, 0, 0x0200, {POP, UFLIP}, true},
      {"2 after teleport, flip values", 0, -1, 0, 0, 0x0200, {POP, UFLIP}, true},
      {"3 after teleport, flip values", 0, -1, 0, 0, 0x0200, {POP, UFLIP}, true},
      {"4 stock flip", 0, -1, 0, 0, 0x0200, {POP}, true},
      {"5 flip values again", 0, -1, 0, 0, 0x0200, {POP, UFLIP}, true},
  };
  static int step = 0;
  static size_t trial = 0;
  static uint64_t tick0 = 0;
  uint8_t* blob = g_blob.load();
  const pr::Status st = pr::GetStatus();
  const uint64_t t = st.marker_updates - tick0;
  auto next = [&](int n) {
    step = n;
    tick0 = st.marker_updates;
  };
  auto set_all_stock = [&]() {
    std::lock_guard lock(g_mutex);
    for (Entry& e : g_entries) {
      e.frozen = false;
      e.value = e.stock;
      WriteEntry(e, blob, e.stock);
    }
  };
  switch (step) {
    case 0:
      if (!blob || !st.marker_seen || t < 240) break;
      pr::DebugResetMeasure();
      pr::SetAutoCapture(false);
      pr::SaveHere(0);
      next(1);
      break;
    case 1:  // apply the trial's values, go back to the start spot
      if (t < 90) break;
      if (trial >= trials.size()) {
        set_all_stock();
        pr::DebugSetPad(false, 0, 0, 0, 0);
        REXLOG_INFO("trainer spintest: done");
        next(99);
        break;
      }
      set_all_stock();
      {
        std::lock_guard lock(g_mutex);
        for (Entry& e : g_entries) {
          for (const Change& c : trials[trial].changes) {
            if (e.source.find(c.part) == std::string::npos) continue;
            e.value = c.multiply ? e.stock * c.v : c.v;
            WriteEntry(e, blob, e.value);
          }
        }
      }
      if (trial > 0) pr::GoTo(0);
      next(2);
      break;
    case 2:  // settle, then crouch (right stick down)
      if (t < 420) break;
      if (trials[trial].pre) pr::DebugSetPad(true, trials[trial].lx, trials[trial].ly, 0, -1, trials[trial].buttons, trials[trial].lt, trials[trial].rt);
      else pr::DebugSetPad(true, 0, 0, 0, -1);
      next(3);
      break;
    case 3:  // pop (right stick up) and hold the left stick left
      if (t < 30) break;
      if (trials[trial].pre) pr::DebugSetPad(true, trials[trial].lx, trials[trial].ly, 0, 1, trials[trial].buttons, trials[trial].lt, trials[trial].rt);
      else pr::DebugSetPad(true, 0, 0, 0, 1);
      pr::DebugResetMeasure();
      next(4);
      break;
    case 4:
      if (t < 8) break;
      pr::DebugSetPad(true, trials[trial].lx, trials[trial].ly, 0, 0, trials[trial].buttons, trials[trial].lt, trials[trial].rt);
      next(5);
      break;
    case 5: {  // through the air and the landing
      if (t < 200) break;
      const pr::Measure m = pr::DebugMeasure();
      REXLOG_INFO("trainer spintest: {:28} AIR yaw {:8.1f} tumble {:7.1f} | all yaw {:8.1f} tumble {:7.1f}  rise {:5.2f} m  air {:3}/{} ticks  tracking {}  states{}",
                  trials[trial].name, m.air_yaw, m.air_tumble, m.yaw_total, m.tumble_total, m.max_rise, m.air_ticks, m.ticks, m.tracking, m.states);
      pr::DebugSetPad(true, 0, 0, 0, 0);
      ++trial;
      next(1);
      break;
    }
    default:
      break;
  }
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
    bool inline_moved = false;
    {
      std::lock_guard lock(g_mutex);
      for (const Entry& e : g_entries) {
        if (e.blob != vault::kVlt || !e.direct) continue;
        uint8_t key[8];
        for (int k = 0; k < 8; ++k) key[k] = uint8_t(e.key >> (56 - 8 * k));
        if (!HostReadable(e.direct - 8, 12) || std::memcmp(e.direct - 8, key, 8) != 0) inline_moved = true;
      }
    }
    if ((blob && !AnchorsMatch(blob, nullptr)) || inline_moved) {
      REXLOG_INFO("trainer: vault moved or unloaded, rescanning");
      g_blob = nullptr;
      g_locate = Locate::kIdle;
    }
    if (!g_blob.load()) StartScan(base);
  }
  // Re-find the inline rows every ~2 s in the background.
  if (frame % 120 == 60 && g_blob.load()) {
    static std::atomic<bool> busy{false};
    if (!busy.exchange(true)) {
      std::thread([] {
        LocateInline(true);
        busy = false;
      }).detach();
    }
  }
  if (uint8_t* blob = g_blob.load()) {
    std::lock_guard lock(g_mutex);
    for (Entry& e : g_entries) {
      // Anything the player set stays set: the game re-creates some rows
      // (session-marker return) and would silently go back to stock.
      if (e.overridden) {
        WriteEntry(e, blob, e.override_value);
      } else if (e.frozen || e.touched) {
        if (frame % 6 == 0 && e.type != Type::kGraphScale) {
          const double live = ReadEntry(e, blob);
          if (std::fabs(live - e.value) > 1e-4 * std::max(1.0, std::fabs(e.value))) {
            REXLOG_INFO("trainer: the game changed {} to {} (re-applying {})", e.source, live, e.value);
          }
        }
        WriteEntry(e, blob, e.value);
      } else if (frame % 30 == 0) {
        e.value = ReadEntry(e, blob);  // follow the game's own changes
      }
    }
  }
  practice::Tick(base);
  if (REXCVAR_GET(skate3_trainer_watch) && g_blob.load()) {
    static bool armed = false;
    if (!armed) {
      armed = true;
      std::vector<watch::Target> targets;
      std::lock_guard lock(g_mutex);
      for (Entry& e : g_entries) {
        if (e.group != "Flips") continue;
        const size_t size = e.type == Type::kGraphScale ? 64 : 4;
        if (e.blob == vault::kVlt) {
          for (uint8_t* c : e.copies) targets.push_back({e.source, c, size});
        } else {
          targets.push_back({e.source, g_blob.load() + e.offset, size});
        }
      }
      watch::Arm(targets);
    }
    if (frame % 120 == 60) watch::Report();
  }
  if (g_blob.load()) {
    // Perfect finish. Grab held: the one-flip lock is off and flips run at the
    // player's speed. Both triggers up: flip rotation stops at once and the
    // lock comes back, so the game squares the skater up for the landing no
    // matter when the grab was released.
    static bool was_on = false, was_grab = false;
    const practice::Status ps = practice::GetStatus();
    const bool grab = ps.pad_lt > 40 || ps.pad_rt > 40;
    if (g_perfect_finish || was_on) {
      std::lock_guard lock(g_mutex);
      for (Entry& e : g_entries) {
        const bool lock_flag = e.source.find("PerfectBodyFlips") != std::string::npos;
        const bool speed = e.source.find("/FlipMaxSpeed") != std::string::npos ||
                           e.source.find("/FlipScalar") != std::string::npos;
        if (!lock_flag && !speed) continue;
        if (!g_perfect_finish) {
          e.overridden = false;
        } else if (lock_flag) {
          e.overridden = true;
          e.override_value = grab ? 0.0 : 1.0;
        } else {
          e.overridden = !grab;
          e.override_value = 0.0;
        }
        WriteEntry(e, g_blob.load(), e.overridden ? e.override_value : e.value);
      }
      if (g_perfect_finish && grab != was_grab) REXLOG_INFO("trainer: perfect finish - grab {}", grab ? "held" : "released");
    }
    was_on = g_perfect_finish;
    was_grab = grab;
  }
  if (REXCVAR_GET(skate3_trainer_selftest)) SelfTest();
  if (REXCVAR_GET(skate3_trainer_audit)) Audit();
  if (REXCVAR_GET(skate3_trainer_spintest)) SpinTest();
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
          if (ImGui::Checkbox("##freeze", &e.frozen)) e.touched = true, LinkModesLocked(e), SaveUser();
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
                                       static_cast<float>(e.max),
                                       e.type == Type::kGraphScale ? "%.2fx" : "%.3f");
            e.value = v;
          }
          if (changed) {
            e.touched = true;
            LinkModesLocked(e);
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
