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
std::vector<Export> LoadVault(Bytes& vlt, Bytes& bin, std::unordered_set<uint32_t>* bin_slots) {
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
          if (bin_slots && target == &bin) {
            for (uint32_t k = 0; k < 4; ++k) bin_slots->insert(offset + k);
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
  const char* key;  // nullptr = every collection of the class
  const char* field;
  double min, max;
};

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
    {"Max pushable speed", "Gravity & Speed", "physics_push", "default", "MaxPushableSpeed", 0, 100},
    {"Push dV start", "Gravity & Speed", "physics_mode", nullptr, "MaxPushDVStart", 0, 20},
    {"Push dV end", "Gravity & Speed", "physics_mode", nullptr, "MaxPushDVEnd", 0, 20},
    {"Motor top speed", "Gravity & Speed", "physics_mode", nullptr, "MotorTopSpeed", 0, 100},
    {"Auto push", "Gravity & Speed", "physics_mode", nullptr, "AutoPushEnabled", 0, 1},
    {"Pump effect", "Gravity & Speed", "physics_mode", nullptr, "PumpEffectFactor", 0, 200},
    {"Max air spin speed", "Spin", "physics_airstates", "default", "MaxSpinSpeed", 0, 5000},
    {"Max auto body spin", "Spin", "physics_mode", nullptr, "MaxAutoBodySpinSpeed", 0, 100},
    {"Easy body spins", "Spin", "physics_mode", nullptr, "EasyBodySpins", 0, 1},
    {"Bail: max speed into ground", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSpeedIntoGround", 0, 1000},
    {"Bail: max speed into stairs", "Bails", "physics_wipeout", "default", "Wipeout_AirMaxSpeedIntoStairs", 0, 1000},
    {"Bail: air skeleton contact", "Bails", "physics_wipeout", "default", "Wipeout_AirSkeletonMaxContact", 0, 1000},
    {"Bail: ground skeleton contact", "Bails", "physics_wipeout", "default", "Wipeout_GroundSkeletonMaxContact", 0, 1000},
    {"Bail: ground balance total", "Bails", "physics_wipeout", "default", "Wipeout_GroundBalanceTotal", 0, 1000},
    {"Bail: vehicle contact", "Bails", "physics_wipeout", "default", "Wipeout_GroundVehicleContact", 0, 1000},
    {"Bail: falling min up-Y", "Bails", "physics_wipeout", "default", "Wipeout_AirFallingMinUpY", -1, 1},
    {"Ragdoll gravity (air Y acc)", "Bails", "physics_wipeout", "default", "Wipeout_AirYAcceleration", -200, 500},
    {"Ragdoll gravity (ground Y)", "Bails", "physics_wipeout", "default", "Wipeout_GroundYAcceleration", -200, 500},
    {"Ragdoll inv-mass factor", "Bails", "physics_wipeout", "default", "RagdollInvMassFactor", 0, 20},
    {"Auto-reset after bail (s)", "Bails", "physics_wipeout", "default", "TeleportMinTimeForAutoReset", 0, 60},
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
    std::unordered_set<uint32_t> slots;
    const auto schema = LoadVault(sv, sb, nullptr);
    const auto collections = LoadVault(cv, cb, &slots);

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
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint32_t>> layouts;
    for (const Export& e : collections) {
      if (e.kind != kCollectionKind) continue;
      const uint64_t key = U64(cv, e.at), cls = U64(cv, e.at + 8);
      // key, class, parent (u64 each), reserve, pad, count (u32), ntypes,
      // typeslen (u16), layout (u32) -> layout at +40.
      const uint32_t layout = U32(cv, e.at + 40);
      if (layout) layouts.push_back({{cls, key}, layout});
    }
    std::sort(layouts.begin(), layouts.end());

    for (const Curated& c : kCurated) {
      const uint64_t cls = Hash64(c.cls), fkey = Hash64(c.field);
      auto ci = classes.find(cls);
      if (ci == classes.end()) continue;
      auto fi = ci->second.find(fkey);
      if (fi == ci->second.end() || !(fi->second.flags & 2)) continue;
      const SchemaField& f = fi->second;
      Type type;
      if (f.type == Hash64("EA::Reflection::Float")) type = Type::kF32;
      else if (f.type == Hash64("EA::Reflection::Bool")) type = Type::kBool;
      else if (f.type == Hash64("EA::Reflection::Int32")) type = Type::kI32;
      else if (f.type == Hash64("EA::Reflection::UInt32")) type = Type::kU32;
      else continue;
      for (const auto& [ck, layout] : layouts) {
        if (ck.first != cls || (c.key && ck.second != Hash64(c.key))) continue;
        const uint32_t at = layout + f.offset;
        if (at + 4 > raw_cb.size() || slots.count(at)) continue;
        Field out;
        out.label = c.key ? c.label : std::string(c.label) + " [" + KeyName(ck.second) + "]";
        out.group = c.group;
        out.source = std::string(c.cls) + "/" + KeyName(ck.second) + "/" + c.field;
        out.type = type;
        out.offset = at;
        out.min = c.min;
        out.max = c.max;
        const uint32_t word = U32(raw_cb, at);
        switch (type) {
          case Type::kF32: {
            float v;
            std::memcpy(&v, &word, 4);
            out.stock = v;
            break;
          }
          case Type::kBool: out.stock = raw_cb[at] ? 1 : 0; break;
          case Type::kI32: out.stock = static_cast<int32_t>(word); break;
          case Type::kU32: out.stock = word; break;
        }
        table.fields.push_back(std::move(out));
      }
    }

    // Anchors: one unique, pointer-free, high-entropy 64-byte window per eighth.
    constexpr size_t kW = 64;
    const size_t step = raw_cb.size() / 8;
    for (size_t start = 0; start + kW < raw_cb.size() && table.anchors.size() < 8; start += step) {
      for (size_t off = start; off < std::min(start + step, raw_cb.size() - kW); off += 16) {
        bool clean = true;
        for (size_t k = off; k < off + kW && clean; ++k) clean = !slots.count(uint32_t(k));
        if (!clean || Entropy(&raw_cb[off], kW) < 4.5) continue;
        auto first = std::search(raw_cb.begin(), raw_cb.end(), raw_cb.begin() + off,
                                 raw_cb.begin() + off + kW);
        auto again = std::search(first + 1, raw_cb.end(), raw_cb.begin() + off,
                                 raw_cb.begin() + off + kW);
        if (again != raw_cb.end()) continue;
        table.anchors.push_back({uint32_t(off), Bytes(raw_cb.begin() + off, raw_cb.begin() + off + kW)});
        break;
      }
    }
    table.bin_size = uint32_t(raw_cb.size());
    if (table.anchors.size() < 3) throw Error("could not pick enough anchors");
    if (table.fields.empty()) throw Error("no curated fields found in this vault");
  } catch (const std::exception& e) {
    table.error = e.what();
  }
  return table;
}

}  // namespace skate3::trainer::vault
