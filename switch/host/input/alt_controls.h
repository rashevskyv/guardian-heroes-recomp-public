// gh: the alternative control scheme (the menu line "controls", "controls=std|alt" in settings.txt), on the pad the game
// sees (Xbox names). Pure: tools/alt_controls_test.cpp checks it on the PC.
//   right stick up   -> left trigger (the action of ZL in the standard scheme)
//   right stick down -> left shoulder (the action of L)
//   ZL and L3        -> right stick click (the action of R3); the left stick click (L3) is not sent any more
//   right stick left / right, R3, L and all other buttons: as in the standard scheme
#pragma once
#include <cstdint>

struct GhAltLatch { bool up = false, down = false; };

// buttons: XInput bits. lt: the left trigger (ZL). ry: the right stick, -32768..32767, up is positive.
inline void GhAltControls(uint16_t& buttons, bool& lt, int32_t& ry, GhAltLatch& s) {
  constexpr uint16_t kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100;
  constexpr int32_t kOn = 16384, kOff = 11500;  // half of the way to press, about a third to release
  s.up = ry > (s.up ? kOff : kOn);
  s.down = ry < -(s.down ? kOff : kOn);
  const bool r3 = lt || (buttons & (kL3 | kR3));
  buttons = uint16_t((buttons & ~(kL3 | kR3)) | (r3 ? kR3 : 0) | (s.down ? kLB : 0));
  lt = s.up;
  ry = 0;
}
