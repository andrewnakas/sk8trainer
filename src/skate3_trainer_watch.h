#pragma once

// SK8TRAINER read watch: diagnostic that logs which game functions read
// chosen vault values (Windows only; no-op elsewhere).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skate3::trainer::watch {

struct Target {
  std::string name;
  uint8_t* host;
  size_t size;
};

void Arm(const std::vector<Target>& targets);
void Report();  // logs per-value read counts and readers

}  // namespace skate3::trainer::watch
