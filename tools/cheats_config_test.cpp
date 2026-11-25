// Host self test for src/cheats_config.h. Build and run from the repo root:
//   g++ -std=c++17 -o /tmp/cheats_config_test tools/cheats_config_test.cpp && /tmp/cheats_config_test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/cheats_config.h"

int main() {
  GhCheatFlags f;
  assert(!f.invincible && !f.mp && !f.continues && !f.attributes);  // all off by default
  GhCheatParseLine("invincible=1\n", f);
  GhCheatParseLine("continues=1\n", f);
  GhCheatParseLine("# attributes=1\n", f);  // a comment line does not match
  GhCheatParseLine("bogus=1\n", f);
  assert(f.invincible && f.continues && !f.mp && !f.attributes);
  GhCheatParseLine("invincible=0\n", f);
  GhCheatParseLine("mp=1", f);  // no trailing newline
  GhCheatParseLine("attributes=2\n", f);
  assert(!f.invincible && f.mp && f.continues && f.attributes);
  // Write, then read back.
  char text[128];
  GhCheatFlags g;
  g.mp = g.attributes = true;
  GhCheatFormat(g, text, sizeof(text));
  assert(std::strcmp(text, "invincible=0\nmp=1\ncontinues=0\nattributes=1\n") == 0);
  GhCheatFlags h;
  for (char* line = std::strtok(text, "\n"); line; line = std::strtok(nullptr, "\n")) GhCheatParseLine(line, h);
  assert(!h.invincible && h.mp && !h.continues && h.attributes);
  std::puts("cheats_config_test ok");
  return 0;
}
