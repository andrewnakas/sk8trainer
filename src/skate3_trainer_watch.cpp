// SK8TRAINER read watch (diagnostic, Windows only; cvar skate3_trainer_watch).
//
// Guard-pages the host pages holding chosen vault values, logs which lifted
// game functions read which values, and re-arms the guard after every
// access (single-step). Answers "does the game read this while I skate, and
// who reads it?" - a value nobody reads cannot change anything.

#include "skate3_trainer_watch.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "generated/skate3_init.h"

#include <rex/logging.h>

namespace skate3::trainer::watch {
namespace {

struct Range {
  std::string name;
  uintptr_t begin, end;
};

std::mutex g_mutex;
std::vector<Range> g_ranges;
std::set<uintptr_t> g_pages;
std::map<std::pair<size_t, uint32_t>, uint64_t> g_hits;  // (range, guest fn) -> reads
std::vector<std::pair<uintptr_t, uint32_t>> g_funcs;      // host fn address -> guest
PVOID g_handler = nullptr;
thread_local uintptr_t t_rearm = 0;
std::atomic<uint64_t> g_total{0};

constexpr uintptr_t kPage = 0x1000;

uint32_t GuestFunctionFor(uintptr_t rip) {
  auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), std::make_pair(rip, UINT32_MAX));
  // Past the last lifted function = host code (the trainer's own reads).
  if (it == g_funcs.begin() || it == g_funcs.end()) return 0;
  return std::prev(it)->second;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == STATUS_GUARD_PAGE_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
    const uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    const uintptr_t page = addr & ~(kPage - 1);
    std::lock_guard lock(g_mutex);
    if (!g_pages.count(page)) return EXCEPTION_CONTINUE_SEARCH;
    for (size_t i = 0; i < g_ranges.size(); ++i) {
      if (addr >= g_ranges[i].begin && addr < g_ranges[i].end) {
        ++g_hits[{i, GuestFunctionFor(ep->ContextRecord->Rip)}];
        ++g_total;
      }
    }
    // Let this instruction run, then re-arm the guard on the single step.
    t_rearm = page;
    ep->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == STATUS_SINGLE_STEP && t_rearm) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(t_rearm), kPage, PAGE_READWRITE | PAGE_GUARD, &old);
    t_rearm = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Arm(const std::vector<Target>& targets) {
  std::lock_guard lock(g_mutex);
  if (g_funcs.empty()) {
    for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->guest; ++m) {
      g_funcs.push_back({reinterpret_cast<uintptr_t>(m->host), static_cast<uint32_t>(m->guest)});
    }
    std::sort(g_funcs.begin(), g_funcs.end());
  }
  if (!g_handler) g_handler = AddVectoredExceptionHandler(1, Handler);
  for (const Target& t : targets) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(t.host);
    g_ranges.push_back({t.name, b, b + t.size});
    for (uintptr_t p = b & ~(kPage - 1); p < b + t.size; p += kPage) g_pages.insert(p);
  }
  for (uintptr_t p : g_pages) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(p), kPage, PAGE_READWRITE | PAGE_GUARD, &old);
  }
  REXLOG_INFO("trainer watch: armed {} values on {} pages", g_ranges.size(), g_pages.size());
}

void Report() {
  std::lock_guard lock(g_mutex);
  if (g_ranges.empty()) return;
  static uint64_t last_game_reads = 0;
  uint64_t game_reads = 0;
  for (const auto& [key, count] : g_hits) game_reads += key.second ? count : 0;
  if (game_reads == last_game_reads) return;  // only when the game read something new
  last_game_reads = game_reads;
  REXLOG_INFO("trainer watch: {} game reads so far", game_reads);
  for (size_t i = 0; i < g_ranges.size(); ++i) {
    std::string who;
    uint64_t n = 0;
    for (const auto& [key, count] : g_hits) {
      if (key.first != i || !key.second) continue;
      n += count;
      char buf[48];
      std::snprintf(buf, sizeof(buf), " sub_%08X x%llu", key.second, static_cast<unsigned long long>(count));
      who += buf;
    }
    if (n) REXLOG_INFO("trainer watch: {:44} {:6} reads{}", g_ranges[i].name, n, who);
  }
}

}  // namespace skate3::trainer::watch

#else

namespace skate3::trainer::watch {
void Arm(const std::vector<Target>&) {}
void Report() {}
}  // namespace skate3::trainer::watch

#endif
