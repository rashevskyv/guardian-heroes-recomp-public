// Cheat switches as "key=0|1" lines. Switch: read from sdmc:/switch/gh/cheats.txt and settings.txt, written to
// settings.txt by the in-game menu (switch/host/input/nx_input_driver.cpp).
// Host test: tools/cheats_config_test.cpp.
#pragma once

#include <cstddef>
#include <cstdio>

struct GhCheatFlags {
  bool invincible = false;  // player health stays at its maximum
  bool mp = false;          // player magic stays at its maximum
  bool continues = false;   // 99 continues, never used up
  bool attributes = false;  // all six attributes at their maximum
};

inline void GhCheatParseLine(const char* line, GhCheatFlags& f) {
  int v = 0;
  if (std::sscanf(line, "invincible=%d", &v) == 1) f.invincible = v != 0;
  else if (std::sscanf(line, "mp=%d", &v) == 1) f.mp = v != 0;
  else if (std::sscanf(line, "continues=%d", &v) == 1) f.continues = v != 0;
  else if (std::sscanf(line, "attributes=%d", &v) == 1) f.attributes = v != 0;
}

// The four lines of a cheats file, the way GhCheatParseLine reads them back.
inline void GhCheatFormat(const GhCheatFlags& f, char* out, size_t size) {
  std::snprintf(out, size, "invincible=%d\nmp=%d\ncontinues=%d\nattributes=%d\n", f.invincible, f.mp, f.continues,
                f.attributes);
}
