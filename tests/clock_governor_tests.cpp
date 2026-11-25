// Host test for switch/clock_governor.h: g++ -std=c++17 -Wall -Wextra tests/clock_governor_tests.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "../switch/clock_governor.h"

using namespace gh_clock;

int main() {
  // The limits: defaults, keys only lower, never above the safe maximum, snapped to the tables.
  assert(limit_from_mhz(kCpuTable, 0, kMaxCpu) == 1785000);
  assert(limit_from_mhz(kCpuTable, 9999, kMaxCpu) == 1785000);
  assert(limit_from_mhz(kGpuTable, 9999, kMaxGpu) == 768000);
  assert(limit_from_mhz(kCpuTable, 1500, kMaxCpu) == 1428000);
  assert(limit_from_mhz(kGpuTable, 100, kMaxGpu) == 307200);  // below the table: the lowest entry
  assert(limit_from_mhz(kMemTable, 1500, kMaxMem) == 1331200);

  // Every rung of both ladders, with any limit, stays inside the limit and the table.
  for (bool docked : {false, true}) {
    const Ladder lad = ladder_for(docked);
    for (uint32_t cpu : kCpuTable) for (uint32_t gpu : kGpuTable) {
      const Limits lim = {cpu, gpu, kMaxMem};
      for (int i = 0; i < lad.count; i++) {
        const Rung r = apply_limits(lad.rungs[i], lim);
        assert(r.cpu <= kMaxCpu && r.gpu <= kMaxGpu);
        assert(r.cpu <= (cpu > 1020000 ? cpu : 1020000) && r.gpu <= (gpu > 307200 ? gpu : 307200));
      }
    }
    // The ladder goes up only, and the top is the old fixed maximum.
    for (int i = 1; i < lad.count; i++) assert(lad.rungs[i].cpu >= lad.rungs[i - 1].cpu && lad.rungs[i].gpu >= lad.rungs[i - 1].gpu);
    assert(lad.rungs[lad.count - 1].cpu == kMaxCpu && lad.rungs[lad.count - 1].gpu == kMaxGpu);
  }

  // Lost frames: 60 swaps in a second = 0, 54 = 6; never negative.
  assert(lost_frames(60, 1.0) == 0.0 && lost_frames(54, 1.0) == 6.0 && lost_frames(70, 1.0) == 0.0);

  Governor g;
  g.reset(5);
  // 1 or 2 lost frames: no change, any number of times.
  for (int i = 0; i < 100; i++) assert(g.judge(i % 20 == 19, 2.0) == 0);
  // 3 lost: one up; 6 lost: two up; clamps at the top.
  assert(g.judge(false, 3.0) == 1);
  assert(g.judge(false, 6.0) == 3);
  assert(g.judge(false, 9.0) == 4);
  assert(g.judge(false, 60.0) == 4);
  // Calm: needs `wait` (5) slow samples in a row under 1 lost, then one rung down; fast samples do not count.
  for (int i = 0; i < 50; i++) assert(g.judge(false, 0.0) == 4);
  for (int i = 0; i < 4; i++) assert(g.judge(true, 0.5) == 4);
  assert(g.judge(true, 0.5) == 3);
  // A sample with 1 to 3 lost resets the count.
  for (int i = 0; i < 4; i++) g.judge(true, 0.0);
  g.judge(true, 1.5);
  for (int i = 0; i < 4; i++) assert(g.judge(true, 0.0) == 3);
  assert(g.judge(true, 0.0) == 2);
  // A step down that fails within 3 samples doubles the wait; the floor is never passed.
  assert(g.wait == 5);
  assert(g.judge(true, 4.0) == 3 && g.wait == 10);
  while (g.rung > 0) g.judge(true, 0.0);
  for (int i = 0; i < 1000; i++) assert(g.judge(true, 0.0) == 0);
  // The wait is capped at 64.
  Governor h;
  h.reset(3);
  h.rung = 1;
  for (int i = 0; i < 20; i++) { h.since_down = 0; h.judge(false, 4.0); h.rung = 1; }
  assert(h.wait == 64);

  // A clock manager holds the clock only if the rate is below ours at two checks in a row.
  Contest c;
  assert(!c.lost(true) && !c.lost(false) && !c.lost(true) && c.lost(true));

  std::printf("clock governor tests passed\n");
  return 0;
}
