#pragma once

// Builds the trainer's entry table at runtime from the player's own game
// files: data/big/db.big -> skaterschema + skatercollections (AttribSys
// vault) -> byte offsets of the curated fields inside skatercollections.bin,
// their stock values, and pointer-free anchor windows used to find that blob
// in guest memory. Nothing from the game is shipped with the trainer.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skate3::trainer::vault {

// kGraphScale: a multiplier applied to every y value of a curve field
// (PointGraphData8 / PointNegGraphData8); its stock value is 1.
enum class Type { kF32, kBool, kI32, kU32, kGraphScale };

// skatercollections.bin / .vlt, or kImage: a constant in the game's loaded
// executable (offset = guest address).
enum Blob : uint8_t { kBin = 0, kVlt = 1, kImage = 2 };

struct Field {
  std::string label, group, source;
  Type type = Type::kF32;
  Blob blob = kBin;
  uint32_t offset = 0;  // into that blob
  uint64_t key = 0;     // field hash (inline .vlt fields are found by key + stock value)
  double stock = 0, min = 0, max = 1;
  // kGraphScale only: byte offsets (from `offset`) and stock values of the
  // y floats to scale.
  std::vector<uint32_t> graph_y_offsets;
  std::vector<float> graph_y_stock;
};

struct Anchor {
  uint32_t offset = 0;
  std::vector<uint8_t> bytes;
};

struct Table {
  uint32_t bin_size = 0;
  std::vector<Anchor> anchors;      // locate skatercollections.bin
  uint32_t vlt_size = 0;
  std::vector<Anchor> vlt_anchors;  // locate skatercollections.vlt
  std::vector<Field> fields;
  std::string error;  // empty on success
};

// game_root is the folder holding data/ (the recomp's game data root).
Table BuildFromGame(const std::filesystem::path& game_root);

// 64-bit AttribSys name hash (Bob Jenkins lookup2, 64-bit variant).
constexpr uint64_t Hash64(const char* text);

namespace detail {
constexpr uint64_t Rd64(const char* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}
constexpr void Mix(uint64_t& a, uint64_t& b, uint64_t& c) {
  a -= b; a -= c; a ^= c >> 43;
  b -= c; b -= a; b ^= a << 9;
  c -= a; c -= b; c ^= b >> 8;
  a -= b; a -= c; a ^= c >> 38;
  b -= c; b -= a; b ^= a << 23;
  c -= a; c -= b; c ^= b >> 5;
  a -= b; a -= c; a ^= c >> 35;
  b -= c; b -= a; b ^= a << 49;
  c -= a; c -= b; c ^= b >> 11;
  a -= b; a -= c; a ^= c >> 12;
  b -= c; b -= a; b ^= a << 18;
  c -= a; c -= b; c ^= b >> 22;
}
constexpr size_t Len(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
}  // namespace detail

constexpr uint64_t Hash64(const char* text) {
  const size_t len = detail::Len(text);
  if (!len) return 0;
  uint64_t a = 0xABCDEF0011223344ull, b = a, c = 0x9E3779B97F4A7C13ull;
  size_t pos = 0;
  while (len - pos >= 24) {
    a += detail::Rd64(text + pos, 8);
    b += detail::Rd64(text + pos + 8, 8);
    c += detail::Rd64(text + pos + 16, 8);
    detail::Mix(a, b, c);
    pos += 24;
  }
  const size_t tail = len - pos;
  a += detail::Rd64(text + pos, tail < 8 ? tail : 8);
  b += tail > 8 ? detail::Rd64(text + pos + 8, tail - 8 < 8 ? tail - 8 : 8) : 0;
  c += len + (tail > 16 ? detail::Rd64(text + pos + 16, tail - 16) << 8 : 0);
  detail::Mix(a, b, c);
  return c;
}

// Checked against the game's schema keys.
static_assert(Hash64("JumpMaxHeight") == 0x78138D95E9C3F15Bull);
static_assert(Hash64("physics_mode") == 0x58F014973A0C54BCull);
static_assert(Hash64("EA::Reflection::Float") == 0xE22228FBB3C209D8ull);
static_assert(Hash64("motorized") == 0xC42A17621ADB49C3ull);

}  // namespace skate3::trainer::vault
