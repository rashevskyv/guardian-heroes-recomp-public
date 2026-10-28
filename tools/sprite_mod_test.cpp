// Host self test for src/sprite_mod.cpp (see tools/sprite_mod_test.py, which builds and runs it).
//   sprite_mod_test decode in.png out.rgba     raw RGBA, width and height printed
//   sprite_mod_test quant  in.png out.idx out.pal
//   sprite_mod_test hash   file                sprite hash of the file bytes
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/sprite_mod.h"

static std::vector<uint8_t> Read(const char* p) {
  std::vector<uint8_t> v;
  if (FILE* f = std::fopen(p, "rb")) {
    uint8_t b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f);
  }
  return v;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  if (!std::strcmp(argv[1], "find") && argc > 5) {  // find <dir> <w> <h> <hash hex>: the store lookup
    gh::SpriteMod m;
    size_t n = m.Init(argv[2]);
    gh::ModSprite* s = m.Find(uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])), std::strtoull(argv[5], nullptr, 16));
    std::printf("files %zu found %d", n, s ? 1 : 0);
    if (s) std::printf(" %ux%u idx0=%u pal1=%u,%u,%u,%u game=%d", s->w, s->h, s->index[0], s->palette[4], s->palette[5], s->palette[6], s->palette[7], s->game_palette ? 1 : 0);
    std::printf(" err=[%s]\n", m.last_error().c_str());
    return 0;
  }
  auto file = Read(argv[2]);
  if (!std::strcmp(argv[1], "hash")) {
    std::printf("%016llx\n", (unsigned long long)gh::SpriteHash(file.data(), file.size()));
    return 0;
  }
  uint32_t w, h;
  std::vector<uint8_t> rgba;
  std::string err;
  if (!gh::DecodePng(file, w, h, rgba, err)) { std::printf("ERR %s\n", err.c_str()); return 1; }
  std::printf("%u %u\n", w, h);
  if (!std::strcmp(argv[1], "decode") && argc > 3) {
    FILE* f = std::fopen(argv[3], "wb"); std::fwrite(rgba.data(), 1, rgba.size(), f); std::fclose(f);
  } else if (!std::strcmp(argv[1], "quant") && argc > 4) {
    gh::ModSprite s;
    gh::QuantiseRgba(rgba, w, h, s);
    FILE* f = std::fopen(argv[3], "wb"); std::fwrite(s.index.data(), 1, s.index.size(), f); std::fclose(f);
    f = std::fopen(argv[4], "wb"); std::fwrite(s.palette, 1, sizeof(s.palette), f); std::fclose(f);
  }
  return 0;
}
