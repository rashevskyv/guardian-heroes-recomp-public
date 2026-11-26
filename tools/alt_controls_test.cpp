// Host test of the alternative control scheme (switch/host/input/alt_controls.h).
//   g++ -O1 -o /tmp/alt_controls_test tools/alt_controls_test.cpp && /tmp/alt_controls_test
#include <cassert>
#include <cstdio>
#include "../switch/host/input/alt_controls.h"

int main() {
  constexpr uint16_t kA = 0x1000, kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100;
  GhAltLatch s;
  auto run = [&](uint16_t b, bool lt, int32_t ry, uint16_t want_b, bool want_lt) {
    GhAltControls(b, lt, ry, s);
    assert(b == want_b && lt == want_lt && ry == 0);
  };
  run(kA, false, 0, kA, false);                // other buttons pass
  run(0, true, 0, kR3, false);                 // ZL -> R3, no trigger
  run(kL3, false, 0, kR3, false);              // L3 -> R3, L3 itself is gone
  run(kR3, false, 0, kR3, false);              // R3 stays
  run(kLB, false, 0, kLB, false);              // L stays
  run(0, false, 16000, 0, false);              // below the press threshold
  run(0, false, 17000, 0, true);               // up -> trigger
  run(0, false, 12000, 0, true);               // hysteresis: still held
  run(0, false, 11000, 0, false);              // released
  run(0, false, -17000, kLB, false);           // down -> left shoulder
  run(0, false, -12000, kLB, false);
  run(0, false, -11000, 0, false);
  run(0, true, 32767, kR3, true);              // ZL and stick up together
  std::puts("alt_controls_test: ok");
  return 0;
}
