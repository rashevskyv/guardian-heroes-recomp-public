// Host test of the guest texture detile (src/gpu/tile_pitch.h, the code UploadTexture runs). The reference is written the
// way ReXGlue's texture_util::GetTiledOffset2D / GetGuestTextureLayout are (one formula per block, byte offsets, stride =
// fetch pitch in texels -> blocks -> aligned to 32 blocks), the encoder tiles a synthetic block image and applies the
// guest endian swap, then DetileBlocks / CopyLinearBlocks must give every block back. Also checks that no block is read
// past the SDK's tiled address upper bound (the height does not have to be aligned in the code: the bound proves it).
//   g++ -O1 -o /tmp/guest_texture_test tools/guest_texture_test.cpp && /tmp/guest_texture_test
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/gpu/tile_pitch.h"

// --- reference: rexglue-sdk src/graphics/pipeline/texture/util.cpp -------------------------------------------------------
static int32_t RefTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t l) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (l + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << l;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}
static uint32_t RefUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t l) {
  uint32_t ub = uint32_t(RefTiledOffset2D(int32_t((right - 1) & ~31u), int32_t((bottom - 1) & ~31u), pitch, l));
  if (l == 0) return ub + 0xA00;
  if (l == 1) return ub + 0xC00;
  return ub + (0x400u << l);
}
// Layout rule of GetGuestTextureLayout for the base level: row pitch = align(align(pitch_texels, bw) / bw, 32) blocks.
static uint32_t RefStrideBlocks(uint32_t pitch_texels, uint32_t bw) { return (((pitch_texels + bw - 1) / bw) + 31) & ~31u; }

// The Xenos endian modes written as 16 / 32 bit integer operations (the SDK's copy_and_swap_* on a little endian host).
static void RefSwap(uint32_t endian, uint8_t* p, size_t n) {
  for (size_t k = 0; endian == 1 && k + 2 <= n; k += 2) { uint16_t v; std::memcpy(&v, p + k, 2); v = uint16_t((v << 8) | (v >> 8)); std::memcpy(p + k, &v, 2); }
  for (size_t k = 0; endian == 2 && k + 4 <= n; k += 4) { uint32_t v; std::memcpy(&v, p + k, 4); v = __builtin_bswap32(v); std::memcpy(p + k, &v, 4); }
  for (size_t k = 0; endian == 3 && k + 4 <= n; k += 4) { uint32_t v; std::memcpy(&v, p + k, 4); v = (v << 16) | (v >> 16); std::memcpy(p + k, &v, 4); }
}

struct Case { const char* name; uint32_t w, h, bw, bpb, pitch, endian; bool tiled; };

