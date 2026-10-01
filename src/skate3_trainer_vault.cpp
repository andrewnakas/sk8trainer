#include "skate3_trainer_vault.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace skate3::trainer::vault {
namespace {

using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

uint16_t U16(const Bytes& d, size_t at) {
  if (at + 2 > d.size()) throw Error("truncated read");
  return uint16_t(d[at] << 8 | d[at + 1]);
}
uint32_t U32(const Bytes& d, size_t at) {
  if (at + 4 > d.size()) throw Error("truncated read");
  return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}
uint64_t U64(const Bytes& d, size_t at) { return uint64_t(U32(d, at)) << 32 | U32(d, at + 4); }
void Put32(Bytes& d, size_t at, uint32_t v) {
  if (at + 4 > d.size()) throw Error("fixup outside blob");
  d[at] = uint8_t(v >> 24), d[at + 1] = uint8_t(v >> 16), d[at + 2] = uint8_t(v >> 8), d[at + 3] = uint8_t(v);
}

// ------------------------------------------------------------ RefPack (EA)
Bytes RefPack(const uint8_t* src, size_t n, size_t expected) {
  size_t p = 0;
  if (n >= 2 && src[1] == 0xFB && (src[0] == 0x10 || src[0] == 0x90)) p = src[0] & 0x80 ? 6 : 5;
  Bytes out;
  out.reserve(expected);
  auto literal = [&](size_t count) {
    if (p + count > n) throw Error("truncated RefPack literal");
    out.insert(out.end(), src + p, src + p + count);
    p += count;
  };
  auto copy = [&](size_t dist, size_t count) {
    if (dist == 0 || dist > out.size()) throw Error("bad RefPack back-reference");
    for (size_t i = 0; i < count; ++i) out.push_back(out[out.size() - dist]);
  };
  while (p < n) {
    const uint8_t c = src[p++];
    if (c < 0x80) {
      if (p >= n) throw Error("truncated RefPack");
      const uint8_t b1 = src[p++];
      literal(c & 3);
      copy(((c & 0x60) << 3) + b1 + 1, ((c >> 2) & 7) + 3);
    } else if (c < 0xC0) {
      if (p + 2 > n) throw Error("truncated RefPack");
      const uint8_t b1 = src[p], b2 = src[p + 1];
      p += 2;
      literal(b1 >> 6);
      copy(((b1 & 0x3F) << 8) + b2 + 1, (c & 0x3F) + 4);
    } else if (c < 0xE0) {
      if (p + 3 > n) throw Error("truncated RefPack");
      const uint8_t b1 = src[p], b2 = src[p + 1], b3 = src[p + 2];
      p += 3;
      literal(c & 3);
      copy(((c & 0x10) << 12) + (b1 << 8) + b2 + 1, ((c & 0x0C) << 6) + b3 + 5);
    } else if (c < 0xFC) {
      literal(((c & 0x1F) << 2) + 4);
    } else {
      literal(c & 3);
      break;
    }
  }
  if (expected && out.size() != expected) throw Error("RefPack size mismatch");
  return out;
}

// ------------------------------------------------------------ EB BIG v3
std::map<std::string, Bytes> ReadBig(const std::filesystem::path& path,
                                     const std::set<std::string>& wanted) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Error("cannot open " + path.string());
  Bytes head(48);
  f.read(reinterpret_cast<char*>(head.data()), 48);
  if (U16(head, 0) != 0x4542 || U16(head, 2) != 3) throw Error("not an EB BIG v3 archive");
  const uint32_t index_size = U32(head, 12), names_size = U32(head, 16);
  Bytes meta(index_size + names_size);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(meta.data()), meta.size());
  const uint32_t count = U32(meta, 4);
  const uint16_t flags = U16(meta, 8);
  const uint8_t shift = meta[10], name_rec = meta[20], dir_rec = meta[21];
  const size_t entry_size = flags & 1 ? 20 : 16;
  const size_t entries = 48, compression = entries + ((entry_size * count + 15) & ~size_t(15));
  const size_t names = index_size, dirs = names + ((size_t(name_rec) * count + 15) & ~size_t(15));
  std::vector<std::string> directories;
  for (size_t at = dirs; at + dir_rec <= meta.size(); at += dir_rec) {
    directories.emplace_back(reinterpret_cast<const char*>(&meta[at]),
                             strnlen(reinterpret_cast<const char*>(&meta[at]), dir_rec));
  }
  std::map<std::string, Bytes> out;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t rec = names + size_t(i) * name_rec;
    const uint16_t dir = U16(meta, rec);
    std::string name(reinterpret_cast<const char*>(&meta[rec + 2]),
                     strnlen(reinterpret_cast<const char*>(&meta[rec + 2]), name_rec - 2));
    std::string full = dir < directories.size() && !directories[dir].empty() && directories[dir] != "."
                           ? directories[dir] + "/" + name
                           : name;
    std::replace(full.begin(), full.end(), '\\', '/');
    if (!wanted.count(full)) continue;
    const size_t e = entries + size_t(i) * entry_size;
    const uint64_t offset = uint64_t(U32(meta, e)) << shift;
    uint32_t stored = U32(meta, e + 4), unpacked = U32(meta, e + 8);
    if (!unpacked) unpacked = stored;
    if (!stored) stored = unpacked;
    const uint8_t method = meta[compression + i];
    Bytes packed(stored);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(packed.data()), stored);
    if (!f) throw Error(full + ": truncated payload");
    if (method == 0) {
      out[full] = std::move(packed);
    } else if (method == 1) {
      out[full] = RefPack(packed.data(), packed.size(), unpacked);
    } else {
      throw Error(full + ": unsupported compression " + std::to_string(method));
    }
  }
  return out;
}

