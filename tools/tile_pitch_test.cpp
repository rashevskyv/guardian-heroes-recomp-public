// Host test: tiles a synthetic DXT-sized texture with the guest layout (stride rounded to 32 blocks), then untiles it with the
// old stride (pitch / block_width) and the new one (GuestPitchBlocks). Offset functions copied from rexglue-sdk conversion.cpp.
//   g++ -O1 -o /tmp/tile_pitch_test tools/tile_pitch_test.cpp && /tmp/tile_pitch_test
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../src/gpu/tile_pitch.h"

static uint32_t Row(uint32_t y, uint32_t width, uint32_t l) {
  uint32_t macro = ((y / 32) * (width / 32)) << (l + 7), micro = ((y & 6) << 2) << l;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + l)) + ((y & 1) << 4);
}
static uint32_t Col(uint32_t x, uint32_t y, uint32_t l, uint32_t base) {
  uint32_t macro = (x / 32) << (l + 7), micro = (x & 7) << l;
  uint32_t o = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((o & ~0x1FFu) << 3) + ((o & 0x1C0) << 2) + (o & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}
// block index (in blocks) of (x, y) for a stride in blocks
static uint32_t At(uint32_t x, uint32_t y, uint32_t stride, uint32_t l) { return Col(x, y, l, Row(y, stride, l)) >> l; }

static int Check(const char* name, uint32_t w, uint32_t h, uint32_t bw, uint32_t log2_bpb) {
  uint32_t pitch = (w + 31) & ~31u;  // the fetch pitch: texels, multiple of 32
  uint32_t bwid = (w + bw - 1) / bw, bhei = (h + bw - 1) / bw;
  uint32_t good = GuestPitchBlocks(pitch, bw), old_ = pitch / bw;
  std::vector<uint32_t> mem(((bhei + 31) & ~31u) * good + 4096, 0xFFFFFFFFu);
  for (uint32_t y = 0; y < bhei; y++) for (uint32_t x = 0; x < bwid; x++) mem[At(x, y, good, log2_bpb)] = y * 4096 + x;  // tile
  uint32_t bad_old = 0, bad_new = 0;
  for (uint32_t y = 0; y < bhei; y++) for (uint32_t x = 0; x < bwid; x++) {
    if (mem[At(x, y, old_, log2_bpb) % mem.size()] != y * 4096 + x) bad_old++;
    if (mem[At(x, y, good, log2_bpb)] != y * 4096 + x) bad_new++;
  }
  std::printf("%-22s %4ux%-4u pitch %4u texels: stride old %3u new %3u blocks: wrong blocks old %6u new %u\n", name, w, h, pitch, old_, good, bad_old, bad_new);
  return bad_new != 0 || (good != old_ && bad_old == 0);
}

int main() {
  int fail = 0;
  fail |= Check("DXT1 1280x720", 1280, 720, 4, 3);   // stride already 320 = 10 * 32: unchanged
  fail |= Check("DXT2_3 608x672", 608, 672, 4, 4);
  fail |= Check("DXT2_3 800x720", 800, 720, 4, 4);
  fail |= Check("DXT2_3 840x709", 840, 709, 4, 4);
  fail |= Check("DXT1 672x720", 672, 720, 4, 3);
  fail |= Check("k_8_8_8_8 544x640", 544, 640, 1, 2);  // block 1x1: the stride was always right
  fail |= Check("k_8 832x512", 832, 512, 1, 0);
  std::puts(fail ? "FAIL" : "ok: the new stride reads back every block; the old one fails for DXT widths that are not a multiple of 128");
  return fail;
}