static int Run(const Case& c) {
  const uint32_t bwid = (c.w + c.bw - 1) / c.bw, bhei = (c.h + c.bw - 1) / c.bw;
  const uint32_t stride = RefStrideBlocks(c.pitch, c.bw);
  if (stride != GuestPitchBlocks(c.pitch, c.bw)) { std::printf("%s: stride mismatch %u vs %u\n", c.name, stride, GuestPitchBlocks(c.pitch, c.bw)); return 1; }
  const uint32_t l = gh_tex::Log2Bytes(c.bpb);
  // The picture: a pseudo random byte per position, distinct per block and per byte.
  std::vector<uint8_t> want(size_t(bwid) * bhei * c.bpb);
  uint32_t seed = 12345;
  for (auto& b : want) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
  // The guest memory: tiled (or linear) at the reference offsets, every block in guest byte order.
  const uint32_t rows_aligned = (bhei + 31) & ~31u;
  uint32_t ub = c.tiled ? RefUpperBound2D(bwid, bhei, stride, l) : stride * c.bpb * bhei;
  std::vector<uint8_t> mem(std::max<size_t>(ub, size_t(stride) * rows_aligned * c.bpb) + 4096, 0xCD);
  uint32_t max_end = 0;
  for (uint32_t y = 0; y < bhei; y++) for (uint32_t x = 0; x < bwid; x++) {
    size_t off = c.tiled ? size_t(uint32_t(RefTiledOffset2D(int32_t(x), int32_t(y), stride, l))) : (size_t(y) * stride + x) * c.bpb;
    std::memcpy(&mem[off], &want[(size_t(y) * bwid + x) * c.bpb], c.bpb);
    RefSwap(c.endian, &mem[off], c.bpb);  // the swaps are their own inverse
    if (off + c.bpb > max_end) max_end = uint32_t(off + c.bpb);
  }
  std::vector<uint8_t> got(want.size(), 0);
  if (c.tiled) gh_tex::DetileBlocks(got.data(), mem.data(), bwid, bhei, GuestPitchBlocks(c.pitch, c.bw), c.bpb, c.endian);
  else gh_tex::CopyLinearBlocks(got.data(), mem.data(), bwid, bhei, GuestPitchBlocks(c.pitch, c.bw), c.bpb, c.endian);
  size_t bad = 0;
  for (size_t i = 0; i < want.size(); i++) bad += want[i] != got[i];
  // The old stride (pitch / block width, not aligned) for the record.
  std::vector<uint8_t> old(want.size(), 0);
  const uint32_t old_stride = c.pitch / c.bw;
  size_t bad_old = 0;
  if (c.tiled) {
    gh_tex::DetileBlocks(old.data(), mem.data(), bwid, bhei, old_stride, c.bpb, c.endian);
    for (size_t i = 0; i < want.size(); i++) bad_old += want[i] != old[i];
  }
  const bool in_bound = (!c.tiled || max_end <= ub) && (!c.tiled || gh_tex::TiledExtentBytes(bwid, bhei, stride, c.bpb) == ub);
  std::printf("%-26s %5ux%-5u pitch %4u stride %3u blocks endian %u %s: wrong bytes %zu (old stride %u: %zu), used %u of bound %u bytes%s\n", c.name, c.w, c.h,
              c.pitch, stride, c.endian, c.tiled ? "tiled" : "linear", bad, old_stride, bad_old, max_end, ub, in_bound ? "" : "  PAST THE BOUND");
  return bad != 0 || !in_bound;
}

int main() {
  const Case cases[] = {
      // From the console log of 2026-11-25 (the fetch pitch is in texels and already a multiple of 32 blocks for these).
      {"DXT3 608x672 (log)", 608, 672, 4, 16, 640, 1, true},
      {"DXT1 672x720 (log)", 672, 720, 4, 8, 768, 1, true},
      {"DXT3 840x709 (log)", 840, 709, 4, 16, 896, 1, true},
      {"DXT3 1600x480 (log)", 1600, 480, 4, 16, 1664, 1, true},
      {"DXT1 1280x720 (log)", 1280, 720, 4, 8, 1280, 1, true},
      {"DXT3 7680x1536 (log)", 7680, 1536, 4, 16, 7680, 1, true},
      {"DXT1 1920x1152 (log)", 1920, 1152, 4, 8, 1920, 1, true},
      {"DXT5 768x512", 768, 512, 4, 16, 768, 1, true},
      {"8888 544x640 e2", 544, 640, 1, 4, 544, 2, true},
      {"8888 1280x720 e2", 1280, 720, 1, 4, 1280, 2, true},
      {"k_8 832x512 e0", 832, 512, 1, 1, 832, 0, true},
      {"k_8 192x288 e0", 192, 288, 1, 1, 192, 0, true},
      {"16 bit 224x160 e1", 224, 160, 1, 2, 224, 1, true},
      // Pitches that are not a multiple of 128 texels for DXT (the stride must round up to 32 blocks).
      {"DXT1 600x300 pitch 608", 600, 300, 4, 8, 608, 1, true},
      {"DXT3 1000x500 pitch 1024", 1000, 500, 4, 16, 1024, 1, true},
      {"DXT1 300x500 pitch 384", 300, 500, 4, 8, 384, 1, true},
      // 16in32 and no swap.
      {"8888 160x64 e3", 160, 64, 1, 4, 160, 3, true},
      {"DXT1 256x256 e0", 256, 256, 4, 8, 256, 0, true},
      // Linear: 4444 atlas of the UI font, 256x1 palettes.
      {"4444 2048x2225 linear e1", 2048, 2225, 1, 2, 2048, 1, false},
      {"8888 256x1 linear e2", 256, 1, 1, 4, 256, 2, false},
  };
  int fail = 0;
  for (const Case& c : cases) fail |= Run(c);
  std::puts(fail ? "FAIL" : "ok: DetileBlocks / CopyLinearBlocks match the SDK reference layout for every case");
  return fail;
}
