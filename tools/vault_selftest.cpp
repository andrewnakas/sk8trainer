// Standalone check of the runtime table builder against a game folder.
// Build: clang-cl /std:c++20 /EHsc /O2 /I../src vault_selftest.cpp ../src/skate3_trainer_vault.cpp
// Run:   vault_selftest <game folder containing data/big/db.big>
#include <cstdio>

#include "skate3_trainer_vault.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: vault_selftest <game folder>\n");
    return 2;
  }
  const auto t = skate3::trainer::vault::BuildFromGame(argv[1]);
  if (!t.error.empty()) {
    std::printf("error: %s\n", t.error.c_str());
    return 1;
  }
  std::printf("bin_size %u, %zu anchors | vlt_size %u, %zu anchors | %zu fields\n", t.bin_size,
              t.anchors.size(), t.vlt_size, t.vlt_anchors.size(), t.fields.size());
  for (const auto& a : t.anchors) std::printf("anchor %u\n", a.offset);
  for (const auto& a : t.vlt_anchors) std::printf("vlt-anchor %u\n", a.offset);
  for (const auto& f : t.fields) {
    std::printf("%-46s %s %8u %g", f.source.c_str(), f.blob ? "vlt" : "bin", f.offset, f.stock);
    for (float y : f.graph_y_stock) std::printf(" %.3f", y);
    std::printf("\n");
  }
  return 0;
}
