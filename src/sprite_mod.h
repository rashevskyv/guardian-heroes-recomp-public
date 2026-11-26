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
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  // True-colour store (textures, SpriteMod::Init(dir, true)): the PNG as RGBA in `rgba`, no index or palette.
  bool true_colour = false;
  std::vector<uint8_t> rgba;
  // Renderer-owned handles (created on first use). A true-colour replacement uses index_image for its RGBA image.
  void* index_image = nullptr;
  void* clut_image = nullptr;
  int mod = 0;  // the mod it came from (the number given to SpriteMod::AddDir)
  bool game_palette = false;  // an indexed PNG: its indices are the game's own palette indices, the game's palette stays
  uint32_t gpu_state = 0;  // bit 0: index image uploaded, bit 1: palette uploaded
};

class SpriteMod {
 public:
  // Lists `dir` once (no per-upload file system calls). Returns the number of mod files found.
  // true_colour: keep the PNG as RGBA (the true-colour textures), no quantising.
  size_t Init(const std::string& dir, bool true_colour = false);
  // Several mods: Reset once, then AddDir for each mod folder, the mod with the highest priority first. When two
  // mods hold the same id, the mod added first wins (conflicts() lists these). AddDir returns the files it found.
  void Reset(bool true_colour = false);
  size_t AddDir(const std::string& dir, int mod);
  // Asked at every Find: is this mod switched on? Without it every mod is on.
  void SetEnabled(std::function<bool(int)> enabled) { enabled_ = std::move(enabled); }
  struct Conflict {
    std::string id;
    int winner, loser;
  };
  const std::vector<Conflict>& conflicts() const { return conflicts_; }
  // The replacement for a texture from the first switched-on mod that has it, loading it on first use; nullptr
  // when there is none or it does not load (then last_error() is set; it is cleared at every call).
  ModSprite* Find(uint32_t w, uint32_t h, uint64_t hash);
  // Some file has the id prefix "<w>x<h>_": a cheap check before hashing a texture.
  bool HasSize(uint32_t w, uint32_t h) const { return sizes_.count((uint64_t(w) << 32) | h) != 0; }
  size_t file_count() const { return file_count_; }
  const std::string& last_error() const { return error_; }

 private:
  ModSprite* Load(const std::string& path, int mod);
  struct File {
    int mod;
    std::string path;
  };
  bool true_colour_ = false;
  size_t file_count_ = 0;
  std::unordered_set<uint64_t> sizes_;  // w << 32 | h of every file's id
  std::unordered_map<std::string, std::vector<File>> files_;  // id -> files, the winning mod first
  std::unordered_map<std::string, ModSprite*> loaded_;  // path -> sprite (nullptr: failed)
  std::vector<Conflict> conflicts_;
  std::function<bool(int)> enabled_;
  std::string error_;
};

// A data mod: a folder "<mods>/<id>/" with a mod.toml and a sprites/ and/or textures/ subfolder of PNG files.
struct ModInfo {
  std::string id, name, version, author, description, kind, license;
  int order = 0;                           // lower = higher priority; equal order: by id
  std::string sprites_dir, textures_dir;   // with a trailing '/', empty when the mod has none
  bool legacy = false;                     // the flat folders mods/sprites/ and mods/textures/ of older versions
};
// Reads `key = "text"` and `key = number` lines (# comments; [tables] are ignored). False: a line is not readable.
bool ParseModToml(const std::string& text, ModInfo& out, std::string& err);
// Lists the mods of `mods_dir`, the highest priority first: by (order, id), then the legacy folders
// ("legacy-sprites", "legacy-textures", present when they hold a PNG). `notes` gets one line per skipped folder.
std::vector<ModInfo> ScanMods(const std::string& mods_dir, std::vector<std::string>* notes = nullptr);

// Exposed for the self test (tools/sprite_mod_test.cpp).
// `indices` (optional) receives the raw palette indices when the PNG is an indexed one, else it is left empty.
bool DecodePng(const std::vector<uint8_t>& file, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err,
               std::vector<uint8_t>* indices = nullptr);
bool QuantiseRgba(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, ModSprite& out);

}  // namespace gh
