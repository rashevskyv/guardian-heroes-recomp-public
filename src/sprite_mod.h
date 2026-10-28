// Sprite replacement store (docs/SPRITES-RESEARCH.md, section 8). No GPU code: it finds PNG files for the game's
// 8 bit sprite textures and turns them into an index image plus a palette. The renderer (src/gpu/deko3d_backend.cpp)
// swaps them in.
//
// Identity of a sprite texture: "<w>x<h>_<hash>" with w, h the texture size and hash the crc32 << 32 | adler32 of its untiled
// bytes (w * h indices). tools/spritemod.py computes the same ids. A mod file is "<anything>__<id>.png" or
// "<id>.png" in the mod folder; the part after the last "__" is the id.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gh {

uint64_t SpriteHash(const uint8_t* data, size_t n);
std::string SpriteId(uint32_t w, uint32_t h, uint64_t hash);

// A replacement: 8 bit indices (0 = transparent) and a 256 entry RGBA palette (entry 0 transparent).
// True-colour PNGs get a palette of their own (fitted to the picture). Indexed PNGs (mode P) are taken as indices
// into the game's own palette, so the costume palettes keep working; tools/palettize.py makes those.
struct ModSprite {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> index;
  uint8_t palette[256 * 4] = {};  // R, G, B, A
  // Renderer-owned handles (created on first use).
  void* index_image = nullptr;
  void* clut_image = nullptr;
  bool game_palette = false;  // an indexed PNG: its indices are the game's own palette indices, the game's palette stays
  uint32_t gpu_state = 0;  // bit 0: index image uploaded, bit 1: palette uploaded
};

class SpriteMod {
 public:
  // Lists `dir` once (no per-upload file system calls). Returns the number of mod files found.
  size_t Init(const std::string& dir);
  // The replacement for a texture, loading it on first use; nullptr when there is none or it does not load.
  ModSprite* Find(uint32_t w, uint32_t h, uint64_t hash);
  size_t file_count() const { return files_.size(); }
  const std::string& last_error() const { return error_; }

 private:
  std::string dir_;
  std::unordered_map<std::string, std::string> files_;  // id -> path
  std::unordered_map<std::string, ModSprite*> loaded_;  // id -> sprite (nullptr: failed)
  std::string error_;
};

// Exposed for the self test (tools/sprite_mod_test.cpp).
// `indices` (optional) receives the raw palette indices when the PNG is an indexed one, else it is left empty.
bool DecodePng(const std::vector<uint8_t>& file, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err,
               std::vector<uint8_t>* indices = nullptr);
bool QuantiseRgba(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, ModSprite& out);

}  // namespace gh