// ------------------------------------------------------------ AttribSys vault
constexpr uint32_t kPtrN = 0x5074724E, kExpN = 0x4578704E, kEndC = 0x456E6443;
constexpr uint64_t kClassKind = 0x2A7895AC4A876152ull;
constexpr uint64_t kCollectionKind = 0xAD303B8F42B3307Eull;

struct Export {
  uint64_t key, kind;
  uint32_t size, at;
};

// Applies the pointer table (like the game's loader, but with file-relative
// targets) and returns exports plus the .bin byte offsets that hold pointers.
std::vector<Export> LoadVault(Bytes& vlt, Bytes& bin, std::unordered_set<uint32_t>* bin_slots,
                              std::unordered_set<uint32_t>* vlt_slots = nullptr) {
  std::vector<Export> exports;
  size_t at = 0;
  while (at + 8 <= vlt.size()) {
    const uint32_t tag = U32(vlt, at), size = U32(vlt, at + 4);
    if (size < 8 || at + size > vlt.size()) throw Error("bad vault chunk");
    const size_t body = at + 8, end = at + size;
    if (tag == kPtrN) {
      Bytes* target = &bin;
      for (size_t p = body; p + 16 <= end; p += 16) {
        const uint32_t offset = U32(vlt, p);
        const uint16_t kind = U16(vlt, p + 4), index = U16(vlt, p + 6);
        const uint32_t dest = uint32_t(U64(vlt, p + 8));
        if (kind == 0) break;
        if (kind == 2) {
          target = index == 0 ? &vlt : &bin;
        } else if (kind == 1 || kind == 3) {
          Put32(*target, offset, kind == 1 ? 0 : dest);
          auto* slots = target == &bin ? bin_slots : vlt_slots;
          if (slots) {
            for (uint32_t k = 0; k < 4; ++k) slots->insert(offset + k);
          }
        } else if (kind != 4) {
          throw Error("unknown vault pointer kind");
        }
      }
    } else if (tag == kExpN) {
      const uint64_t count = U64(vlt, body);
      for (uint64_t i = 0; i < count; ++i) {
        const size_t e = body + 8 + i * 24;
        exports.push_back({U64(vlt, e), U64(vlt, e + 8), U32(vlt, e + 16), U32(vlt, e + 20)});
      }
    }
    at = end;
    if (tag == kEndC) break;
  }
  return exports;
}

