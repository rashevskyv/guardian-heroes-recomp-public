// gh: the guest row stride of a texture, in blocks. The Xenos tiles are 32x32 BLOCKS, so the stride is the fetch pitch
// (texels, a multiple of 32) in blocks, rounded up to 32 blocks. For 4x4 DXT blocks a pitch of 608 texels is 152 blocks
// but the data is laid out with 160 (reference: rexglue-sdk GetGuestTextureLayout, row_pitch_blocks_tile_aligned).
#pragma once
#include <cstdint>

inline uint32_t GuestPitchBlocks(uint32_t pitch_texels, uint32_t block_width) {
  uint32_t blocks = (pitch_texels + block_width - 1) / block_width;
  return (blocks + 31u) & ~31u;
}

// gh: the guest texture layout in one place, so a host test (tools/guest_texture_test.cpp) can check it. Offsets are the
// ReXGlue SDK's (texture/conversion.cpp Untile); the endian modes are Xenos Endian 1 = 8in16, 2 = 8in32, 3 = 16in32.
#include <cstddef>
#include <cstring>

namespace gh_tex {

inline uint32_t TiledRow(uint32_t y, uint32_t width_blocks, uint32_t l) {
  uint32_t macro = ((y / 32) * (width_blocks / 32)) << (l + 7), micro = ((y & 6) << 2) << l;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + l)) + ((y & 1) << 4);
}
inline uint32_t TiledCol(uint32_t x, uint32_t y, uint32_t l, uint32_t base) {
  uint32_t macro = (x / 32) << (l + 7), micro = (x & 7) << l;
  uint32_t o = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((o & ~0x1FFu) << 3) + ((o & 0x1C0) << 2) + (o & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// Copies n bytes (one block or one row) from guest to host order; a tail that is not a whole 2 or 4 byte unit is copied as is.
inline void SwapCopy(uint32_t endian, uint8_t* o, const uint8_t* i, size_t n) {
  size_t k = 0;
  if (endian == 1) for (; k + 2 <= n; k += 2) { o[k] = i[k + 1]; o[k + 1] = i[k]; }
  else if (endian == 2) for (; k + 4 <= n; k += 4) { o[k] = i[k + 3]; o[k + 1] = i[k + 2]; o[k + 2] = i[k + 1]; o[k + 3] = i[k]; }
  else if (endian == 3) for (; k + 4 <= n; k += 4) { o[k] = i[k + 2]; o[k + 1] = i[k + 3]; o[k + 2] = i[k]; o[k + 3] = i[k + 1]; }
  if (k < n) std::memcpy(o + k, i + k, n - k);
}

inline uint32_t Log2Bytes(uint32_t bpb) { return bpb >= 16 ? 4 : bpb >= 8 ? 3 : bpb >= 4 ? 2 : bpb >= 2 ? 1 : 0; }

// The bytes of guest memory a tiled bw x bh block image really covers (the SDK's GetTiledAddressUpperBound2D): the fetch
// constant's base size is smaller (608x672 DXT3: 420 KB, the data reaches 470 KB), so late writes to the tail were missed.
inline uint32_t TiledExtentBytes(uint32_t bw, uint32_t bh, uint32_t stride_blocks, uint32_t bpb) {
  const uint32_t l = Log2Bytes(bpb);
  if (!bw || !bh) return 0;
  const uint32_t y = (bh - 1) & ~31u, x = (bw - 1) & ~31u;
  return TiledCol(x, y, l, TiledRow(y, stride_blocks, l)) + (l == 0 ? 0xA00u : l == 1 ? 0xC00u : 0x400u << l);
}

// Tiled: block (x, y) of a bw x bh block image is at the 32x32 block tile layout of a stride_blocks wide surface.
inline void DetileBlocks(uint8_t* dst, const uint8_t* src, uint32_t bw, uint32_t bh, uint32_t stride_blocks, uint32_t bpb, uint32_t endian) {
  const uint32_t l = Log2Bytes(bpb);
  for (uint32_t y = 0; y < bh; y++) {
    const uint32_t row = TiledRow(y, stride_blocks, l);
    for (uint32_t x = 0; x < bw; x++) SwapCopy(endian, dst + (size_t(y) * bw + x) * bpb, src + size_t(TiledCol(x, y, l, row) >> l) * bpb, bpb);
  }
}

// Linear: rows of stride_blocks blocks.
inline void CopyLinearBlocks(uint8_t* dst, const uint8_t* src, uint32_t bw, uint32_t bh, uint32_t stride_blocks, uint32_t bpb, uint32_t endian) {
  for (uint32_t y = 0; y < bh; y++) SwapCopy(endian, dst + size_t(y) * bw * bpb, src + size_t(y) * stride_blocks * bpb, size_t(bw) * bpb);
}

}  // namespace gh_tex
