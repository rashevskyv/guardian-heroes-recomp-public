// The automatic clock governor's decision logic (used by switch/host/switch_nx.cpp). Pure C++, no libnx: the host test
// tests/clock_governor_tests.cpp includes it. Adapted from the Hard Corps: Uprising port (renut ultimate base,
// switch/clock_ladders.h and govern() in hcu_nx_base.cpp, same author, GPL-3.0-or-later): the ladder, the lost-frame
// rule, the step-down wait with its doubling. The rates are steps of the pcv/clkrst tables (kHz).
// The ladders below are NOT measured on Guardian Heroes yet: they are HCU's shape with GH's proven maximum (the old
// fixed CPU 1785 / GPU 768 / memory 1600 MHz) as the top rung.
#pragma once

#include <cstdint>

namespace gh_clock {

struct Rung { uint32_t cpu, gpu; };  // kHz

// The tables the clocks may take. A value from settings.txt snaps down to one of these.
constexpr uint32_t kCpuTable[] = {1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000};
constexpr uint32_t kGpuTable[] = {307200, 384000, 460800, 537600, 614400, 691200, 768000};
constexpr uint32_t kMemTable[] = {1331200, 1600000};

// The safe maximum: never above these, whatever the settings say (they only lower the limit).
constexpr uint32_t kMaxCpu = 1785000, kMaxGpu = 768000, kMaxMem = 1600000;

struct Limits { uint32_t cpu, gpu, mem; };  // kHz
constexpr Limits kDefaultLimits = {kMaxCpu, kMaxGpu, kMaxMem};

// The highest table value at or below `khz`; the lowest table value if `khz` is below the table.
template <int N>
constexpr uint32_t snap_down(const uint32_t (&table)[N], uint32_t khz) {
  uint32_t best = table[0];
  for (uint32_t v : table) if (v <= khz) best = v;
  return best;
}

// A settings.txt value in MHz -> a limit in kHz. 0 or unset = the default; above the safe maximum = the maximum.
template <int N>
constexpr uint32_t limit_from_mhz(const uint32_t (&table)[N], uint32_t mhz, uint32_t max_khz) {
  if (mhz == 0) return max_khz;
  const uint32_t khz = mhz >= max_khz / 1000 ? max_khz : mhz * 1000;
  return snap_down(table, khz);
}

constexpr uint32_t cap(uint32_t v, uint32_t max) { return v < max ? v : max; }
constexpr Rung apply_limits(Rung r, Limits l) { return {snap_down(kCpuTable, cap(r.cpu, l.cpu)), snap_down(kGpuTable, cap(r.gpu, l.gpu))}; }

// The first rung is the start and the floor. Handheld: the stock GPU is 307.2, the CPU 1020, so every rung is above stock.
constexpr Rung kHandheld[] = {{1224000, 384000}, {1428000, 460800}, {1683000, 460800}, {1785000, 691200}, {1785000, 768000}};
// Docked: the stock GPU is 768 already, so it is not lowered (the CPU stock is 1020).
constexpr Rung kDocked[] = {{1428000, 768000}, {1581000, 768000}, {1683000, 768000}, {1785000, 768000}};

struct Ladder { const char* name; const Rung* rungs; int count; };
constexpr Ladder ladder_for(bool docked) {
  return docked ? Ladder{"docked", kDocked, int(sizeof(kDocked) / sizeof(kDocked[0]))}
                : Ladder{"handheld", kHandheld, int(sizeof(kHandheld) / sizeof(kHandheld[0]))};
}

// Frames the game is short of in a window: 60 swaps a second is full speed.
inline double lost_frames(uint32_t swaps, double secs) {
  const double lost = 60.0 - swaps / secs;
  return lost > 0.0 ? lost : 0.0;
}

// The rule. One call per judgement: `lost` is the frames lost in the last second; `slow` is true every 2 s (the
// step-down check). 3 or more lost: one rung up at once (two for 6 or more). Every 2 s: under 1 lost for `wait`
// samples in a row: one rung down; a step down that fails within 3 samples doubles the wait (5 to 64). Between
// (1 and 3 lost) the rung stays. Hysteresis = the asymmetry: up at once, down only after 10 s of calm.
struct Governor {
  int rung = 0;  // an index in the ladder; 0 is the floor
  int count = 1;
  unsigned wait = 5, good = 0, since_down = 100;

  void reset(int ladder_count) { *this = Governor{}; count = ladder_count; }

  // Returns the rung to use now (also stored in `rung`).
  int judge(bool slow, double lost) {
    int to = rung;
    if (slow) since_down++;
    if (lost >= 3.0) {
      good = 0;
      wait = since_down <= 3 ? (wait * 2 < 64 ? wait * 2 : 64) : 5;
      to = rung + (lost >= 6.0 ? 2 : 1);
      if (to > count - 1) to = count - 1;
    } else if (!slow) {
    } else if (lost < 1.0 && rung > 0) {
      if (++good >= wait) { good = 0; to = rung - 1; since_down = 0; }
    } else {
      good = 0;
    }
    rung = to;
    return to;
  }
};

// Does a clock manager (sys-clk, Horizon OC) hold a clock? One call per 2-s check with "the rate is below ours again
// although we set it at the last check". Twice in a row = it holds the clock: the caller stops and leaves the clocks
// to it. Once = HOME, dock or sleep (the system put the rate back once).
struct Contest {
  int seen = 0;
  bool lost(bool below) { seen = below ? seen + 1 : 0; return seen >= 2; }
};

}  // namespace gh_clock