struct SchemaField {
  uint64_t type;
  uint16_t offset, size;
  uint8_t flags;
};

// ------------------------------------------------------------ curated list
struct Curated {
  const char* label;
  const char* group;
  const char* cls;
  const char* key;    // nullptr = every collection of the class
  const char* field;  // name, or "#<16 hex digits>" for an unnamed field
  double min, max;
  char kind = 'v';    // 'v' value, 'g' curve multiplier (y values x N)
};

uint64_t FieldKey(const char* field) {
  return field[0] == '#' ? std::strtoull(field + 1, nullptr, 16) : Hash64(field);
}

// Field names are the game's AttribSys identifiers (hashed, never shipped
// data). Ranges are generous editing limits, not game values.
constexpr Curated kCurated[] = {
    {"Ollie min height", "Pop & Jump", "physics_mode", nullptr, "JumpMinHeight", 0, 20},
    {"Ollie max height", "Pop & Jump", "physics_mode", nullptr, "JumpMaxHeight", 0, 20},
    {"Jump height override", "Pop & Jump", "physics_mode", nullptr, "JumpHeightOverrideEnabled", 0, 1},
    {"Absolute min pop", "Pop & Jump", "physics_jump", "default", "AbsoluteMinHeight", 0, 20},
    {"Pop Y bonus max", "Pop & Jump", "physics_jump", "default", "JumpYBonusMax", 0, 20},
    {"Pop speed response max", "Pop & Jump", "physics_jump", "default", "SpeedResponseMaxSpeed", 0, 200},
    {"Hippy/biped jump height", "Pop & Jump", "physics_biped", "default", "JumpHeight", 0, 20},
    {"Biped jump speed scalar", "Pop & Jump", "physics_biped", "default", "JumpSpeedScalar", 0, 10},
    {"Gravity (speed cons.)", "Gravity & Speed", "physics_speed_conservation", "default", "Gravity", -50, 50},
    {"Max gravity accel", "Gravity & Speed", "physics_speed_conservation", "default", "MaxGravityAcceleration", 0, 100},
    {"Max pushable speed", "Gravity & Speed", "physics_push", "default", "MaxPushableSpeed", 0, 500},
    {"Push dV start", "Gravity & Speed", "physics_mode", nullptr, "MaxPushDVStart", 0, 100},
    {"Push dV end", "Gravity & Speed", "physics_mode", nullptr, "MaxPushDVEnd", 0, 100},
    {"Motor top speed", "Gravity & Speed", "physics_mode", nullptr, "MotorTopSpeed", 0, 100},
    {"Auto push", "Gravity & Speed", "physics_mode", nullptr, "AutoPushEnabled", 0, 1},
    {"Pump effect", "Gravity & Speed", "physics_mode", nullptr, "PumpEffectFactor", 0, 200},
    {"Body spin speed x", "Spin", "physics_bodyspin", "default", "PropBodySpinVsTime", 0.1, 10, 'g'},
    {"Body spin speed x (alt curve)", "Spin", "physics_bodyspin", "default", "#D7C6855B7814D048", 0.1, 10, 'g'},
    {"Body spin response x", "Spin", "physics_bodyspin", "default", "DerivativeBodySpinVsTime", 0.1, 10, 'g'},
    {"Max air spin speed", "Spin", "physics_airstates", "default", "MaxSpinSpeed", 0, 5000},
    {"Max auto body spin", "Spin", "physics_mode", nullptr, "MaxAutoBodySpinSpeed", 0, 100},
    {"Easy body spins", "Spin", "physics_mode", nullptr, "EasyBodySpins", 0, 1},
    {"Flip scalar", "Flips", "physics_reckoning", "default", "FlipScalar", 0, 20},
    {"Flip max speed", "Flips", "physics_reckoning", "default", "FlipMaxSpeed", 0, 100},
    {"Flip body-spin scalar", "Flips", "physics_reckoning", "default", "FlipBodySpinScalar", 0, 10},
    {"Flip speed smoothing", "Flips", "physics_reckoning", "default", "FlipSpeedSmoothingFactor", 0, 1},
    {"Off-board jump height", "Pop & Jump", "physics_state_offboard_air", "default", "JumpHeight", 0, 20},
    {"Bail: max speed into ground", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSpeedIntoGround", 0, 1000},
    {"Bail: max speed into stairs", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSpeedIntoStairs", 0, 1000},
    {"Bail: air skeleton contact", "Bails", "physics_wipeout", "default", "Wipeout_AirSkeletonMaxContact", 0, 1000},
    {"Bail: ground skeleton contact", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkeletonMaxContact", 0, 1000},
    {"Bail: ground balance total", "Bails", "physics_wipeout", "default", "Wipeout_GroundBalanceTotal", 0, 1000},
    {"Bail: vehicle contact", "Bails", "physics_wipeout", "default", "Wipeout_GroundVehicleContact", 0, 1000},
    {"Bail if tilted past this in the air (up-Y; -1 or lower = never, 1 = ALWAYS bail)", "Bails", "physics_wipeout", "default", "Wipeout_AirFallingMinUpY", -10, 1},
    {"Ragdoll gravity (air Y acc)", "Bails", "physics_wipeout", "default", "Wipeout_AirYAcceleration", -200, 500},
    {"Ragdoll gravity (ground Y)", "Bails", "physics_wipeout", "default", "Wipeout_GroundYAcceleration", -200, 500},
    {"Ragdoll inv-mass factor", "Bails", "physics_wipeout", "default", "RagdollInvMassFactor", 0, 20},
    {"Auto-reset after bail (s)", "Bails", "physics_wipeout", "default", "TeleportMinTimeForAutoReset", 0, 60},
    {"Bail: AirMaxSquash", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSquash", 0, 100000},
    {"Bail: AirMaxSpeedIntoCollisionNearGrind", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSpeedIntoCollisionNearGrind", 0, 100000},
    {"Bail: GroundMaxSquashCoffin", "Bails", "physics_wipeout", "default", "Wipeout_GroundMaxSquashCoffin", 0, 100000},
    {"Bail: GroundMaxSquash", "Bails", "physics_wipeout", "default", "Wipeout_GroundMaxSquash", 0, 100000},
    {"Bail: OB_MaxSquash", "Bails", "physics_wipeout", "default", "Wipeout_OB_MaxSquash", 0, 100000},
    {"Bail: AirSkeletonMaxContactArms", "Bails", "physics_wipeout", "default", "Wipeout_AirSkeletonMaxContactArms", 0, 100000},
    {"Bail: GroundSkeletonMaxContactArms", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkeletonMaxContactArms", 0, 100000},
    {"Bail: OB_SkeletonMaxContact", "Bails", "physics_wipeout", "default", "Wipeout_OB_SkeletonMaxContact", 0, 100000},
    {"Bail: OB_SkeletonMaxContactArms", "Bails", "physics_wipeout", "default", "Wipeout_OB_SkeletonMaxContactArms", 0, 100000},
    {"Bail: OB_Air_SkelMaxContact", "Bails", "physics_wipeout", "default", "Wipeout_OB_Air_SkelMaxContact", 0, 100000},
    {"Bail: AirSkeletonMaxDisp", "Bails", "physics_wipeout", "default", "Wipeout_AirSkeletonMaxDisp", 0, 100000},
    {"Bail: GroundSkeletonMaxDisp", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkeletonMaxDisp", 0, 100000},
    {"Bail: OB_SkeletonMaxDisp", "Bails", "physics_wipeout", "default", "Wipeout_OB_SkeletonMaxDisp", 0, 100000},
    {"Bail: OB_Air_SkelMaxDisp", "Bails", "physics_wipeout", "default", "Wipeout_OB_Air_SkelMaxDisp", 0, 100000},
    {"Bail: GroundBalanceBase", "Bails", "physics_wipeout", "default", "Wipeout_GroundBalanceBase", 0, 100000},
    {"Bail: GroundOpposingContact", "Bails", "physics_wipeout", "default", "Wipeout_GroundOpposingContact", 0, 100000},
    {"Bail: OB_VehicleContact", "Bails", "physics_wipeout", "default", "Wipeout_OB_VehicleContact", 0, 100000},
    {"Bail: GroundSkitchingContact", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkitchingContact", 0, 100000},
    {"Bail: GroundMaxAngularDeckError", "Bails", "physics_wipeout", "default", "Wipeout_GroundMaxAngularDeckError", 0, 100000},
    {"Bail: GroundLeanContactYThresh", "Bails", "physics_wipeout", "default", "Wipeout_GroundLeanContactYThresh", 0, 100000},
    {"Bail: #EE81DD78506E4A2D", "Bails", "physics_wipeout", "default", "#EE81DD78506E4A2D", 0, 100000},
    {"Bail: #F784AC3BA4422FFD", "Bails", "physics_wipeout", "default", "#F784AC3BA4422FFD", 0, 100000},
    {"Bail: #C7DDE25FF0D72DA0", "Bails", "physics_wipeout", "default", "#C7DDE25FF0D72DA0", 0, 100000},
    {"Bail: #9F1C2EF30C749332", "Bails", "physics_wipeout", "default", "#9F1C2EF30C749332", 0, 100000},
    {"Bail: falling max angle (lower = safer)", "Bails", "physics_wipeout", "default", "Wipeout_AirFallingMaxAngle", -10, 1},
    {"Bail on bad landing", "Bails", "physics_mode", nullptr, "WipeoutCheckForBadLanding", 0, 1},
    {"Push speed limit 2", "Gravity & Speed", "physics_push", "default", "#501D5581043D7D3C", 0, 500},
    {"Run speed x", "Gravity & Speed", "physics_biped", "default", "SpeedVsInput", 0.1, 20, 'g'},
    {"Run speed x (alt curve)", "Gravity & Speed", "physics_biped", "default", "#CE8C0D4C6B92FD27", 0.1, 20, 'g'},
    {"Jump height A", "Pop & Jump", "physics_mode", nullptr, "#0F2473E9125079F0", 0, 50},
    {"Jump height B", "Pop & Jump", "physics_mode", nullptr, "#1B3E9F9C836D287D", 0, 50},
    {"Jump height C", "Pop & Jump", "physics_mode", nullptr, "#703829BD711E54DE", 0, 50},
    {"Jump height D", "Pop & Jump", "physics_mode", nullptr, "#B2B1170AFFC8AC69", 0, 50},
    {"Jump min height 2", "Pop & Jump", "physics_mode", nullptr, "#BE3F74F978D777E5", 0, 50},
    {"Hit tolerance: SkaterSkaterThresholdScalar", "Bails", "physics_wipeout", "default", "SkaterSkaterThresholdScalar", 0, 100000},
    {"Hit tolerance: GroundVehicleScalar", "Bails", "physics_wipeout", "default", "Wipeout_GroundVehicleScalar", 0, 100000},
    {"Hit tolerance: OB_VehicleScalar", "Bails", "physics_wipeout", "default", "Wipeout_OB_VehicleScalar", 0, 100000},
    {"Hit tolerance: GroundSkitchingScalar", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkitchingScalar", 0, 100000},
    {"Hit tolerance: GroundSkitchingScalarArms", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkitchingScalarArms", 0, 100000},
    {"Hit tolerance: GroundLightDMOScalar", "Bails", "physics_wipeout", "default", "WipeoutGroundLightDMOScalar", 0, 100000},
    {"Hit tolerance: AirLightDMOScalar", "Bails", "physics_wipeout", "default", "WipeoutAirLightDMOScalar", 0, 100000},
    {"Hit tolerance: DeckAccSkitchScalar", "Bails", "physics_wipeout", "default", "DeckAccSkitchScalar", 0, 100000},
    {"Hit tolerance: DeckAccAIScalar", "Bails", "physics_wipeout", "default", "DeckAccAIScalar", 0, 100000},
    {"Keep board: body-flip bail scalar", "Bails", "physics_wipeout", "default", "Wipeout_AirBodyFlipScalar", 0, 100000},
    {"Keep board: deck accel (body flip)", "Bails", "physics_wipeout", "default", "DeckAccBodyFlipScalar", 0, 100000},
    {"Keep board: deck accel (player)", "Bails", "physics_wipeout", "default", "DeckAccPlayerScalar", 0, 100000},
    {"Bail: air trick XZ", "Bails", "physics_wipeout", "default", "Wipeout_AirXZTrick", 0, 100000},
    {"Bail: air trick Y", "Bails", "physics_wipeout", "default", "Wipeout_AirYTrick", 0, 100000},
    {"Body flip: min grab time fraction", "Flips", "physics_airstates", "default", "BodyFlipMinGrabTimeFraction", 0, 1},
    {"Landing: heading auto-correct x", "Flips", "physics_airstates", "default", "MaxHeadingAdjustVsUpY", 0.1, 4, 'g'},
    {"Landing: max ground angle from up", "Flips", "physics_reckoning", "default", "MaxAllowedGroundNormalFromUp", 0, 180},
    {"Perfect body flips (locks to ONE flip)", "Flips", "physics_mode", nullptr, "PerfectBodyFlips", 0, 1},
    {"Landing: align-to-ground max angle", "Flips", "physics_airstates", "default", "DontAlignAnglePhysicsAir", 0, 4},
    {"Landing: align-to-ground speed", "Flips", "physics_airstates", "default", "SpeedToAlignToGround_PhysAir", 0, 5},
    {"Slow-mo fps at scale 1", "World", "slowmotion_controller", "default", "fps_at_scale_one", 1, 240},
};

// Collection keys we can name (physics_mode difficulty presets etc.).
constexpr const char* kKnownKeys[] = {"default", "normal", "test", "motorized", "easy", "hardcore"};

std::string KeyName(uint64_t key) {
  for (const char* k : kKnownKeys) {
    if (Hash64(k) == key) return k;
  }
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(key));
  return buf;
}

double Entropy(const uint8_t* w, size_t n) {
  int counts[256] = {};
  for (size_t i = 0; i < n; ++i) ++counts[w[i]];
  double e = 0;
  for (int c : counts) {
    if (c) e -= double(c) / n * std::log2(double(c) / n);
  }
  return e;
}

}  // namespace

Table BuildFromGame(const std::filesystem::path& game_root) {
  Table table;
  try {
    const std::set<std::string> wanted = {"data/db/skaterschema.vlt", "data/db/skaterschema.bin",
                                          "data/db/skatercollections.vlt",
                                          "data/db/skatercollections.bin"};
    auto files = ReadBig(game_root / "data" / "big" / "db.big", wanted);
    for (const auto& w : wanted) {
      if (!files.count(w)) throw Error("db.big has no " + w);
    }
    Bytes& sv = files["data/db/skaterschema.vlt"];
    Bytes& sb = files["data/db/skaterschema.bin"];
    Bytes& cv = files["data/db/skatercollections.vlt"];
    Bytes& cb = files["data/db/skatercollections.bin"];
    const Bytes raw_cb = cb;  // what sits in guest memory, minus pointer slots
    const Bytes raw_cv = cv;
    std::unordered_set<uint32_t> slots, vslots;
    const auto schema = LoadVault(sv, sb, nullptr);
    const auto collections = LoadVault(cv, cb, &slots, &vslots);

    // class -> field -> layout info
    std::map<uint64_t, std::map<uint64_t, SchemaField>> classes;
    for (const Export& e : schema) {
      if (e.kind != kClassKind) continue;
      const uint64_t key = U64(sv, e.at);
      const uint32_t count = U32(sv, e.at + 12), defs = U32(sv, e.at + 16);
      auto& fields = classes[key];
      for (uint32_t i = 0; i < count; ++i) {
        const size_t d = defs + size_t(i) * 24;
        fields[U64(sb, d)] = {U64(sb, d + 8), U16(sb, d + 16), U16(sb, d + 18), sb[d + 22]};
      }
    }
    // (class, key) -> layout offset in the .bin
    struct Collection {
      uint32_t layout;   // .bin offset of the layout block (0 = none)
      uint32_t entries;  // .vlt offset of the inline entry table
      uint32_t count;
    };
    std::map<std::pair<uint64_t, uint64_t>, Collection> layouts;
    for (const Export& e : collections) {
      if (e.kind != kCollectionKind) continue;
      const uint64_t key = U64(cv, e.at), cls = U64(cv, e.at + 8);
      // key, class, parent (u64 each), reserve, pad, count (u32), ntypes,
      // typeslen (u16), layout (u32) -> count at +32, typeslen at +38,
      // layout at +40; 16-byte entries follow the 48-byte header + types.
      const uint32_t count = U32(cv, e.at + 32);
      const uint16_t typeslen = U16(cv, e.at + 38);
      layouts[{cls, key}] = {U32(cv, e.at + 40), uint32_t(e.at + 48 + typeslen * 8u), count};
    }

    for (const Curated& c : kCurated) {
      const uint64_t cls = Hash64(c.cls), fkey = FieldKey(c.field);
      auto ci = classes.find(cls);
      if (ci == classes.end()) continue;
      auto fi = ci->second.find(fkey);
      if (fi == ci->second.end()) continue;
      const SchemaField& f = fi->second;
      const bool layout_field = (f.flags & 2) != 0;
      Type type;
      if (c.kind == 'g') {
        // PointNegGraphData8 (80 bytes): xmin ymin xmax ymax, x[8], y[8].
        // PointGraphData8 (64 bytes): x[8], y[8].
        // PointNegGraphDataN is 16 + 8N bytes (N = 4, 8, 16).
        if (!layout_field || (f.size != 80 && f.size != 64 && f.size != 144 && f.size != 48)) continue;
        type = Type::kGraphScale;
      } else if (f.type == Hash64("EA::Reflection::Float")) type = Type::kF32;
      else if (f.type == Hash64("EA::Reflection::Bool")) type = Type::kBool;
      else if (f.type == Hash64("EA::Reflection::Int32")) type = Type::kI32;
      else if (f.type == Hash64("EA::Reflection::UInt32")) type = Type::kU32;
      else continue;
      if (!layout_field && (f.size > 4 || (f.flags & 1))) continue;  // inline scalars only
      for (const auto& [ck, col] : layouts) {
        if (ck.first != cls || (c.key && ck.second != Hash64(c.key))) continue;
        Field out;
        const Bytes* raw = &raw_cb;
        if (layout_field) {
          if (!col.layout) continue;
          out.blob = kBin;
          out.offset = col.layout + f.offset;
          if (out.offset + f.size > raw_cb.size() || slots.count(out.offset)) continue;
        } else {
          // Inline: find this field's entry; its value is the entry's last 4 bytes.
          uint32_t at = 0;
          for (uint32_t k = 0; k < col.count && !at; ++k) {
            const uint32_t pos = col.entries + k * 16;
            if (pos + 16 <= raw_cv.size() && U64(raw_cv, pos) == fkey) at = pos + 8;
          }
          if (!at || vslots.count(at)) continue;
          out.blob = kVlt;
          out.offset = at;
          out.key = fkey;
          raw = &raw_cv;
        }
        out.label = c.key ? c.label : std::string(c.label) + " [" + KeyName(ck.second) + "]";
        out.group = c.group;
        out.source = std::string(c.cls) + "/" + KeyName(ck.second) + "/" + c.field;
        out.type = type;
        out.min = c.min;
        out.max = c.max;
        if (type == Type::kGraphScale) {
          const bool header = f.size != 64;
          const uint32_t n = header ? (f.size - 16) / 8 : 8;
          const uint32_t y0 = (header ? 16 : 0) + n * 4;
          for (uint32_t k = 0; k < n; ++k) out.graph_y_offsets.push_back(y0 + k * 4);
          if (header) out.graph_y_offsets.push_back(12);  // ymax
          for (uint32_t o : out.graph_y_offsets) {
            const uint32_t word = U32(*raw, out.offset + o);
            float v;
            std::memcpy(&v, &word, 4);
            out.graph_y_stock.push_back(v);
          }
          out.stock = 1.0;
        } else {
          const uint32_t word = U32(*raw, out.offset);
          switch (type) {
            case Type::kF32: {
              float v;
              std::memcpy(&v, &word, 4);
              out.stock = v;
              break;
            }
            case Type::kBool: out.stock = (*raw)[out.offset] ? 1 : 0; break;
            case Type::kI32: out.stock = static_cast<int32_t>(word); break;
            default: out.stock = word; break;
          }
        }
        table.fields.push_back(std::move(out));
      }
    }

    // Anchors: one unique, pointer-free, high-entropy 64-byte window per
    // eighth of each blob.
    auto pick = [](const Bytes& raw, const std::unordered_set<uint32_t>& ptrs) {
      constexpr size_t kW = 64;
      std::vector<Anchor> out;
      const size_t step = raw.size() / 8;
      for (size_t start = 0; start + kW < raw.size() && out.size() < 8; start += step) {
        for (size_t off = start; off < std::min(start + step, raw.size() - kW); off += 16) {
          bool clean = true;
          for (size_t k = off; k < off + kW && clean; ++k) clean = !ptrs.count(uint32_t(k));
          if (!clean || Entropy(&raw[off], kW) < 4.5) continue;
          auto first = std::search(raw.begin(), raw.end(), raw.begin() + off, raw.begin() + off + kW);
          auto again = std::search(first + 1, raw.end(), raw.begin() + off, raw.begin() + off + kW);
          if (again != raw.end()) continue;
          out.push_back({uint32_t(off), Bytes(raw.begin() + off, raw.begin() + off + kW)});
          break;
        }
      }
      return out;
    };
    table.anchors = pick(raw_cb, slots);
    // .vlt: the game rewrites entry flag bytes when it loads the vault, so
    // 64-byte windows do not survive. Anchor on entry KEYS instead (8-byte
    // field hashes it never touches), spread over the file, each unique.
    {
      std::vector<uint32_t> keys;
      for (const auto& [ck, col] : layouts) {
        for (uint32_t k = 0; k < col.count; ++k) keys.push_back(col.entries + k * 16);
      }
      std::sort(keys.begin(), keys.end());
      const size_t want = 8;
      for (size_t n = 0; n < want && !keys.empty(); ++n) {
        // Walk forward from each eighth of the entry list to a unique key.
        for (size_t i = keys.size() * n / want; i < keys.size(); ++i) {
          const uint32_t at = keys[i];
          if (at + 8 > raw_cv.size()) continue;
          const Bytes key(raw_cv.begin() + at, raw_cv.begin() + at + 8);
          auto first = std::search(raw_cv.begin(), raw_cv.end(), key.begin(), key.end());
          auto again = std::search(first + 1, raw_cv.end(), key.begin(), key.end());
          if (again != raw_cv.end()) continue;
          table.vlt_anchors.push_back({at, key});
          break;
        }
      }
    }
    table.vlt_size = uint32_t(raw_cv.size());
    table.bin_size = uint32_t(raw_cb.size());
    if (table.anchors.size() < 3) throw Error("could not pick enough anchors");
    if (table.fields.empty()) throw Error("no curated fields found in this vault");
  } catch (const std::exception& e) {
    table.error = e.what();
  }
  return table;
}

}  // namespace skate3::trainer::vault
