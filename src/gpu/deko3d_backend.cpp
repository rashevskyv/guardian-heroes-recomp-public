// deko3d GPU backend (--gpu_backend=deko3d, Nintendo Switch): the SDK's Xenos command processor
// parses the ring (fences, read-pointer write-back, vblank, bin masks), this backend draws with
// deko3d. No EDRAM model: a host image per render target (keyed by EDRAM base and pitch), resolves
// are image copies into images keyed by guest address, textures bound at such an address alias
// them (no readback), the swap presents the image at the front buffer's address through one
// textured triangle. Shaders are translated offline (tools/xenos2glsl.py, uam) and loaded by the
// ucode hash from sdmc:/switch/gh/shaders/. docs/native-renderer-plan.md, M3.
// cvhd-recomp, BSD-3 like the SDK.
#ifdef __SWITCH__
#include <switch.h>
#include <deko3d.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string/buffer.h>
#include <rex/system/gpu_plugin.h>

#include "tile_pitch.h"
#include "../sprite_mod.h"  // gh: sprite replacement
#include "ucode_hash.h"

// Where the DKSH shaders are (tools/xenos2glsl.py + uam, staged by switch/build.sh): a game sets it
// in its gh.toml / arguments. Missing shaders are dumped to its missing/ subdirectory. The debug
// markers are files next to the directory: dk_dump (image readback at swaps 120 and 600),
// dk_present_rt (present the widest colour render target instead of the front buffer).
REXCVAR_DEFINE_STRING(deko3d_shader_dir, "sdmc:/switch/gh/shaders/", "GPU",
                      "deko3d backend: directory of shader_<ucode hash>.vert|geom|frag.dksh and present.vert|frag.dksh")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Xenos rectangle lists (3 vertices, the 4th = v1 + v2 - v0): by default through the geometry shader
// the translator writes next to each vertex shader; the hardware fill-rectangle mode (NV_fill_rectangle,
// the same semantics) is an option for the console: Eden ignores that mode (one triangle is drawn).
REXCVAR_DEFINE_STRING(button_glyphs, "switch", "GPU",
                      "Controller button icons: switch = the Switch letter at the same position (A<->B, X<->Y), xbox = as the game draws them")
    .allowed({"switch", "xbox"});
REXCVAR_DEFINE_BOOL(deko3d_fill_rectangle, false, "GPU",
                    "deko3d backend: draw rectangle lists with the hardware fill-rectangle mode instead of a geometry shader")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The Switch host layer's dirty-page bytes (switch/host/memory_nx.cpp): one byte per 4 KB page of the
// physical arena, set by every guest store (the codegen pch) and by host writes into guest memory
// (file reads). This backend is their only consumer (there is no SharedMemory here).
extern "C" volatile uint8_t rex_nx_dirty[0x20000];
extern "C" volatile uint8_t rex_nx_dirty_coarse[0x200];
// gh: the [dk] perf line. Counted by the SDK's command processor (sdk-edits.py: WAIT_REG_MEM time by kind), by the
// guest's GPU poll hook (cheats.cpp) and by the 4P hooks (story4p.cpp); defined in switch_nx.cpp.
extern "C" volatile uint64_t rex_nx_wait_kind_ns[4], rex_nx_guest_poll_ticks, rex_nx_hook_calls;

namespace rex::graphics::deko {

namespace {

constexpr uint32_t kFrames = 2;
constexpr uint32_t kFrameDataBytes = 24u << 20;  // vertex, index and uniform data of one frame
constexpr uint32_t kFrameCmdBytes = 4u << 20;
constexpr uint32_t kStagingBytes = 24u << 20;    // texture uploads of one frame
// ponytail: a bump pool, nothing is freed (the title 132 MB, the menu 185 MB): a free list with
// eviction of textures not bound for a while is the upgrade when a stage fills it.
constexpr uint32_t kImagePoolBytes = 448u << 20;
constexpr uint32_t kCodePoolBytes = 4u << 20;
constexpr uint32_t kMaxImageDescriptors = 512;
constexpr uint32_t kSamplerCount = 64;  // min filter x mag filter x wrap x (clamp, repeat, mirror) x wrap y
constexpr uint32_t kPresentWidth = 1280;
constexpr uint32_t kPresentHeight = 720;
constexpr uint32_t kEdramTiles = 2048;  // 10 MB of 5120-byte tiles (80 x 16 x 32 bit)
constexpr uint32_t kMaxTargetHeight = 1088;
// gh: a sampler wrap code (0 clamp, 1 repeat, 2 mirrored repeat) of a fetch constant clamp mode: the forest stage backgrounds use
// mirrored repeat and streaked horizontally when it was a clamp.
constexpr uint32_t WrapCodeOf(xenos::ClampMode m) { return m == xenos::ClampMode::kRepeat ? 1 : m == xenos::ClampMode::kMirroredRepeat ? 2 : 0; }
constexpr DkWrapMode WrapOf(uint32_t code) { return code == 1 ? DkWrapMode_Repeat : code == 2 ? DkWrapMode_MirroredRepeat : DkWrapMode_ClampToEdge; }

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("[dk] ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
  // gh: no flush here. Each flush is a write to the SD card on the command processor's thread; the fights of 0.111
  // wrote 35-45 lines a second. switch_nx.cpp flushes the log twice a second and in the crash handler.
}

// gh: per-second profile ([dk] perf): adds the time of its scope to a counter.
struct Tick {
  uint64_t& sum;
  uint64_t t0 = armGetSystemTick();
  ~Tick() { sum += armGetSystemTick() - t0; }
};

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
  Log("deko3d %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
}

struct MemBlock {
  DkMemBlock block = nullptr;
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = 0;
  uint32_t size = 0;
  uint32_t used = 0;
  uint32_t peak = 0;
  bool Create(DkDevice device, uint32_t bytes, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, bytes);
    maker.flags = flags;
    block = dkMemBlockCreate(&maker);
    if (!block) return false;
    size = bytes;
    cpu = (flags & DkMemBlockFlags_CpuUncached) || (flags & DkMemBlockFlags_CpuCached)
              ? static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block))
              : nullptr;
    gpu = dkMemBlockGetGpuAddr(block);
    return true;
  }
  // Bump allocation; UINT32_MAX when full.
  uint32_t Alloc(uint32_t bytes, uint32_t align) {
    uint32_t off = (used + align - 1) & ~(align - 1);
    if (off + bytes > size) return UINT32_MAX;
    used = off + bytes;
    if (used > peak) peak = used;
    return off;
  }
  void Reset() { used = 0; }
};

struct HostImage {
  DkImage image{};
  DkImageLayout layout{};
  DkImageFormat format = DkImageFormat_RGBA8_Unorm;
  uint32_t width = 0, height = 0;
  bool is_depth = false;
  uint32_t pool_off = 0, pool_size = 0, pool_align = 0;  // gh: its slot in the image pool (a full pool reuses a recycled slot)
  uint32_t flags = 0;         // gh: the layout flags (a recycled image must match)
  bool from_resolve = false;  // host-native channels: the fetch swizzle is not applied
  uint32_t content_serial = 0;  // the draw serial when the contents were set (upload or resolve); 0 = never
  uint32_t checked_serial = 0;  // the draw serial of the last dirty-page check
  uint64_t content_hash = 0;    // gh: textures of 4 KB or less (palettes): the hash of the guest bytes at the upload
  uint32_t guest_base = 0;    // textures and resolve destinations
  uint32_t edram_base = 0, edram_pitch = 0;  // render targets
  ::gh::ModSprite* mod = nullptr;  // gh: the replacement for this texture's contents (8 bit sprite or true-colour), if a mod folder has one
  // gh: what the upload looked up, so a mod switched in the menu takes effect on the next draw (no new upload needed).
  uint8_t mod_kind = 0;    // 0 no lookup, 1 sprite store, 2 texture store
  uint64_t mod_hash = 0;
  int mod_epoch = 0;       // rex_nx_mod_epoch at the lookup
};

class DkShaderObj : public Shader {
 public:
  DkShaderObj(xenos::ShaderType type, uint64_t hash, const uint32_t* dwords, uint32_t count)
      : Shader(type, hash, dwords, count) {}
  DkShader shader{};
  bool loaded = false;
  DkShader geom{};           // vertex shaders: the rectangle-list geometry shader
  bool geom_loaded = false;
};

struct Frame {
  DkCmdBuf cmdbuf = nullptr;
  uint32_t cmd_offset = 0;
  MemBlock data;     // vertex/index/uniform ring
  MemBlock staging;  // texture uploads
  uint32_t desc_offset = 0;  // image descriptors of this frame, inside `data`? no: a separate static block
  DkFence fence{};
  bool fenced = false;
  uint32_t next_descriptor = 0;
  std::unordered_map<uint64_t, uint32_t> descriptors;  // image (and swizzle) -> descriptor slot this frame
};

struct SysConstants {
  float ndc_scale[4];
  float ndc_offset[4];
  float alpha[4];
  uint32_t bools[8];
  uint32_t loops[32];  // gh: integer loop constants i0..i31 (count | start << 8 | step << 16)
};

DkCompareOp CompareOp(xenos::CompareFunction f) { return DkCompareOp(uint32_t(f)); }

DkBlendFactor BlendFactor(xenos::BlendFactor f) {
  switch (f) {
    case xenos::BlendFactor::kZero: return DkBlendFactor_Zero;
    case xenos::BlendFactor::kOne: return DkBlendFactor_One;
    case xenos::BlendFactor::kSrcColor: return DkBlendFactor_SrcColor;
    case xenos::BlendFactor::kOneMinusSrcColor: return DkBlendFactor_InvSrcColor;
    case xenos::BlendFactor::kSrcAlpha: return DkBlendFactor_SrcAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
    case xenos::BlendFactor::kDstColor: return DkBlendFactor_DstColor;
    case xenos::BlendFactor::kOneMinusDstColor: return DkBlendFactor_InvDstColor;
    case xenos::BlendFactor::kDstAlpha: return DkBlendFactor_DstAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha: return DkBlendFactor_InvDstAlpha;
    case xenos::BlendFactor::kConstantColor: return DkBlendFactor_ConstColor;
    case xenos::BlendFactor::kOneMinusConstantColor: return DkBlendFactor_InvConstColor;
    case xenos::BlendFactor::kConstantAlpha: return DkBlendFactor_ConstAlpha;
    case xenos::BlendFactor::kOneMinusConstantAlpha: return DkBlendFactor_InvConstAlpha;
    case xenos::BlendFactor::kSrcAlphaSaturate: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
  }
}

DkBlendOp BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return DkBlendOp_Add;
    case xenos::BlendOp::kSubtract: return DkBlendOp_Sub;
    case xenos::BlendOp::kMin: return DkBlendOp_Min;
    case xenos::BlendOp::kMax: return DkBlendOp_Max;
    case xenos::BlendOp::kRevSubtract: return DkBlendOp_RevSub;
    default: return DkBlendOp_Add;
  }
}

bool PrimitiveOf(xenos::PrimitiveType t, DkPrimitive& out, bool& rects) {
  rects = false;
  switch (t) {
    case xenos::PrimitiveType::kPointList: out = DkPrimitive_Points; return true;
    case xenos::PrimitiveType::kLineList: out = DkPrimitive_Lines; return true;
    case xenos::PrimitiveType::kLineStrip: out = DkPrimitive_LineStrip; return true;
    case xenos::PrimitiveType::kLineLoop: out = DkPrimitive_LineLoop; return true;
    case xenos::PrimitiveType::kTriangleList: out = DkPrimitive_Triangles; return true;
    case xenos::PrimitiveType::kTriangleFan: out = DkPrimitive_TriangleFan; return true;
    case xenos::PrimitiveType::kTriangleStrip: out = DkPrimitive_TriangleStrip; return true;
    case xenos::PrimitiveType::kRectangleList: out = DkPrimitive_Triangles; rects = true; return true;
    case xenos::PrimitiveType::kQuadList: out = DkPrimitive_Quads; return true;
    case xenos::PrimitiveType::kQuadStrip: out = DkPrimitive_QuadStrip; return true;
    case xenos::PrimitiveType::kPolygon: out = DkPrimitive_Polygon; return true;
    default: return false;
  }
}

bool VertexAttribOf(const ParsedVertexFetchInstruction::Attributes& a, DkVtxAttribSize& size, DkVtxAttribType& type) {
  using VF = xenos::VertexFormat;
  DkVtxAttribType norm = a.is_integer ? (a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled)
                                      : (a.is_signed ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm);
  type = norm;
  switch (a.data_format) {
    case VF::k_8_8_8_8: size = DkVtxAttribSize_4x8; return true;
    case VF::k_2_10_10_10: size = DkVtxAttribSize_10_10_10_2; return true;
    case VF::k_16_16: size = DkVtxAttribSize_2x16; return true;
    case VF::k_16_16_16_16: size = DkVtxAttribSize_4x16; return true;
    case VF::k_16_16_FLOAT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Float; return true;
    case VF::k_16_16_16_16_FLOAT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Float; return true;
    case VF::k_32: size = DkVtxAttribSize_1x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_32: size = DkVtxAttribSize_2x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_32_32_32: size = DkVtxAttribSize_4x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_FLOAT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_FLOAT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_32_FLOAT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_32_32_FLOAT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Float; return true;
    case VF::k_10_11_11: case VF::k_11_11_10: size = DkVtxAttribSize_11_11_10; type = DkVtxAttribType_Float; return true;
    default: return false;
  }
}

bool TextureFormatOf(xenos::TextureFormat f, DkImageFormat& out) {
  using TF = xenos::TextureFormat;
  switch (f) {
    case TF::k_8_8_8_8: case TF::k_8_8_8_8_A: out = DkImageFormat_RGBA8_Unorm; return true;
    case TF::k_8: case TF::k_8_A: case TF::k_8_B: out = DkImageFormat_R8_Unorm; return true;
    case TF::k_8_8: out = DkImageFormat_RG8_Unorm; return true;
    case TF::k_DXT1: out = DkImageFormat_RGBA_BC1; return true;
    case TF::k_DXT2_3: out = DkImageFormat_RGBA_BC2; return true;
    case TF::k_DXT4_5: out = DkImageFormat_RGBA_BC3; return true;
    case TF::k_4_4_4_4: out = DkImageFormat_RGBA4_Unorm; return true;
    case TF::k_5_6_5: out = DkImageFormat_RGB565_Unorm; return true;
    case TF::k_1_5_5_5: out = DkImageFormat_RGB5A1_Unorm; return true;
    case TF::k_16_16_16_16_FLOAT: out = DkImageFormat_RGBA16_Float; return true;
    case TF::k_32_FLOAT: out = DkImageFormat_R32_Float; return true;
    case TF::k_16_FLOAT: out = DkImageFormat_R16_Float; return true;
    case TF::k_16: out = DkImageFormat_R16_Unorm; return true;
    case TF::k_16_16: out = DkImageFormat_RG16_Unorm; return true;
    default: return false;
  }
}

DkImageFormat ColorTargetFormatOf(xenos::ColorRenderTargetFormat f) {
  switch (f) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return DkImageFormat_RGBA8_Unorm_sRGB;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10: return DkImageFormat_RGB10A2_Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return DkImageFormat_RGBA16_Float;
    default: return DkImageFormat_RGBA8_Unorm;
  }
}

DkImageSwizzle SwizzleOf(uint32_t s) {
  switch (s & 7) {
    case 0: return DkImageSwizzle_Red;
    case 1: return DkImageSwizzle_Green;
    case 2: return DkImageSwizzle_Blue;
    case 3: return DkImageSwizzle_Alpha;
    case 4: return DkImageSwizzle_Zero;
    default: return DkImageSwizzle_One;
  }
}

// ----- button glyphs (gh) ---------------------------------------------------------------------------------
// The game draws Xbox button icons (A green, B red, X blue, Y yellow). The Switch pad maps by position (the
// bottom button is Xbox A), so the icons must show the Switch letter at the same position: A<->B, X<->Y.
// The icons are characters of the UI font atlas (k_4_4_4_4 2048x2225, linear: one row of 22x24 icons, pitch 25
// px) and one baked into the title's "PRESS A BUTTON" texture (k_8_8_8_8 288x96). Both are patched in the
// staged, untiled data before the upload: the atlas cells are swapped, the title's A disc is replaced by the
// atlas B icon. Cvar button_glyphs = switch (default) | xbox. docs/BUTTON-GLYPHS.md.
struct GlyphCell {
  uint32_t x, y;  // top-left of the 24x26 cell (icon + 1 px margin)
};
constexpr uint32_t kGlyphW = 24, kGlyphH = 26;
constexpr GlyphCell kAtlasGlyph[4] = {{125, 49}, {150, 49}, {175, 49}, {200, 49}};  // A, B, X, Y
constexpr uint32_t kAtlasW = 2048, kAtlasH = 2225;
constexpr uint32_t kAtlasPhysical = 0x1D89E000;  // where the font atlas sits in every run so far (a hint, validated)

// The 4_4_4_4 texel at (x, y) of a staged atlas as 8-bit B, G, R, A.
static void Texel4444(const uint8_t* atlas, uint32_t x, uint32_t y, uint8_t* bgra) {
  const uint8_t* p = atlas + (size_t(y) * kAtlasW + x) * 2;
  const uint16_t v = uint16_t(p[0] | (p[1] << 8));
  bgra[0] = uint8_t((v & 15) * 17);
  bgra[1] = uint8_t(((v >> 4) & 15) * 17);
  bgra[2] = uint8_t(((v >> 8) & 15) * 17);
  bgra[3] = uint8_t(((v >> 12) & 15) * 17);
}

// Hue in degrees (0..359) of an RGB colour, or -1 for a grey.
static int HueOf(int r, int g, int b) {
  const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
  if (mx - mn < 90) return -1;
  float h;
  if (mx == r) h = 60.0f * float(g - b) / float(mx - mn);
  else if (mx == g) h = 120.0f + 60.0f * float(b - r) / float(mx - mn);
  else h = 240.0f + 60.0f * float(r - g) / float(mx - mn);
  if (h < 0) h += 360.0f;
  return int(h);
}

// Cell `i` looks like the Xbox button i: the hue of its saturated pixels is yellow-green (A, ~70), red (B, 0),
// blue (X, ~216) or orange (Y, ~37).
static bool AtlasCellIs(const uint8_t* atlas, int i) {
  const int lo[4] = {55, -1, 190, 20}, hi[4] = {110, 15, 250, 50};
  uint32_t hits = 0;
  for (uint32_t y = 0; y < kGlyphH; y++)
    for (uint32_t x = 0; x < kGlyphW; x++) {
      uint8_t t[4];
      Texel4444(atlas, kAtlasGlyph[i].x + x, kAtlasGlyph[i].y + y, t);
      if (t[3] < 128) continue;
      int h = HueOf(t[2], t[1], t[0]);
      if (i == 1 && h >= 345) h = 0;
      if (h >= lo[i] && h <= hi[i] && h >= 0) hits++;
    }
  return hits > 80;
}

// Swaps the A<->B and X<->Y cells of the staged atlas. Returns true when the atlas looked as expected.
static bool SwapAtlasGlyphs(uint8_t* atlas) {
  for (int i = 0; i < 4; i++)
    if (!AtlasCellIs(atlas, i)) return false;
  for (int pair = 1; pair < 2; pair++) {  // X<->Y only: Switch A/B now map to the Xbox A/B by name, so A and B keep their cells
    const GlyphCell a = kAtlasGlyph[pair * 2], b = kAtlasGlyph[pair * 2 + 1];
    for (uint32_t y = 0; y < kGlyphH; y++) {
      uint8_t* ra = atlas + (size_t(a.y + y) * kAtlasW + a.x) * 2;
      uint8_t* rb = atlas + (size_t(b.y + y) * kAtlasW + b.x) * 2;
      for (uint32_t k = 0; k < kGlyphW * 2; k++) std::swap(ra[k], rb[k]);
    }
  }
  return true;
}

// Replaces the green A disc of the title's "PRESS A BUTTON" (staged k_8_8_8_8, bytes B G R A, 288x96, disc at
// 126..148 x 36..58) by the red B icon read from the original atlas data `atlas` (the B cell, not swapped yet).
[[maybe_unused]] static bool ReplaceTitleGlyph(uint8_t* tex, const uint8_t* atlas) {
  constexpr int cx = 137, cy = 47, radius = 13, w = 288;
  uint32_t green = 0;
  for (int y = cy - 11; y <= cy + 11; y++)
    for (int x = cx - 11; x <= cx + 11; x++) {
      const uint8_t* p = tex + (size_t(y) * w + x) * 4;
      const int hue = HueOf(p[2], p[1], p[0]);
      if (hue >= 55 && hue <= 110) green++;
    }
  if (green < 120) return false;
  // The disc's surroundings: the black glow between the words (left and right of the disc).
  uint8_t fill[4];
  for (int k = 0; k < 4; k++)
    fill[k] = uint8_t((tex[(size_t(cy) * w + cx - 15) * 4 + k] + tex[(size_t(cy) * w + cx + 15) * 4 + k]) / 2);
  const GlyphCell b = kAtlasGlyph[1];  // B
  for (int y = cy - radius; y <= cy + radius; y++)
    for (int x = cx - radius; x <= cx + radius; x++) {
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) continue;
      uint8_t* p = tex + (size_t(y) * w + x) * 4;
      std::memcpy(p, fill, 4);
      // The B icon is 22x24 with its centre at (11, 12) of the cell offset by the 1 px margin.
      const int gx = x - cx + 12, gy = y - cy + 13;
      if (gx < 0 || gy < 0 || gx >= int(kGlyphW) || gy >= int(kGlyphH)) continue;
      uint8_t t[4];
      Texel4444(atlas, b.x + gx, b.y + gy, t);
      const int a = t[3];
      for (int k = 0; k < 3; k++) p[k] = uint8_t((t[k] * a + p[k] * (255 - a)) / 255);
      p[3] = uint8_t(std::max<int>(p[3], a));
    }
  return true;
}

// ----- texture dumper (gh debug) ------------------------------------------------------------------------
// The marker file sdmc:/switch/gh/dk_dump_tex makes UploadTexture write every distinct texture as a PPM to
// sdmc:/switch/gh/tex/ (decoded from the staged, untiled, endian-swapped data; alpha composited on grey):
// used to find the controller button glyphs (docs/BUTTON-GLYPHS.md).
struct RgbImage {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgb;
};

static void BlendGrey(uint8_t* out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  out[0] = uint8_t((r * a + 128 * (255 - a)) / 255);
  out[1] = uint8_t((g * a + 128 * (255 - a)) / 255);
  out[2] = uint8_t((b * a + 128 * (255 - a)) / 255);
}

static void Rgb565(uint16_t c, uint8_t* o) {
  o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((c & 31) * 255 / 31);
}

static bool DecodeStaged(const char* fmt, const uint8_t* s, uint32_t w, uint32_t h, RgbImage& img) {
  img.w = w;
  img.h = h;
  img.rgb.assign(size_t(w) * h * 3, 0);
  const std::string f(fmt);
  if (f == "k_8_8_8_8") {
    for (size_t i = 0; i < size_t(w) * h; i++) BlendGrey(&img.rgb[i * 3], s[i * 4 + 2], s[i * 4 + 1], s[i * 4], s[i * 4 + 3]);
    return true;
  }
  if (f == "k_4_4_4_4") {
    for (size_t i = 0; i < size_t(w) * h; i++) {
      const uint16_t v = uint16_t(s[i * 2] | (s[i * 2 + 1] << 8));
      BlendGrey(&img.rgb[i * 3], uint8_t(((v >> 8) & 15) * 17), uint8_t(((v >> 4) & 15) * 17), uint8_t((v & 15) * 17), uint8_t(((v >> 12) & 15) * 17));
    }
    return true;
  }
  if (f == "k_8") {
    for (size_t i = 0; i < size_t(w) * h; i++) img.rgb[i * 3] = img.rgb[i * 3 + 1] = img.rgb[i * 3 + 2] = s[i];
    return true;
  }
  if (f == "k_DXT1" || f == "k_DXT2_3" || f == "k_DXT4_5") {
    const uint32_t bpb = f == "k_DXT1" ? 8 : 16;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
      for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t* b = s + (size_t(by) * bw + bx) * bpb;
        const uint8_t* c = b + (bpb == 16 ? 8 : 0);
        uint16_t c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
        uint8_t col[4][4];
        Rgb565(c0, col[0]);
        Rgb565(c1, col[1]);
        const bool four = bpb == 16 || c0 > c1;
        for (int k = 0; k < 3; k++) {
          if (four) {
            col[2][k] = uint8_t((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = uint8_t((col[0][k] + 2 * col[1][k]) / 3);
          } else {
            col[2][k] = uint8_t((col[0][k] + col[1][k]) / 2);
            col[3][k] = 0;
          }
        }
        uint8_t alpha[16];
        for (int i = 0; i < 16; i++) alpha[i] = 255;
        if (f == "k_DXT2_3") {
          for (int i = 0; i < 16; i++) alpha[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
        } else if (f == "k_DXT4_5") {
          uint8_t a0 = b[0], a1 = b[1], tab[8];
          tab[0] = a0;
          tab[1] = a1;
          if (a0 > a1) for (int k = 1; k < 7; k++) tab[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
          else { for (int k = 1; k < 5; k++) tab[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5); tab[6] = 0; tab[7] = 255; }
          uint64_t bits = 0;
          for (int k = 0; k < 6; k++) bits |= uint64_t(b[2 + k]) << (8 * k);
          for (int i = 0; i < 16; i++) alpha[i] = tab[(bits >> (3 * i)) & 7];
        }
        uint32_t idx = c[4] | (c[5] << 8) | (c[6] << 16) | (uint32_t(c[7]) << 24);
        for (int i = 0; i < 16; i++) {
          const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          if (x >= w || y >= h) continue;
          const uint8_t* q = col[(idx >> (2 * i)) & 3];
          BlendGrey(&img.rgb[(size_t(y) * w + x) * 3], q[0], q[1], q[2], alpha[i]);
        }
      }
    return true;
  }
  return false;
}

// ----- on-screen version / fps strip (gh) ---------------------------------------------------------
// Rule: every build shows its version on screen. There is no ImGui under deko3d, so the strip is a
// 5x7 bitmap font rendered on the CPU into a small RGBA image per frame in flight and drawn with the
// present shaders into a viewport at the bottom-left corner. The menu item "overlay" (saved in settings.txt) or the
// file sdmc:/switch/gh/overlay_off hides it.
extern "C" const char* rex_nx_version(void);

constexpr uint32_t kOverlayW = 512, kOverlayH = 20, kOverlayScale = 2;
// The settings menu (Start+Back, see switch/host/input/nx_input_driver.cpp): a 256x214 panel, 9 lines.
constexpr uint32_t kMenuW = 400, kMenuH = 296;  // 32 characters a line; 9 list lines (the main page: 11, no description), 3 description lines, the key line
constexpr int kMenuCols = 32;
extern "C" volatile int rex_nx_menu_open;
extern "C" volatile int rex_nx_menu_cursor;
extern "C" volatile int rex_nx_menu_page;  // 0 main, 1 cheats, 2 mods
extern "C" const char* rex_nx_mod_description(int i);
extern "C" const char* rex_nx_mod_info(int i);
extern "C" volatile int rex_nx_mod_epoch;
extern "C" int rex_nx_mod_count(void);
extern "C" const char* rex_nx_mod_name(int i);
extern "C" int rex_nx_mod_on(int i);
extern "C" int rex_nx_register_mod(const char* id, const char* name, const char* description, const char* info);
extern "C" volatile int rex_nx_show_fps;
extern "C" volatile int rex_nx_sound;
extern "C" volatile int rex_nx_controls_alt;  // the menu: 1 = the alternative control scheme (controls=alt)
extern "C" volatile int rex_nx_frame_skip;   // the menu: 1 = draw every second guest frame (fps_cap=30)
extern "C" volatile int rex_nx_hatching;     // the menu: 0 = no pencil hatching (PS CE23671D gets an empty mask)
extern "C" volatile int rex_nx_overlay;
extern "C" volatile int rex_nx_cheat_invincible, rex_nx_cheat_mp, rex_nx_cheat_continues, rex_nx_cheat_attributes;
extern "C" void rex_nx_load_settings(void);
extern "C" volatile int rex_nx_start_level;
extern "C" const char* rex_nx_level_name(int n);

struct Glyph {
  char ch;
  uint8_t rows[7];  // 5 bits per row, bit 4 = leftmost pixel
};
constexpr Glyph kGlyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'a', {0, 0, 14, 1, 15, 17, 15}},
    {'b', {16, 16, 22, 25, 17, 17, 30}},
    {'c', {0, 0, 14, 16, 16, 17, 14}},
    {'d', {1, 1, 13, 19, 17, 17, 15}},
    {'e', {0, 0, 14, 17, 31, 16, 14}},
    {'f', {6, 9, 8, 28, 8, 8, 8}},
    {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'h', {16, 16, 22, 25, 17, 17, 17}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'v', {0, 0, 17, 17, 17, 10, 4}},
    {'p', {0, 0, 30, 17, 30, 16, 16}},
    {'s', {0, 0, 15, 16, 14, 1, 30}},
    {'g', {0, 0, 15, 17, 17, 15, 1}},
    {'i', {4, 0, 12, 4, 4, 4, 14}},
    {'k', {16, 16, 18, 20, 24, 20, 18}},
    {'l', {12, 4, 4, 4, 4, 4, 14}},
    {'m', {0, 0, 26, 21, 21, 21, 21}},
    {'n', {0, 0, 22, 25, 17, 17, 17}},
    {'o', {0, 0, 14, 17, 17, 17, 14}},
    {'r', {0, 0, 22, 25, 16, 16, 16}},
    {'t', {8, 8, 30, 8, 8, 9, 6}},
    {'u', {0, 0, 17, 17, 17, 19, 13}},
    {'x', {0, 0, 17, 10, 4, 10, 17}},
    {'y', {0, 0, 17, 17, 15, 1, 14}},
    {'>', {8, 4, 2, 1, 2, 4, 8}},
    {':', {0, 12, 12, 0, 12, 12, 0}},
    {'j', {2, 0, 6, 2, 2, 18, 12}},
    {'q', {0, 0, 13, 19, 15, 1, 1}},
    {'w', {0, 0, 17, 17, 21, 21, 10}},
    {'z', {0, 0, 31, 2, 4, 8, 31}},
    {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},
    {',', {0, 0, 0, 0, 12, 4, 8}},
    {'\'', {4, 4, 0, 0, 0, 0, 0}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},
    {'&', {12, 18, 20, 8, 21, 18, 13}},
};

// Menu text of a mod: what the font can draw. One '?' for every other character (a UTF-8 letter counts once).
static std::string MenuPlain(const char* text) {
  std::string out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; p++) {
    if ((*p & 0xC0) == 0x80) continue;  // the tail of a UTF-8 character
    char ch = char(*p);
    if (ch >= 'A' && ch <= 'Z' && ch != 'G' && ch != 'H') ch = char(ch - 'A' + 'a');
    bool known = ch == ' ';
    for (const Glyph& g : kGlyphs) known |= g.ch == ch;
    out += known ? ch : '?';
  }
  return out;
}

// Splits text into lines of at most `cols` characters at spaces (a longer word is cut). At most `max_lines`; text
// that does not fit ends with "..".
static std::vector<std::string> MenuWrap(const std::string& text, size_t cols, size_t max_lines) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size() && lines.size() < max_lines) {
    while (pos < text.size() && text[pos] == ' ') pos++;
    size_t len = std::min(cols, text.size() - pos);
    if (pos + len < text.size()) {
      const size_t sp = text.rfind(' ', pos + len);
      if (sp != std::string::npos && sp > pos) len = sp - pos;
    }
    if (len) lines.push_back(text.substr(pos, len));
    pos += len;
  }
  while (pos < text.size() && text[pos] == ' ') pos++;
  if (pos < text.size() && !lines.empty()) {
    std::string& last = lines.back();
    if (last.size() > cols - 2) last.resize(cols - 2);
    last += "..";
  }
  return lines;
}

// Draws `text` into px (width w) at (x0, y0) in white, 5x7 glyphs at scale 2.
static void DrawText(uint32_t* px, uint32_t w, uint32_t x0, uint32_t y0, const char* text) {
  uint32_t pen = x0;
  for (const char* p = text; *p && pen + 12 <= w; p++) {
    for (const Glyph& cand : kGlyphs) {
      if (cand.ch != *p) continue;
      for (uint32_t row = 0; row < 7; row++)
        for (uint32_t col = 0; col < 5; col++)
          if (cand.rows[row] & (16u >> col))
            for (uint32_t sy = 0; sy < 2; sy++)
              for (uint32_t sx = 0; sx < 2; sx++) px[(y0 + row * 2 + sy) * w + pen + col * 2 + sx] = 0xFFFFFFFFu;
      break;
    }
    pen += 12;
  }
}

// Fills px (kOverlayW x kOverlayH, RGBA8 little-endian = 0xAABBGGRR) with `text` in white on black.
void RenderOverlayText(uint32_t* px, const char* text) {
  for (uint32_t i = 0; i < kOverlayW * kOverlayH; i++) px[i] = 0xFF000000u;
  uint32_t pen = 4;
  for (const char* p = text; *p && pen + 6 * kOverlayScale <= kOverlayW; p++) {
    const Glyph* g = nullptr;
    for (const Glyph& cand : kGlyphs) if (cand.ch == *p) { g = &cand; break; }
    if (g) {
      for (uint32_t row = 0; row < 7; row++)
        for (uint32_t col = 0; col < 5; col++)
          if (g->rows[row] & (16u >> col))
            for (uint32_t sy = 0; sy < kOverlayScale; sy++)
              for (uint32_t sx = 0; sx < kOverlayScale; sx++)
                px[(3 + row * kOverlayScale + sy) * kOverlayW + pen + col * kOverlayScale + sx] = 0xFFFFFFFFu;
    }
    pen += 6 * kOverlayScale;
  }
}

class Deko3dCommandProcessor : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

 protected:
  bool SetupContext() override {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = DebugCallback;
    device_ = dkDeviceCreate(&dm);
    if (!device_) { Log("dkDeviceCreate failed"); return false; }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics;
    queue_ = dkQueueCreate(&qm);
    if (!queue_) { Log("dkQueueCreate failed"); return false; }
    if (!code_.Create(device_, kCodePoolBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code) ||
        !images_.Create(device_, kImagePoolBytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image) ||
        !descriptors_.Create(device_, DK_MEMBLOCK_ALIGNMENT * 4 * kFrames + DK_MEMBLOCK_ALIGNMENT * 2, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached) ||
        !cmdmem_.Create(device_, kFrameCmdBytes * kFrames, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
      Log("memory block creation failed");
      return false;
    }
    for (uint32_t i = 0; i < kFrames; i++) {
      Frame& f = frames_[i];
      if (!f.data.Create(device_, kFrameDataBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached) ||
          !f.staging.Create(device_, kStagingBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
        Log("frame %u memory failed", i);
        return false;
      }
      DkCmdBufMaker cm;
      dkCmdBufMakerDefaults(&cm, device_);
      f.cmdbuf = dkCmdBufCreate(&cm);
      f.cmd_offset = i * kFrameCmdBytes;
      f.desc_offset = i * kMaxImageDescriptors * sizeof(DkImageDescriptor);
    }
    sampler_desc_offset_ = kFrames * kMaxImageDescriptors * sizeof(DkImageDescriptor);
    // Samplers: min filter (bit 5) x mag filter (bit 4) x wrap x (bits 3-2) x wrap y (bits 1-0), wrap 0 clamp, 1 repeat, 2 mirrored repeat; the text
    // lost thin strokes when its minification took the nearest magnification filter.
    for (uint32_t i = 0; i < kSamplerCount; i++) {
      DkSampler s;
      dkSamplerDefaults(&s);
      s.minFilter = (i & 32) ? DkFilter_Linear : DkFilter_Nearest;
      s.magFilter = (i & 16) ? DkFilter_Linear : DkFilter_Nearest;
      s.mipFilter = DkMipFilter_None;
      s.wrapMode[0] = WrapOf((i >> 2) & 3);
      s.wrapMode[1] = WrapOf(i & 3);
      s.wrapMode[2] = DkWrapMode_ClampToEdge;
      DkSamplerDescriptor sd;
      dkSamplerDescriptorInitialize(&sd, &s);
      std::memcpy(descriptors_.cpu + sampler_desc_offset_ + i * sizeof(DkSamplerDescriptor), &sd, sizeof(sd));
    }
    // The swapchain on the default window.
    for (uint32_t i = 0; i < kFrames; i++) {
      HostImage* img = CreateImage(kPresentWidth, kPresentHeight, DkImageFormat_RGBA8_Unorm,
                                   DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression, false);
      if (!img) { Log("swapchain image %u failed", i); return false; }
      swap_images_[i] = img;
      swap_image_ptrs_[i] = &img->image;
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device_, nwindowGetDefault(), swap_image_ptrs_, kFrames);
    swapchain_ = dkSwapchainCreate(&sm);
    if (!swapchain_) { Log("dkSwapchainCreate failed"); return false; }
    shader_dir_ = REXCVAR_GET(deko3d_shader_dir);
    if (shader_dir_.empty() || shader_dir_.back() != '/') shader_dir_ += '/';
    size_t cut = shader_dir_.size() >= 2 ? shader_dir_.rfind('/', shader_dir_.size() - 2) : std::string::npos;
    std::string debug_dir = cut == std::string::npos ? "" : shader_dir_.substr(0, cut + 1);
    debug_dir_ = debug_dir;
    present_target_ = access((debug_dir + "dk_present_rt").c_str(), F_OK) == 0;
    dump_images_ = access((debug_dir + "dk_dump").c_str(), F_OK) == 0;  // readback dumps stall the GPU: debug only
    dump_tex_ = access((debug_dir + "dk_dump_tex").c_str(), F_OK) == 0;
    // gh: the per-draw log lines (each shader pair, texture and texture set once) and the sprite diagnostics cost
    // string keys and set lookups in every draw: only with this marker.
    draw_diag_ = access((debug_dir + "dk_diag").c_str(), F_OK) == 0;
    {
      // gh: data mods (src/sprite_mod.h, docs/SPRITE-REPLACEMENT.md): sdmc:/switch/gh/mods/<id>/ with a mod.toml and
      // sprites/<name>__<id>.png (8 bit sprites) and/or textures/<name>__<id>.png (true-colour, drawn as RGBA). The
      // flat folders mods/sprites/ and mods/textures/ of older versions are the mods legacy-sprites and legacy-textures.
      sprite_mod_.Reset(false);
      texture_mod_.Reset(true);
      std::vector<std::string> notes;
      const std::vector<::gh::ModInfo> mods = ::gh::ScanMods(debug_dir + "mods/", &notes);
      for (const std::string& t : notes) Log("mods: %s", t.c_str());
      std::vector<std::string> ids;
      for (const ::gh::ModInfo& m : mods) {
        std::string info = m.version.empty() ? "" : "v" + m.version;
        if (!m.author.empty()) info += (info.empty() ? "by " : " by ") + m.author;
        const int n = rex_nx_register_mod(m.id.c_str(), m.name.c_str(), m.description.c_str(), info.c_str());
        if (n < 0) { Log("mods: %s is not loaded (more than 16 mods)", m.id.c_str()); continue; }
        if (ids.size() <= size_t(n)) ids.resize(n + 1);  // mod 0 is the built-in 4-player mod
        ids[n] = m.id;
        const size_t sn = m.sprites_dir.empty() ? 0 : sprite_mod_.AddDir(m.sprites_dir, n);
        const size_t tn = m.textures_dir.empty() ? 0 : texture_mod_.AddDir(m.textures_dir, n);
        if (m.name == m.id && !m.legacy) Log("mods: warning: %s has no name in its mod.toml, the id is shown", m.id.c_str());
        Log("mod %d: %s \"%s\" %s by %s: %s, %zu sprite(s), %zu texture(s)", n, m.id.c_str(), m.name.c_str(),
            m.version.empty() ? "-" : m.version.c_str(), m.author.empty() ? "-" : m.author.c_str(),
            rex_nx_mod_on(n) ? "on" : "off", sn, tn);
      }
      for (::gh::SpriteMod* store : {&sprite_mod_, &texture_mod_}) {
        store->SetEnabled([](int mod) { return rex_nx_mod_on(mod) != 0; });
        const auto& cs = store->conflicts();
        for (size_t i = 0; i < cs.size() && i < 20; i++)
          Log("mods: conflict: %s is in %s and in %s; %s wins", cs[i].id.c_str(), ids[cs[i].winner].c_str(),
              ids[cs[i].loser].c_str(), ids[cs[i].winner].c_str());
        if (cs.size() > 20) Log("mods: %zu more conflicts", cs.size() - 20);
      }
      sprite_mod_on_ = sprite_mod_.file_count() > 0;
      texture_mod_on_ = texture_mod_.file_count() > 0;
      Log("mods: %d mod(s) in the menu, %zu sprite file(s), %zu texture file(s) under %smods/", rex_nx_mod_count(), sprite_mod_.file_count(),
          texture_mod_.file_count(), debug_dir.c_str());
    }
    rex_nx_load_settings();  // gh: rex_nx_overlay (version + fps on screen), settings.txt or the overlay_off marker
    {
      // gh: settings.txt "dump_tex=N" (default 0): the raw guest bytes of the first N distinct textures of 256x256 or more.
      if (FILE* fp = std::fopen((debug_dir + "settings.txt").c_str(), "r")) {
        char line[64];
        int v = 0;
        while (std::fgets(line, sizeof(line), fp)) if (std::sscanf(line, "dump_tex=%d", &v) == 1) raw_dump_left_ = v > 0 ? uint32_t(v) : 0;
        std::fclose(fp);
      }
      if (raw_dump_left_) { mkdir((debug_dir + "texdump").c_str(), 0777); Log("dump_tex: the first %u large textures go to %stexdump/", raw_dump_left_, debug_dir.c_str()); }
    }
    present_vs_ = LoadDksh(shader_dir_ + "present.vert.dksh", present_vs_shader_);
    present_fs_ = LoadDksh(shader_dir_ + "present.frag.dksh", present_fs_shader_);
    Log("deko3d backend up: shaders from %s (%s present shaders), %u MB images, %u MB data per frame",
        shader_dir_.c_str(), present_vs_ && present_fs_ ? "with" : "WITHOUT", kImagePoolBytes >> 20, kFrameDataBytes >> 20);
    BeginFrame();
    return true;
  }

  void ShutdownContext() override {
    if (queue_ && !gpu_dead_) dkQueueWaitIdle(queue_);
    Log("deko3d backend down: %u swaps, %u draws, %u skipped, %u without shaders, %u clears, %u resolves, %u images (%u MB), %u uploads (%u KB, %u re-uploads), %llu dirty pages",
        swaps_, draws_, skipped_, missing_draws_, clears_, resolves_, image_count_, images_.peak >> 20, uploads_,
        uint32_t(upload_bytes_ >> 10), reuploads_, (unsigned long long)dirty_pages_);
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address, const uint32_t* host_address,
                     uint32_t dword_count) override {
    uint64_t hash = gh::UcodeHash(host_address, dword_count);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second;
    DkShaderObj* shader = new DkShaderObj(shader_type, hash, host_address, dword_count);
    string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    shaders_.emplace(hash, shader);
    char name[512];
    std::snprintf(name, sizeof(name), "%sshader_%016llX.%s.dksh", shader_dir_.c_str(), (unsigned long long)hash,
                  shader_type == xenos::ShaderType::kVertex ? "vert" : "frag");
    shader->loaded = LoadDksh(name, shader->shader);
    if (shader->loaded && shader_type == xenos::ShaderType::kVertex) {
      std::snprintf(name, sizeof(name), "%sshader_%016llX.geom.dksh", shader_dir_.c_str(), (unsigned long long)hash);
      shader->geom_loaded = LoadDksh(name, shader->geom);
      if (!shader->geom_loaded) LogOnce("geom", "no rectangle-list geometry shader for VS %016llX (%s)", (unsigned long long)hash, name);
    }
    if (!shader->loaded) {
      missing_shaders_++;
      Log("no DKSH for %s %016llX (%u dwords at %08X): dumping the ucode to %smissing/",
          shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", (unsigned long long)hash, dword_count, guest_address,
          shader_dir_.c_str());
      DumpMissing(*shader);
    } else {
      REXLOG_INFO("[dk] shader {} {:016X} loaded ({} dwords)", shader_type == xenos::ShaderType::kVertex ? "VS" : "PS",
                  hash, dword_count);
    }
    return shader;
  }

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    if (gpu_dead_) return true;
    if (skip_frame_) {
      if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) return SkippedCopy();
      perf_skipped_++;
      return true;
    }
    SyncDirtyPages();
    const RegisterFile& regs = *register_file_;
    auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
    if (mode == xenos::EdramMode::kCopy) return Resolve();
    auto* vs = static_cast<DkShaderObj*>(active_vertex_shader());
    auto* ps = static_cast<DkShaderObj*>(active_pixel_shader());
    if (!vs || !ps) { skipped_++; return true; }
    auto mask = regs.Get<reg::RB_COLOR_MASK>();
    auto dc = draw_util::GetNormalizedDepthControl(regs);
    bool writes_color = mask.write_red0 || mask.write_green0 || mask.write_blue0 || mask.write_alpha0;
    bool writes_depth = dc.z_enable && dc.z_write_enable;
    if (!writes_color && !writes_depth && !dc.stencil_enable) { skipped_++; return true; }
    if (IsClearDraw(vs, ps, prim_type)) return ClearDraw(index_count, mode);
    if (!vs->loaded || !ps->loaded) { missing_draws_++; return true; }
    DkPrimitive prim;
    bool rects;
    if (!PrimitiveOf(prim_type, prim, rects)) { LogOnce("prim", "unsupported primitive type %u", uint32_t(prim_type)); skipped_++; return true; }
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    Tick perf_tick{perf_draw_};
    if (draw_diag_) {
      // Each shader pair's first draw, once: the state a new kind of draw comes with.
      char key[48];
      std::snprintf(key, sizeof(key), "pair %016llX %016llX", (unsigned long long)vs->ucode_data_hash(), (unsigned long long)ps->ucode_data_hash());
      auto blend = regs.Get<reg::RB_BLENDCONTROL>();
      if (!logged_.count(key)) {
        LogOnce(key, "draw VS %016llX PS %016llX: prim %u n=%u%s, blend %u/%u op %u, mask %x, z %u/%u func %u, %zu vfetch %zu tfetch",
                (unsigned long long)vs->ucode_data_hash(), (unsigned long long)ps->ucode_data_hash(), uint32_t(prim_type), index_count,
                index_buffer_info ? " indexed" : "", uint32_t(blend.color_srcblend), uint32_t(blend.color_destblend),
                uint32_t(blend.color_comb_fcn), regs.values[XE_GPU_REG_RB_COLOR_MASK] & 0xF, dc.z_enable, dc.z_write_enable,
                uint32_t(dc.zfunc), vs->vertex_bindings().size(), ps->texture_bindings().size());
        // Its vertex layout and the first vertices (byte-swapped words as hex and as floats).
        for (const auto& vb : vs->vertex_bindings()) {
          auto fetch = regs.GetVertexFetch(vb.fetch_constant);
          std::string attrs;
          for (const auto& a : vb.attributes) {
            char s[64];
            std::snprintf(s, sizeof(s), " [fmt %u off %u%s%s]", uint32_t(a.fetch_instr.attributes.data_format), a.fetch_instr.attributes.offset,
                          a.fetch_instr.attributes.is_signed ? " signed" : "", a.fetch_instr.attributes.is_integer ? " integer" : "");
            attrs += s;
          }
          const uint32_t* w = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
          uint32_t n = std::min<uint32_t>(vb.stride_words * 3, 24);
          std::string words;
          for (uint32_t i = 0; i < n && i < fetch.size; i++) {
            uint32_t v = fetch.endian == xenos::Endian::k8in32 ? __builtin_bswap32(w[i]) : w[i];
            float fl;
            std::memcpy(&fl, &v, 4);
            char s[48];
            std::snprintf(s, sizeof(s), " %08X(%g)", v, fl);
            words += s;
          }
          Log("  vf%u stride %u words, %u words at %08X endian %u:%s; data%s", vb.fetch_constant, vb.stride_words, fetch.size, fetch.address << 2,
              uint32_t(fetch.endian), attrs.c_str(), words.c_str());
        }
      }
    }

    // Render targets.
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    uint32_t pitch = EffectivePitch(surf);
    HostImage* color = (mode != xenos::EdramMode::kDepthOnly && writes_color)
                           ? TargetImage(color_info.color_base, pitch, ColorTargetFormatOf(color_info.color_format), false)
                           : nullptr;
    HostImage* depth = (dc.z_enable || dc.stencil_enable) ? TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true) : nullptr;
    if (!color && !depth) { skipped_++; return true; }
    BindTargets(cmd, color, depth);
    uint32_t target_w = color ? color->width : depth->width;
    uint32_t target_h = color ? color->height : depth->height;

    // Viewport and scissor (the window offset included, as the SDK computes them).
    // deko3d (upper-left origin) maps NDC y = +1 to the bottom row like Vulkan; the Xenos clip space is
    // y-up like Direct3D, so the SDK's "origin_bottom_left" negation of the NDC y scale/offset applies
    // (the first Eden picture was upside down without it).
    draw_util::ViewportInfo vi;
    draw_util::GetHostViewportInfo(regs, 1, 1, true, target_w, target_h, false, dc, false, false, false, vi);
    DkViewport vp{float(vi.xy_offset[0]), float(vi.xy_offset[1]), float(vi.xy_extent[0]), float(vi.xy_extent[1]), vi.z_min, vi.z_max};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    draw_util::Scissor sc;
    draw_util::GetScissor(regs, sc, true);
    DkScissor scissor{sc.offset[0], sc.offset[1], sc.extent[0], sc.extent[1]};
    if (scissor.x + scissor.width > target_w) scissor.width = scissor.x < target_w ? target_w - scissor.x : 0;
    if (scissor.y + scissor.height > target_h) scissor.height = scissor.y < target_h ? target_h - scissor.y : 0;
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);

    // Fixed-function state.
    auto su = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    // Rectangle lists: the geometry shader makes the 4th vertex, or the hardware mode (cvar).
    const bool use_gs = rects && vs->geom_loaded && !REXCVAR_GET(deko3d_fill_rectangle);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;  // TODO(cull): cull_front/cull_back once the winding is verified on a picture
    rs.fillRectangleEnable = rects && !use_gs;
    (void)su;
    dkCmdBufBindRasterizerState(cmd, &rs);
    auto blend = regs.Get<reg::RB_BLENDCONTROL>();
    bool blend_enabled = !(blend.color_srcblend == xenos::BlendFactor::kOne && blend.color_destblend == xenos::BlendFactor::kZero &&
                           blend.color_comb_fcn == xenos::BlendOp::kAdd && blend.alpha_srcblend == xenos::BlendFactor::kOne &&
                           blend.alpha_destblend == xenos::BlendFactor::kZero && blend.alpha_comb_fcn == xenos::BlendOp::kAdd);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = blend_enabled ? 1 : 0;
    dkCmdBufBindColorState(cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    cw.masks = (mask.write_red0 ? DkColorMask_R : 0) | (mask.write_green0 ? DkColorMask_G : 0) |
               (mask.write_blue0 ? DkColorMask_B : 0) | (mask.write_alpha0 ? DkColorMask_A : 0);
    dkCmdBufBindColorWriteState(cmd, &cw);
    DkBlendState bs{BlendOp(blend.color_comb_fcn), BlendFactor(blend.color_srcblend), BlendFactor(blend.color_destblend),
                    BlendOp(blend.alpha_comb_fcn), BlendFactor(blend.alpha_srcblend), BlendFactor(blend.alpha_destblend)};
    dkCmdBufBindBlendStates(cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = depth && dc.z_enable;
    ds.depthWriteEnable = depth && dc.z_write_enable;
    ds.depthCompareOp = CompareOp(dc.zfunc);
    ds.stencilTestEnable = 0;
    dkCmdBufBindDepthStencilState(cmd, &ds);

    // Shaders (the whole graphics mask: a stage without a shader here is unbound).
    DkShader const* shaders[3] = {&vs->shader, &ps->shader, &vs->geom};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, use_gs ? 3 : 2);

    // Vertex layout and data: one buffer per fetch constant the vertex shader uses.
    DkVtxAttribState attribs[DK_MAX_VERTEX_ATTRIBS];
    DkVtxBufferState buffers[DK_MAX_VERTEX_BUFFERS];
    uint32_t attrib_count = 0, buffer_count = 0;
    uint32_t vertex_count = index_count;
    uint32_t index_offset = UINT32_MAX;
    if (index_buffer_info) {
      if (!CopyIndices(f, *index_buffer_info, index_offset, vertex_count)) { skipped_++; return true; }
    }
    for (const auto& vb : vs->vertex_bindings()) {
      if (buffer_count >= DK_MAX_VERTEX_BUFFERS) break;
      auto fetch = regs.GetVertexFetch(vb.fetch_constant);
      uint32_t stride = vb.stride_words * 4;
      uint32_t bytes = vertex_count * stride;
      uint32_t avail = fetch.size * 4;
      if (bytes > avail) {
        LogOnce("vbclamp", "vertex data clamped: %u vertices x %u bytes = %u, fetch constant %u holds %u bytes (VS %016llX)", vertex_count,
                stride, bytes, vb.fetch_constant, avail, (unsigned long long)vs->ucode_data_hash());
        bytes = avail;
      }
      uint32_t off = f.data.Alloc(bytes ? bytes : 16, 64);
      if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full"); skipped_++; return true; }
      const uint8_t* src = memory_->TranslatePhysical(fetch.address << 2);
      texture_conversion::CopySwapBlock(fetch.endian, f.data.cpu + off, src, bytes);
      dkCmdBufBindVtxBuffer(cmd, buffer_count, f.data.gpu + off, bytes);
      buffers[buffer_count] = DkVtxBufferState{stride, 0};
      for (const auto& a : vb.attributes) {
        if (attrib_count >= DK_MAX_VERTEX_ATTRIBS) break;
        DkVtxAttribSize size;
        DkVtxAttribType type;
        if (!VertexAttribOf(a.fetch_instr.attributes, size, type)) {
          LogOnce("vfmt", "unsupported vertex format %u", uint32_t(a.fetch_instr.attributes.data_format));
          size = DkVtxAttribSize_4x32;
          type = DkVtxAttribType_Float;
        }
        DkVtxAttribState st{};
        st.bufferId = buffer_count;
        st.isFixed = 0;
        st.offset = uint32_t(a.fetch_instr.attributes.offset) * 4;
        st.size = size;
        st.type = type;
        st.isBgra = 0;
        attribs[attrib_count++] = st;
      }
      buffer_count++;
    }
    dkCmdBufBindVtxAttribState(cmd, attribs, attrib_count);
    dkCmdBufBindVtxBufferState(cmd, buffers, buffer_count);

    // Textures of the pixel shader.
    std::string bound_textures;  // for the draw-type log below
    const uint64_t perf_tex0 = armGetSystemTick();
    // gh: sprite replacement. A replaced 8 bit sprite is drawn with its palette: the palette binding is the 256 wide
    // RGBA texture of the same draw. Both are swapped for the replacement's index image and its own palette.
    struct PendingTex {
      uint32_t fetch_constant = 0;
      xenos::xe_gpu_texture_fetch_t fetch{};
      TextureInfo info;
      HostImage* tex = nullptr;
    };
    PendingTex pending[16];  // no copies of TextureInfo
    size_t pending_count = 0;
    int sprite_slot = -1, clut_slot = -1;
    HostImage* mod_index = nullptr;
    HostImage* mod_clut = nullptr;
    for (const auto& tb : ps->texture_bindings()) {
      if (pending_count >= 16) break;
      PendingTex& pt = pending[pending_count];
      pt.fetch = regs.GetTextureFetch(tb.fetch_constant);
      // 1D textures are 2D textures of height 1 on the Xenos (the shaders sample them so).
      if (!TextureInfo::Prepare(pt.fetch, &pt.info) ||
          (pt.info.dimension != xenos::DataDimension::k2DOrStacked && pt.info.dimension != xenos::DataDimension::k1D)) {
        LogOnce("tex", "texture fetch %u not preparable or not 1D/2D (dim %u)", tb.fetch_constant, uint32_t(pt.fetch.dimension));
        continue;
      }
      pt.tex = TextureImage(f, pt.info, pt.fetch);
      if (!pt.tex) {
        char key[40];
        std::snprintf(key, sizeof(key), "notex %08X", pt.info.memory.base_address);
        LogOnce(key, "texture at %08X (%s %ux%u) has no image: fetch %u keeps its previous binding", pt.info.memory.base_address,
                pt.info.format_info()->name, pt.info.width + 1, pt.info.height + 1, tb.fetch_constant);
        continue;
      }
      pt.fetch_constant = tb.fetch_constant;
      pending_count++;
      // gh: a mod was switched in the menu since this texture's lookup: look again (another mod may have it).
      if (pt.tex->mod_kind && pt.tex->mod_epoch != rex_nx_mod_epoch) {
        pt.tex->mod_epoch = rex_nx_mod_epoch;
        pt.tex->mod = (pt.tex->mod_kind == 1 ? sprite_mod_ : texture_mod_).Find(pt.info.width + 1, pt.info.height + 1, pt.tex->mod_hash);
      }
    }
    if (sprite_mod_on_) {  // a switched-off mod has no tex->mod: the stores' Find skips it
      for (size_t i = 0; i < pending_count; i++) {
        if (pending[i].tex->mod && !pending[i].tex->mod->true_colour && sprite_slot < 0) sprite_slot = int(i);
        else if (pending[i].info.width + 1 == 256 && std::strcmp(pending[i].info.format_info()->name, "k_8_8_8_8") == 0 &&
                 // gh: the hatch shader CE23671D fetches tf0, tf3 (the hatch mask, also 256x1), tf1 (the palette): the first
                 // match was the mask, so the replacement's palette went to tf3 and its indices read the game's palette
                 // (black above entry ~30: black Valgar with speckles). The palette is fetch 1 in both sprite shaders.
                 (clut_slot < 0 || pending[i].fetch_constant == 1)) clut_slot = int(i);
      }
      if (sprite_slot >= 0) {
        if (pending[sprite_slot].tex->mod->game_palette) {
          clut_slot = -1;  // an indexed PNG: the game's own palette texture stays bound (costumes keep working)
          if (!ModImages(f, pending[sprite_slot].tex->mod, mod_index, mod_clut)) sprite_slot = -1;
        } else if (clut_slot < 0) {
          LogOnce("spritemod-noclut", "sprite mod: a replaced sprite is drawn without a 256 wide palette texture; left as it is");
          sprite_slot = -1;
        } else if (!ModImages(f, pending[sprite_slot].tex->mod, mod_index, mod_clut)) {
          sprite_slot = clut_slot = -1;  // not ready (pool or staging full): draw the original this time
        }
      }
    }
    for (size_t pi = 0; pi < pending_count; pi++) {
      const auto& fetch = pending[pi].fetch;
      const auto& info = pending[pi].info;
      HostImage* tex = pending[pi].tex;
      bool identity = false;  // gh: a true-colour replacement is plain RGBA: no fetch swizzle
      if (int(pi) == sprite_slot) tex = mod_index;
      else if (int(pi) == clut_slot && sprite_slot >= 0) tex = mod_clut;
      // gh: hatching off (the menu): the hatch mask (tf3 of the sprite shader) is an all-zero image, so the shader's
      // "mask > 0" branch never runs. No other shader hatches (it is the only one with four textures).
      if (!rex_nx_hatching && pending[pi].fetch_constant == 3 && ps->ucode_data_hash() == 0xCE23671D5EFA0EDBull) {
        if (HostImage* z = NoHatchImage(f)) tex = z;
      }
      else if (tex->mod && tex->mod->true_colour) {
        if (HostImage* r = ModRgbaImage(f, tex->mod)) { tex = r; identity = true; }  // not ready: the original this time
      }
      const uint32_t fc = pending[pi].fetch_constant;
      uint32_t slot = DescriptorFor(f, tex, (identity || tex->from_resolve) ? 0 : fetch.swizzle);
      if (slot == UINT32_MAX) continue;
      uint32_t sampler = (fetch.min_filter == xenos::TextureFilter::kLinear ? 32 : 0) |
                         (fetch.mag_filter == xenos::TextureFilter::kLinear ? 16 : 0) |
                         (WrapCodeOf(fetch.clamp_x) << 2) | WrapCodeOf(fetch.clamp_y);
      if (draw_diag_) {
        // Each texture's first bind, once: what the game draws with (correlates with the uploads).
        char key[32];
        std::snprintf(key, sizeof(key), "tex %08X", info.memory.base_address);
        LogOnce(key, "texture at %08X: %s %ux%u %s endian %u swizzle %03X filters min %u mag %u clamp %u/%u (%u KB)%s",
                info.memory.base_address, info.format_info()->name, info.width + 1, info.height + 1, info.is_tiled ? "tiled" : "linear",
                uint32_t(fetch.endianness), fetch.swizzle, uint32_t(fetch.min_filter), uint32_t(fetch.mag_filter), uint32_t(fetch.clamp_x),
                uint32_t(fetch.clamp_y), info.memory.base_size >> 10, tex->from_resolve ? " (a resolve image)" : "");
      }
      DkResHandle handle = dkMakeTextureHandle(slot, sampler);
      dkCmdBufBindTextures(cmd, DkStage_Fragment, fc, &handle, 1);
      if (draw_diag_) {
        char t[48];
        std::snprintf(t, sizeof(t), " tf%u=%08X(%ux%u)", fc, info.memory.base_address, tex->width, tex->height);
        bound_textures += t;
      }
    }
    perf_tex_ += armGetSystemTick() - perf_tex0;
    if (draw_diag_) {
      // Each shader pair's first draw with each texture set, once (the texture bases name the layer).
      char key[160];
      std::snprintf(key, sizeof(key), "pairtex %016llX %016llX%s", (unsigned long long)vs->ucode_data_hash(),
                    (unsigned long long)ps->ucode_data_hash(), bound_textures.c_str());
      LogOnce(key, "draw VS %016llX PS %016llX prim %u n=%u:%s", (unsigned long long)vs->ucode_data_hash(),
              (unsigned long long)ps->ucode_data_hash(), uint32_t(prim_type), index_count, bound_textures.empty() ? " no textures" : bound_textures.c_str());
    }

    // gh: diagnostics for the black enemy sprites (Valgar, the ninja, the dragon). The sprite shader: idx = tf0; colour =
    // tf1[idx] + c3; where tf3[idx].x > 0 the colour is multiplied by c255.x - tf2(uv * c2 + c1).w (the pencil hatching);
    // the result times c0. One line per character (the tf3 base) and again when its palettes or c0/c3/c255 change (8 per
    // character, 160 in all). Read: c3 (negative = the game darkens), c0, c255.x (1 expected), the mask count, the hatch range.
    if (draw_diag_ && ps->ucode_data_hash() == 0xCE23671D5EFA0EDBull && sprdiag_lines_ < 160) {
      const PendingTex* t[4] = {};
      for (size_t i = 0; i < pending_count; i++) if (pending[i].fetch_constant < 4) t[pending[i].fetch_constant] = &pending[i];
      if (t[0] && t[1] && t[2] && t[3] && t[1]->info.memory.base_size >= 1024 && t[3]->info.memory.base_size >= 1024) {
        float c[5][4];
        const uint32_t ci[5] = {0, 1, 2, 3, 255};
        for (int i = 0; i < 5; i++) for (int k = 0; k < 4; k++) c[i][k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + ci[i] * 4 + k);
        const uint8_t* pal = memory_->TranslatePhysical(t[1]->info.memory.base_address);
        const uint8_t* mask = memory_->TranslatePhysical(t[3]->info.memory.base_address);
        float keyed[12];
        std::memcpy(keyed, c[0], 16); std::memcpy(keyed + 4, c[3], 16); std::memcpy(keyed + 8, c[4], 16);
        const uint64_t state = ::gh::SpriteHash(pal, 1024) ^ (::gh::SpriteHash(mask, 1024) * 3) ^
                               (::gh::SpriteHash(reinterpret_cast<const uint8_t*>(keyed), sizeof(keyed)) * 5);
        auto& seen = sprdiag_[t[3]->info.memory.base_address];
        if (seen.second < 8 && (!seen.second || seen.first != state)) {
          seen.first = state;
          const bool first = seen.second++ == 0;
          sprdiag_lines_++;
          uint32_t nz[4] = {}, lit = 0;
          for (uint32_t i = 0; i < 256; i++) {
            for (int k = 0; k < 4; k++) nz[k] += mask[i * 4 + k] != 0;
            lit += pal[i * 4] != 0;  // guest bytes A, R, G, B
          }
          float q[4] = {};
          if (!vs->vertex_bindings().empty()) {
            const auto& vb = vs->vertex_bindings()[0];
            auto vf = regs.GetVertexFetch(vb.fetch_constant);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(vf.address << 2));
            const uint32_t at[4] = {0, 1, vb.stride_words, vb.stride_words * 2 + 1};  // v0.x, v0.y, v1.x, v2.y
            for (int i = 0; i < 4; i++) if (at[i] < vf.size) {
              uint32_t v = vf.endian == xenos::Endian::k8in32 ? __builtin_bswap32(w[at[i]]) : w[at[i]];
              std::memcpy(&q[i], &v, 4);
            }
          }
          // The index texels that land on a black palette entry (0 = the index data fits the palette: the fault is then
          // in the draw state, not in the textures) and the blend state of this draw.
          uint32_t black_texels = 0, texels = 0;
          {
            const uint8_t* ix = memory_->TranslatePhysical(t[0]->info.memory.base_address);
            for (uint32_t i = 0; i < t[0]->info.memory.base_size; i++) {
              const uint8_t* e = pal + ix[i] * 4;
              texels += ix[i] != 0;
              black_texels += ix[i] != 0 && !(e[1] | e[2] | e[3]);
            }
          }
          // A 16 colour character (the black ones: the mech, the dragon): its raw index bytes, 4 files at most, to compare
          // with the frame in chara.mrg (the same tiled bytes).
          // gh: the trigger was `lit <= 17`, but lit counts alpha (255 for every palette), so 0.96 wrote no file.
          // The black sprites are the ones with a frame side above 320 (mech, dragon); the good ones stay at 320 or less.
          if ((t[0]->info.width >= 320 || t[0]->info.height >= 320) && sprdiag_files_ < 4) {
            sprdiag_files_++;
            mkdir((debug_dir_ + "texdump").c_str(), 0777);
            char path[200];
            std::snprintf(path, sizeof(path), "%stexdump/spr_%08X_%ux%u.bin", debug_dir_.c_str(), t[0]->info.memory.base_address, t[0]->info.width + 1,
                          t[0]->info.height + 1);
            if (FILE* fp = std::fopen(path, "wb")) {
              std::fwrite(memory_->TranslatePhysical(t[0]->info.memory.base_address), 1, t[0]->info.memory.base_size, fp);
              std::fclose(fp);
              Log("sprdiag file %s (%u bytes, the host image is %ux%u%s)", path, uint32_t(t[0]->info.memory.base_size), t[0]->tex->width, t[0]->tex->height,
                  t[0]->tex->from_resolve ? ", a resolve image" : "");
            } else {
              Log("sprdiag file %s: cannot open", path);
            }
          }
          auto bl = regs.Get<reg::RB_BLENDCONTROL>();
          Log("sprdiag state mask %08X: blend colour %u/%u op %u alpha %u/%u op %u, constant %g %g %g %g, write mask %x, alpha test %u; index texels %u, on black "
              "palette entries %u", t[3]->info.memory.base_address, uint32_t(bl.color_srcblend), uint32_t(bl.color_destblend), uint32_t(bl.color_comb_fcn),
              uint32_t(bl.alpha_srcblend), uint32_t(bl.alpha_destblend), uint32_t(bl.alpha_comb_fcn), regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
              regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN), regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
              regs.values[XE_GPU_REG_RB_COLOR_MASK] & 0xF, uint32_t(regs.Get<reg::RB_COLORCONTROL>().alpha_test_enable), texels, black_texels);
          Log("sprdiag #%u mask %08X pal %08X idx %08X %ux%u quad %.0f,%.0f..%.0f,%.0f | c0 %g %g %g %g | c1 %g %g | c2 %g %g | c3 %g %g %g %g | "
              "c255 %g %g %g %g | mask nonzero per byte %u %u %u %u | pal alpha>0 %u | idx filter %u/%u mod %d/%d",
              seen.second, t[3]->info.memory.base_address, t[1]->info.memory.base_address, t[0]->info.memory.base_address, t[0]->info.width + 1,
              t[0]->info.height + 1, q[0], q[1], q[2], q[3], c[0][0], c[0][1], c[0][2], c[0][3], c[1][0], c[1][1], c[2][0], c[2][1], c[3][0],
              c[3][1], c[3][2], c[3][3], c[4][0], c[4][1], c[4][2], c[4][3], nz[0], nz[1], nz[2], nz[3], lit, uint32_t(t[0]->fetch.min_filter),
              uint32_t(t[0]->fetch.mag_filter), sprite_slot, clut_slot);
          if (first) {
            std::string hex[2];
            for (int p = 0; p < 2; p++) for (uint32_t i = 0; i < 1024; i++) {
              char h[4];
              std::snprintf(h, sizeof(h), "%02X", (p ? mask : pal)[i]);
              hex[p] += h;
            }
            Log("sprdiag pal %08X bytes ARGB: %s", t[1]->info.memory.base_address, hex[0].c_str());
            Log("sprdiag mask %08X bytes ARGB: %s", t[3]->info.memory.base_address, hex[1].c_str());
            char key[32];
            std::snprintf(key, sizeof(key), "hatch %08X", t[2]->info.memory.base_address);
            if (!logged_.count(key) && t[2]->info.memory.base_size >= 4096) {
              const uint8_t* h = memory_->TranslatePhysical(t[2]->info.memory.base_address);
              const uint32_t n = std::min<uint32_t>(t[2]->info.memory.base_size, 16384) / 4;
              uint32_t lo[4] = {255, 255, 255, 255}, hi[4] = {}, sum[4] = {};
              for (uint32_t i = 0; i < n; i++) for (int k = 0; k < 4; k++) {
                const uint32_t v = h[i * 4 + k];
                lo[k] = std::min(lo[k], v); hi[k] = std::max(hi[k], v); sum[k] += v;
              }
              LogOnce(key, "sprdiag hatch %08X %ux%u swizzle %03X clamp %u/%u: byte lanes A R G B min/max/mean %u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u",
                      t[2]->info.memory.base_address, t[2]->info.width + 1, t[2]->info.height + 1, t[2]->fetch.swizzle, uint32_t(t[2]->fetch.clamp_x),
                      uint32_t(t[2]->fetch.clamp_y), lo[0], hi[0], sum[0] / n, lo[1], hi[1], sum[1] / n, lo[2], hi[2], sum[2] / n, lo[3], hi[3], sum[3] / n);
            }
          }
        }
      }
    }

    // Uniforms: the ALU constants and the system constants, per stage.
    if (!BindUniforms(f, vs, ps, vi, regs)) { skipped_++; return true; }

    if (index_buffer_info) {
      dkCmdBufBindIdxBuffer(cmd, index_buffer_info->format == xenos::IndexFormat::kInt32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                            f.data.gpu + index_offset);
      dkCmdBufDrawIndexed(cmd, prim, index_count, 1, 0, 0, 0);
    } else {
      dkCmdBufDraw(cmd, prim, index_count, 1, 0, 0);
    }
    draws_++;
    frame_draws_++;
    return true;
  }

  bool IssueCopy() override {
    if (gpu_dead_) return true;
    if (skip_frame_) return SkippedCopy();
    SyncDirtyPages();
    return Resolve();
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    Tick perf_tick{perf_swap_};
    if (gpu_dead_) {  // gh: see BeginFrame: no acquire (it aborts the process on a dead queue), no submit
      swaps_++;
      PerfLine();
      return;
    }
    // gh: a skipped frame shows the front buffer of the last drawn frame (the game may swap between two buffers; the
    // one of this frame was not filled).
    if (skip_frame_ && last_front_[0]) { frontbuffer_ptr = last_front_[0]; frontbuffer_width = last_front_[1]; frontbuffer_height = last_front_[2]; }
    else { last_front_[0] = frontbuffer_ptr; last_front_[1] = frontbuffer_width; last_front_[2] = frontbuffer_height; }
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    HostImage* front = nullptr;
    if (guest_images_.count(frontbuffer_ptr) && frontbuffer_width && frontbuffer_height) {
      // The resolve created it with the aligned height: the first swap trims it to the front buffer's size.
      front = GuestImage(f, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, DkImageFormat_RGBA8_Unorm, false);
    }
    if (present_target_) {
      // Debug (the file sdmc:/switch/gh/dk_present_rt): show the widest colour render target itself,
      // bypassing the resolve copies and the front buffer image.
      HostImage* widest = nullptr;
      for (auto& it : targets_) if (!it.second->is_depth && (!widest || it.second->width > widest->width)) widest = it.second;
      if (widest) front = widest;
    }
    // Debug readback at two swaps: what the images hold (uniform colour = a broken stage).
    std::vector<std::pair<std::string, uint32_t>> dumps;
    if (dump_images_ && (swaps_ + 1 == 120 || swaps_ + 1 == 600 || swaps_ + 1 == 2400)) {
      auto queue_dump = [&](const char* what, HostImage* img) {
        if (!img) return;
        uint32_t bytes = img->width * img->height * 4;
        uint32_t off = f.staging.Alloc(bytes, 0x1000);
        if (off == UINT32_MAX) return;
        DkImageView v;
        dkImageViewDefaults(&v, &img->image);
        DkImageRect r{0, 0, 0, img->width, img->height, 1};
        DkCopyBuf cb{f.staging.gpu + off, 0, 0};
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufCopyImageToBuffer(cmd, &v, &r, &cb, 0);
        char name[96];
        std::snprintf(name, sizeof(name), "%s %ux%u", what, img->width, img->height);
        dumps.emplace_back(name, off);
      };
      queue_dump("front", front);
      for (auto& it : targets_) if (!it.second->is_depth) { char n[48]; std::snprintf(n, sizeof(n), "target base %u pitch %u", it.second->edram_base, it.second->edram_pitch); queue_dump(n, it.second); }
      for (auto& it : guest_images_) if (it.second->width >= 600 && it.second != front) { char n[48]; std::snprintf(n, sizeof(n), "guest %08X%s", it.first, it.second->from_resolve ? " (resolve)" : " (upload)"); queue_dump(n, it.second); }
      dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    }
    // gh overlay: the version and the fps, uploaded into this frame's image before the present pass.
    HostImage* overlay = nullptr;
    // The fps number does not depend on the overlay flag: the strip is drawn when either is on.
    if (rex_nx_overlay || rex_nx_show_fps) {
      const uint64_t now = armGetSystemTick();
      if (!fps_tick_) fps_tick_ = now;
      fps_frames_++;
      if (now - fps_tick_ >= armGetSystemTickFreq()) {
        fps_x10_ = uint32_t(uint64_t(fps_frames_) * 10 * armGetSystemTickFreq() / (now - fps_tick_));
        fps_tick_ = now;
        fps_frames_ = 0;
      }
      if (!overlay_[frame_]) overlay_[frame_] = CreateImage(kOverlayW, kOverlayH, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
      overlay = overlay_[frame_];
      if (overlay) {
        char text[64];
        if (rex_nx_overlay && rex_nx_show_fps) std::snprintf(text, sizeof(text), "GH %s  %u.%u fps", rex_nx_version(), fps_x10_ / 10, fps_x10_ % 10);
        else if (rex_nx_overlay) std::snprintf(text, sizeof(text), "GH %s", rex_nx_version());
        else std::snprintf(text, sizeof(text), "%u.%u fps", fps_x10_ / 10, fps_x10_ % 10);
        if (std::strcmp(text, overlay_text_) != 0) {
          RenderOverlayText(overlay_px_.data(), text);
          std::memcpy(overlay_text_, text, sizeof(overlay_text_));
        }
        const uint32_t bytes = kOverlayW * kOverlayH * 4;
        const uint32_t off = f.staging.Alloc(bytes, 0x100);
        if (off != UINT32_MAX) {
          std::memcpy(f.staging.cpu + off, overlay_px_.data(), bytes);
          DkCopyBuf cb{f.staging.gpu + off, 0, 0};
          DkImageView ov;
          dkImageViewDefaults(&ov, &overlay->image);
          DkImageRect rect{0, 0, 0, kOverlayW, kOverlayH, 1};
          dkCmdBufCopyBufferToImage(cmd, &cb, &ov, &rect, 0);
          dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        } else {
          overlay = nullptr;
        }
      }
    }
    // gh: the settings menu panel, rendered and uploaded only while it is open.
    HostImage* menu = nullptr;
    if (rex_nx_menu_open) {
      if (!menu_img_[frame_]) menu_img_[frame_] = CreateImage(kMenuW, kMenuH, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
      menu = menu_img_[frame_];
      if (menu) {
        for (auto& px : menu_px_) px = 0xFF000000u;
        for (uint32_t x = 0; x < kMenuW; x++) menu_px_[x] = menu_px_[(kMenuH - 1) * kMenuW + x] = 0xFFFFFFFFu;
        for (uint32_t y = 0; y < kMenuH; y++) menu_px_[y * kMenuW] = menu_px_[y * kMenuW + kMenuW - 1] = 0xFFFFFFFFu;
        const int cur = rex_nx_menu_cursor;
        char line[80];
        int first = 0;  // the first list line on screen (the mods page scrolls)
        const int rows = rex_nx_menu_page == 0 ? 11 : 9;
        auto row = [&](int i, const char* name, const char* val) {
          if (i < first || i >= first + rows) return;
          std::snprintf(line, sizeof(line), "%s%s%s%s", cur == i ? "> " : "  ", name, val ? ": " : "", val ? val : "");
          DrawText(menu_px_.data(), kMenuW, 12, 26 + 20 * (i - first), line);
        };
        auto onoff = [](int v) { return v ? "on" : "off"; };
        if (rex_nx_menu_page == 0) {
          DrawText(menu_px_.data(), kMenuW, 12, 6, "settings");
          row(0, "resume", nullptr);
          row(1, "fps counter", onoff(rex_nx_show_fps));
          row(2, "sound", onoff(rex_nx_sound));
          row(3, "overlay", onoff(rex_nx_overlay));
          row(4, "hatching", onoff(rex_nx_hatching));
          row(5, "frame skip", rex_nx_frame_skip ? "30 fps" : "off");
          row(6, "controls", rex_nx_controls_alt ? "alternative" : "standard");
          row(7, "start level", rex_nx_level_name(rex_nx_start_level));
          row(8, "go to level", nullptr);
          row(9, "cheats", nullptr);
          row(10, "mods", nullptr);
        } else if (rex_nx_menu_page == 1) {
          DrawText(menu_px_.data(), kMenuW, 12, 6, "cheats");
          row(0, "invincible", onoff(rex_nx_cheat_invincible));
          row(1, "magic", onoff(rex_nx_cheat_mp));
          row(2, "continues", onoff(rex_nx_cheat_continues));
          row(3, "attributes", onoff(rex_nx_cheat_attributes));
          row(4, "back", nullptr);
        } else {
          // The mods page: one line per mod (the list scrolls), then the description of the mod under the cursor.
          const int mods = rex_nx_mod_count(), total = mods + 1;
          first = std::max(0, std::min(cur - 4, total - 9));
          DrawText(menu_px_.data(), kMenuW, 12, 6, mods ? (first + 9 < total ? "mods (more below)" : "mods") : "mods: none found");
          const size_t name_cols = kMenuCols - 2 - 5;  // "> " before, ": off" after
          for (int i = 0; i < mods; i++) {
            std::string name = MenuPlain(rex_nx_mod_name(i));
            if (name.size() > name_cols) {
              if (i == cur) {
                // The line under the cursor scrolls: a step every 0.25 s, with a pause at both ends.
                const size_t over = name.size() - name_cols, step = size_t(armTicksToNs(armGetSystemTick()) / 250000000ull) % (over + 8);
                name = name.substr(step < 4 ? 0 : std::min(step - 4, over), name_cols);
              } else {
                name = name.substr(0, name_cols - 2) + "..";
              }
            }
            row(i, name.c_str(), onoff(rex_nx_mod_on(i)));
          }
          row(mods, "back", nullptr);
          for (uint32_t x = 8; x < kMenuW - 8; x++) menu_px_[210 * kMenuW + x] = 0xFFFFFFFFu;
          if (cur < mods) {
            const std::vector<std::string> text = MenuWrap(MenuPlain(rex_nx_mod_description(cur)), kMenuCols - 1, 2);
            for (size_t i = 0; i < text.size(); i++) DrawText(menu_px_.data(), kMenuW, 12, 216 + 18 * uint32_t(i), text[i].c_str());
            const std::vector<std::string> info = MenuWrap(MenuPlain(rex_nx_mod_info(cur)), kMenuCols - 1, 1);
            if (!info.empty()) DrawText(menu_px_.data(), kMenuW, 12, 252, info[0].c_str());
          }
        }
        DrawText(menu_px_.data(), kMenuW, 12, 274, "a ok   b back");
        const uint32_t bytes = kMenuW * kMenuH * 4;
        const uint32_t off = f.staging.Alloc(bytes, 0x100);
        if (off != UINT32_MAX) {
          std::memcpy(f.staging.cpu + off, menu_px_.data(), bytes);
          DkCopyBuf cb{f.staging.gpu + off, 0, 0};
          DkImageView mv;
          dkImageViewDefaults(&mv, &menu->image);
          DkImageRect rect{0, 0, 0, kMenuW, kMenuH, 1};
          dkCmdBufCopyBufferToImage(cmd, &cb, &mv, &rect, 0);
          dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        } else {
          menu = nullptr;
        }
      }
    }
    if (swaps_ < 5) Log("swap #%u: acquiring a swapchain image", swaps_ + 1);
    const uint64_t perf_acq0 = armGetSystemTick();
    int slot = dkQueueAcquireImage(queue_, swapchain_);
    perf_acquire_ += armGetSystemTick() - perf_acq0;
    if (swaps_ < 5) Log("swap #%u: slot %d", swaps_ + 1, slot);
    HostImage* target = swap_images_[slot];
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    DkImageView tv;
    dkImageViewDefaults(&tv, &target->image);
    DkImageView const* targets[1] = {&tv};
    dkCmdBufBindRenderTargets(cmd, targets, 1, nullptr);
    DkViewport vp{0, 0, float(kPresentWidth), float(kPresentHeight), 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    DkScissor sc{0, 0, kPresentWidth, kPresentHeight};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    const float navy[4] = {0.0f, 0.0f, 0.25f, 1.0f};  // dark blue: visible when the present draw does not run
    dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, navy);
    if (front && present_vs_ && present_fs_) {
      DkRasterizerState rs;
      dkRasterizerStateDefaults(&rs);
      rs.cullMode = DkFace_None;
      dkCmdBufBindRasterizerState(cmd, &rs);
      DkColorState cs;
      dkColorStateDefaults(&cs);
      dkCmdBufBindColorState(cmd, &cs);
      DkColorWriteState cw;
      dkColorWriteStateDefaults(&cw);
      dkCmdBufBindColorWriteState(cmd, &cw);
      DkDepthStencilState ds;
      dkDepthStencilStateDefaults(&ds);
      ds.depthTestEnable = 0;
      ds.depthWriteEnable = 0;
      dkCmdBufBindDepthStencilState(cmd, &ds);
      DkShader const* shaders[2] = {&present_vs_shader_, &present_fs_shader_};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
      dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
      dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
      uint32_t dslot = DescriptorFor(f, front, 0);
      if (dslot != UINT32_MAX) {
        DkResHandle handle = dkMakeTextureHandle(dslot, 16);  // linear, clamp
        dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &handle, 1);
        dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
      }
      if (overlay) {
        const uint32_t oslot = DescriptorFor(f, overlay, 0);
        if (oslot != UINT32_MAX) {
          const uint32_t ox = 8, oy = kPresentHeight - kOverlayH - 8;
          DkViewport ovp{float(ox), float(oy), float(kOverlayW), float(kOverlayH), 0.0f, 1.0f};
          dkCmdBufSetViewports(cmd, 0, &ovp, 1);
          DkScissor osc{ox, oy, kOverlayW, kOverlayH};
          dkCmdBufSetScissors(cmd, 0, &osc, 1);
          DkResHandle oh = dkMakeTextureHandle(oslot, 0);  // nearest, clamp
          dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &oh, 1);
          dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        }
      }
      if (menu) {
        const uint32_t mslot = DescriptorFor(f, menu, 0);
        if (mslot != UINT32_MAX) {
          const uint32_t mx = (kPresentWidth - kMenuW) / 2, my = (kPresentHeight - kMenuH) / 2;
          DkViewport mvp{float(mx), float(my), float(kMenuW), float(kMenuH), 0.0f, 1.0f};
          dkCmdBufSetViewports(cmd, 0, &mvp, 1);
          DkScissor msc{mx, my, kMenuW, kMenuH};
          dkCmdBufSetScissors(cmd, 0, &msc, 1);
          DkResHandle mh = dkMakeTextureHandle(mslot, 0);
          dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &mh, 1);
          dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        }
      }
    }
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    DkCmdList list = dkCmdBufFinishList(cmd);
    std::snprintf(frame_note_[frame_], sizeof(frame_note_[0]), "swap %u: %u draws, %u clears, %u resolves, %u re-uploads, %u resize copies, "
                  "staging %u KB, data %u KB, menu %d, skip %d", swaps_ + 1, frame_draws_, frame_clears_, frame_resolves_, frame_reuploads_,
                  frame_resize_copies_, f.staging.used >> 10, f.data.used >> 10, int(rex_nx_menu_open), int(skip_frame_));
    dkQueueSubmitCommands(queue_, list);
    dkQueueSignalFence(queue_, &f.fence, false);
    f.fenced = true;
    dkQueuePresentImage(queue_, swapchain_, slot);
    if (!dumps.empty()) {
      dkQueueWaitIdle(queue_);
      for (auto& d : dumps) {
        // The image's dims are in the name; recover them from the staging layout: sample a 16x16 grid.
        uint32_t w = 0, h = 0;
        std::sscanf(d.first.c_str() + d.first.rfind(' ') + 1, "%ux%u", &w, &h);
        const uint32_t* px = reinterpret_cast<const uint32_t*>(f.staging.cpu + d.second);
        std::unordered_set<uint32_t> distinct;
        for (uint32_t y = 0; y < 16 && h; y++)
          for (uint32_t x = 0; x < 16 && w; x++) distinct.insert(px[(y * h / 16) * w + x * w / 16]);
        {  // gh: the image as a PPM next to the NRO (view it on the PC)
          static unsigned dump_index = 0;
          char path[96];
          std::snprintf(path, sizeof(path), "%sdump_%03u_s%u.ppm", debug_dir_.c_str(), dump_index++, swaps_ + 1);
          if (FILE* fp = std::fopen(path, "wb")) {
            std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
            std::vector<uint8_t> row(size_t(w) * 3);
            for (uint32_t y = 0; y < h; y++) {
              for (uint32_t x = 0; x < w; x++) {
                const uint8_t* q = reinterpret_cast<const uint8_t*>(px + size_t(y) * w + x);
                row[x * 3] = q[0]; row[x * 3 + 1] = q[1]; row[x * 3 + 2] = q[2];
              }
              std::fwrite(row.data(), 1, row.size(), fp);
            }
            std::fclose(fp);
            Log("dump written: %s (%s)", path, d.first.c_str());
          }
        }
        Log("dump at swap #%u: %s: %zu distinct colours in a 16x16 grid; pixels %08X %08X %08X %08X %08X", swaps_ + 1,
            d.first.c_str(), distinct.size(), w && h ? px[0] : 0, w && h ? px[(h / 4) * w + w / 4] : 0,
            w && h ? px[(h / 2) * w + w / 2] : 0, w && h ? px[(3 * h / 4) * w + 3 * w / 4] : 0, w && h ? px[(h - 1) * w + w - 1] : 0);
      }
    }
    swaps_++;
    if (swaps_ <= 3 || swaps_ % 60 == 0) {
      std::printf("[nx] gpu: swap #%u\n", swaps_);  // the Eden gate's frame counter
      Log("swap #%u: front %08X %ux%u %s; frame: %u draws, %u clears, %u resolves, %u skipped, %u no-shader, %u re-uploads, %llu dirty pages; data %u KB staging %u KB; images %u (%u MB)",
          swaps_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, front ? "shown" : "MISSING", frame_draws_,
          frame_clears_, frame_resolves_, frame_skipped_, frame_missing_, frame_reuploads_, (unsigned long long)(dirty_pages_ - dirty_pages_at_swap_),
          f.data.used >> 10, f.staging.used >> 10, image_count_, images_.used >> 20);
    }
    PerfLine();
    // gh: frame skip (the menu, fps_cap=30). The game runs its 60 frames a second; every second one is not drawn: its
    // draws, clears and copies are dropped whole and this swap shows the last image again, on time. The host images
    // (render targets, resolve destinations) keep the last drawn frame, the next frame draws everything again, and a
    // texture the game changed meanwhile is uploaded at its next bind (the dirty pages stay marked). The guest reads
    // nothing back from the renderer, so it cannot see the difference.
    // ponytail: parity only. A picture the game renders ONCE into a texture on a skipped frame (a pause background)
    // is lost; SkippedCopy counts copies to a destination that has no image ("new" in the perf line). If that shows,
    // draw such a frame late (keep its packets) or never skip the frame after a change of the copy count.
    skip_frame_ = rex_nx_frame_skip && (swaps_ & 1);
    dirty_pages_at_swap_ = dirty_pages_;
    frame_draws_ = frame_clears_ = frame_resolves_ = frame_skipped_ = frame_missing_ = frame_reuploads_ = frame_resize_copies_ = 0;
    frame_ = (frame_ + 1) % kFrames;
    BeginFrame();
  }

 private:
  bool SkippedCopy() {
    perf_skipped_++;
    draw_util::ResolveInfo info;  // the same lookup as Resolve(): its destination is the image key
    if (draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false, info) && !info.IsCopyingDepth() &&
        !guest_images_.count(info.copy_dest_base)) perf_skip_new_++;
    return true;
  }

  // gh: one line a second: where the command processor's time goes, per frame. "draw" is IssueDraw without clears and
  // copies ("tex" inside it: texture checks, hashes, uploads, binds), "copy" the resolves, "swap" the whole IssueSwap
  // ("acquire" and "fence" inside it wait for the display and for the GPU). "gpu wait" is the time in WAIT_REG_MEM: the
  // command processor waits for a value the guest (or its interrupt callback) writes. "guest poll" is the share of the
  // second the game's thread spent in its own GPU poll (sub_825D6A68): it waits for the command processor there.
  void PerfLine() {
    const uint64_t now = armGetSystemTick();
    perf_frames_++;
    if (!perf_t0_) perf_t0_ = now;
    if (now - perf_t0_ < armGetSystemTickFreq()) return;
    const double n = perf_frames_, ms = 1e-6 / n;
    const double sec = double(armTicksToNs(now - perf_t0_)) * 1e-9;
    uint64_t kind[4];
    double wait = 0;
    for (int i = 0; i < 4; i++) {
      const uint64_t v = rex_nx_wait_kind_ns[i];
      kind[i] = v - perf_kind_[i];
      perf_kind_[i] = v;
      wait += double(kind[i]);
    }
    const uint64_t poll = rex_nx_guest_poll_ticks, hooks = rex_nx_hook_calls;
    std::printf("[dk] perf: %.1f fps | frame ms: draw %.2f (tex %.2f, %u draws), clear %.2f (%u), copy %.2f (%u), swap %.2f "
                "(acquire %.2f, fence %.2f) | uploads %u, %u KB | gpu wait %.2f ms: reg %.2f, 1002 %.2f, 1006 %.2f, other %.2f"
                " | guest poll %.0f%% | hooks %llu/s | skip %s: %u packets/s, %u new, upload %.2f (detile %.2f) ms\n",
                n / sec, double(armTicksToNs(perf_draw_)) * ms, double(armTicksToNs(perf_tex_)) * ms, uint32_t((draws_ - perf_draws_) / n),
                double(armTicksToNs(perf_clear_)) * ms, uint32_t((clears_ - perf_clears_) / n), double(armTicksToNs(perf_copy_)) * ms,
                uint32_t((resolves_ - perf_resolves_) / n), double(armTicksToNs(perf_swap_)) * ms, double(armTicksToNs(perf_acquire_)) * ms,
                double(armTicksToNs(perf_fence_)) * ms, uploads_ - perf_uploads_, uint32_t((upload_bytes_ - perf_upload_bytes_) >> 10),
                wait * ms, double(kind[0]) * ms, double(kind[1]) * ms, double(kind[2]) * ms, double(kind[3]) * ms,
                double(armTicksToNs(poll - perf_poll_)) * 1e-7 / sec, (unsigned long long)(double(hooks - perf_hooks_) / sec),
                rex_nx_frame_skip ? "on" : "off", uint32_t(perf_skipped_ / sec), perf_skip_new_, double(armTicksToNs(perf_upload_)) * ms,
                double(armTicksToNs(perf_detile_)) * ms);
    perf_skipped_ = perf_skip_new_ = 0;
    perf_upload_ = perf_detile_ = 0;
    perf_draw_ = perf_tex_ = perf_clear_ = perf_copy_ = perf_swap_ = perf_acquire_ = perf_fence_ = 0;
    perf_draws_ = draws_; perf_clears_ = clears_; perf_resolves_ = resolves_; perf_uploads_ = uploads_;
    perf_upload_bytes_ = upload_bytes_; perf_poll_ = poll; perf_hooks_ = hooks;
    perf_frames_ = 0;
    perf_t0_ = now;
  }

  // ----- frames ---------------------------------------------------------------------------------
  void BeginFrame() {
    if (gpu_dead_) return;
    Frame& f = frames_[frame_];
    if (f.fenced) {
      if (swaps_ < 6) Log("frame %u: waiting for its fence", frame_);
      const uint64_t perf_f0 = armGetSystemTick();
      DkResult r = dkFenceWait(&f.fence, 2000000000ll);  // 2 s: a hang is logged, not silent
      perf_fence_ += armGetSystemTick() - perf_f0;
      if (r != DkResult_Success) {
        Log("frame %u: fence wait result %d (GPU hang?), swap %u, %u images, %zu free, %zu pending", frame_, int(r), swaps_, image_count_,
            free_images_.size(), pending_free_[0].size() + pending_free_[1].size());
        DumpImageEvents();
        // gh: the frame that waits is this slot's last one; the other slot's ran before or after it.
        for (uint32_t i = 0; i < kFrames; i++) Log("frame %u was %s", i, frame_note_[i]);
        // A hung channel never comes back, and the next dkQueueAcquireImage aborts the process (0.115: crash report
        // 2359-0001 from dk::detail::RaiseError). Stop all GPU work: the game and the sound run on, the picture stays.
        // The second timeout in a row decides too, so one slow frame (sleep, HOME) does not end the picture.
        const bool queue_error = dkQueueIsInErrorState(queue_);
        if (queue_error || ++fence_timeouts_ >= 2) {
          gpu_dead_ = true;
          Log("GPU is hung (queue error state %d): drawing stopped, the picture stays frozen. Close the game from HOME.", int(queue_error));
          std::fflush(stdout);
          return;
        }
        std::fflush(stdout);
      } else {
        fence_timeouts_ = 0;
      }
      free_images_.insert(free_images_.end(), pending_free_[frame_].begin(), pending_free_[frame_].end());
      pending_free_[frame_].clear();
      if (swaps_ < 6) Log("frame %u: fence done", frame_);
      f.fenced = false;
    }
    f.data.Reset();
    f.staging.Reset();
    f.next_descriptor = 0;
    f.descriptors.clear();
    dkCmdBufClear(f.cmdbuf);
    dkCmdBufAddMemory(f.cmdbuf, cmdmem_.block, f.cmd_offset, kFrameCmdBytes);
    dkCmdBufBindImageDescriptorSet(f.cmdbuf, descriptors_.gpu + f.desc_offset, kMaxImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(f.cmdbuf, descriptors_.gpu + sampler_desc_offset_, kSamplerCount);
    bound_color_ = nullptr;
    bound_depth_ = nullptr;
  }

  // ----- dirty pages ----------------------------------------------------------------------------
  // Consumes the dirty-page bytes before every draw and stamps each written page with the draw
  // serial; a texture whose pages were written after the upload or resolve that filled its image
  // is uploaded again at its next bind (TextureImage). The coarse map makes a clean frame cost 512
  // byte reads. ponytail: the guest marks a page after the store without a barrier, so a texture
  // written in the same microsecond as its bind can be uploaded stale once (the next write fixes it).
  void SyncDirtyPages() {
    serial_++;
    const uint64_t* words = reinterpret_cast<const uint64_t*>(const_cast<const uint8_t*>(rex_nx_dirty));
    for (uint32_t c = 0; c < 0x200; c++) {
      if (!rex_nx_dirty_coarse[c]) continue;
      rex_nx_dirty_coarse[c] = 0;
      for (uint32_t w = c * 32; w < c * 32 + 32; w++) {
        if (!words[w]) continue;
        for (uint32_t i = w * 8; i < w * 8 + 8; i++) {
          if (!rex_nx_dirty[i]) continue;
          rex_nx_dirty[i] = 0;
          page_serial_[i] = serial_;
          dirty_pages_++;
        }
      }
    }
  }

  bool WrittenSince(uint32_t physical, uint32_t bytes, uint32_t serial) const {
    uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
    uint32_t last = ((physical & 0x1FFFFFFFu) + (bytes ? bytes - 1 : 0)) >> 12;
    for (uint32_t p = first; p <= last && p < page_serial_.size(); p++) {
      if (page_serial_[p] > serial) return true;
    }
    return false;
  }

  // ----- images ---------------------------------------------------------------------------------
  // gh: images replaced by a resize (or a format change) wait here for a request of the same shape: the
  // pool is a bump allocator and a per-frame resize ping-pong filled 448 MB in minutes (finding 0.22).
  // The old image may still be sampled or copied by a command buffer that has not run yet (this frame or the
  // other one in flight): it is reusable only after this frame slot's fence (BeginFrame). 0.85 froze in level 3
  // right after a resize 576x192 -> 64x160 (gh_log line 15817); reusing at once was the one unfenced hazard.
  std::vector<HostImage*> free_images_;
  std::vector<HostImage*> pending_free_[kFrames];
  void RecycleImage(HostImage* img) {
    if (img && !img->is_depth) pending_free_[frame_].push_back(img);
  }

  // The last image events, written to the log when a fence wait times out.
  struct ImageEvent { char text[96]; };
  ImageEvent events_[32];
  uint32_t event_count_ = 0;
  void NoteImage(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(events_[event_count_++ % 32].text, sizeof(ImageEvent::text), fmt, ap);
    va_end(ap);
  }
  void DumpImageEvents() {
    const uint32_t n = event_count_ < 32 ? event_count_ : 32;
    for (uint32_t i = 0; i < n; i++) Log("last image event -%u: %s", n - i, events_[(event_count_ - n + i) % 32].text);
  }

  HostImage* CreateImage(uint32_t width, uint32_t height, DkImageFormat format, uint32_t flags, bool is_depth) {
    for (size_t i = 0; i < free_images_.size(); i++) {
      HostImage* r = free_images_[i];
      if (r->width == width && r->height == height && r->format == format && r->flags == flags && r->is_depth == is_depth) {
        free_images_.erase(free_images_.begin() + i);
        r->from_resolve = false;
        r->content_serial = 0;
        r->checked_serial = 0;
        r->guest_base = 0;
        r->edram_base = r->edram_pitch = 0;
        r->mod = nullptr;
        return r;
      }
    }
    auto* img = new HostImage();
    img->flags = flags;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = flags;
    lm.format = format;
    lm.dimensions[0] = width;
    lm.dimensions[1] = height;
    lm.dimensions[2] = 1;
    dkImageLayoutInitialize(&img->layout, &lm);
    uint64_t size = dkImageLayoutGetSize(&img->layout);
    uint32_t align = dkImageLayoutGetAlignment(&img->layout);
    uint32_t off = images_.Alloc(uint32_t(size), align);
    if (off == UINT32_MAX) {
      // gh: pool full: take the smallest recycled slot that fits (a recycled image is idle; its shape differs).
      size_t best = free_images_.size();
      for (size_t i = 0; i < free_images_.size(); i++) {
        HostImage* r = free_images_[i];
        if (r->pool_size >= size && r->pool_align % align == 0 &&
            (best == free_images_.size() || r->pool_size < free_images_[best]->pool_size)) best = i;
      }
      if (best != free_images_.size()) {
        HostImage* r = free_images_[best];
        free_images_.erase(free_images_.begin() + best);
        r->layout = img->layout;
        dkImageInitialize(&r->image, &r->layout, images_.block, r->pool_off);
        // The descriptor cache keys on the image pointer: drop this frame's entries for the old shape.
        for (auto it = frames_[frame_].descriptors.begin(); it != frames_[frame_].descriptors.end();) {
          if ((it->first & 0xFFFFFFFFFFFFull) == uint64_t(reinterpret_cast<uintptr_t>(r))) it = frames_[frame_].descriptors.erase(it);
          else ++it;
        }
        r->flags = flags; r->format = format; r->width = width; r->height = height; r->is_depth = is_depth;
        r->from_resolve = false; r->content_serial = r->checked_serial = 0; r->guest_base = 0;
        r->edram_base = r->edram_pitch = 0; r->mod = nullptr;
        delete img;
        Log("image pool full: %ux%u placed in a recycled slot", width, height);
        return r;
      }
      Log("image pool full (%u MB used): %ux%u not created", images_.used >> 20, width, height);
      delete img;
      return nullptr;
    }
    dkImageInitialize(&img->image, &img->layout, images_.block, off);
    img->pool_off = off; img->pool_size = uint32_t(size); img->pool_align = align;
    img->format = format;
    img->width = width;
    img->height = height;
    img->is_depth = is_depth;
    image_count_++;
    return img;
  }

  static uint32_t EffectivePitch(reg::RB_SURFACE_INFO surf) {
    // The XDK clears through a 4x MSAA alias at half the pitch (2x2 samples per pixel: the same
    // EDRAM bytes as the 1x surface, both axes halved).
    return surf.msaa_samples == xenos::MsaaSamples::k4X ? surf.surface_pitch * 2 : surf.surface_pitch;
  }

  HostImage* TargetImage(uint32_t edram_base, uint32_t pitch, DkImageFormat format, bool is_depth) {
    if (!pitch) return nullptr;
    uint64_t key = (uint64_t(edram_base) << 32) | (uint64_t(pitch) << 1) | (is_depth ? 1 : 0);
    auto it = targets_.find(key);
    if (it != targets_.end()) return it->second;
    uint32_t tiles = edram_base < kEdramTiles ? kEdramTiles - edram_base : 0;
    uint32_t rows = tiles ? uint32_t((uint64_t(tiles) * 80 * 16) / pitch) : 0;
    if (rows > kMaxTargetHeight) rows = kMaxTargetHeight;
    if (rows < 16) rows = 16;
    HostImage* img = CreateImage(pitch, rows, format, DkImageFlags_UsageRender, is_depth);
    if (!img) return nullptr;
    img->edram_base = edram_base;
    img->edram_pitch = pitch;
    targets_.emplace(key, img);
    Log("%s target: EDRAM base %u pitch %u -> %ux%u image (%u MB in images)", is_depth ? "depth" : "color", edram_base, pitch,
        img->width, img->height, images_.used >> 20);
    return img;
  }

  void BindTargets(DkCmdBuf cmd, HostImage* color, HostImage* depth) {
    if (color == bound_color_ && depth == bound_depth_) return;
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
    DkImageView cv, dv;
    DkImageView const* colors[1] = {&cv};
    if (color) dkImageViewDefaults(&cv, &color->image);
    if (depth) dkImageViewDefaults(&dv, &depth->image);
    dkCmdBufBindRenderTargets(cmd, color ? colors : nullptr, color ? 1 : 0, depth ? &dv : nullptr);
    bound_color_ = color;
    bound_depth_ = depth;
  }

  // The image at a guest address (resolve destinations, textures). A fetch or a swap asks for the
  // exact size (a resolve created it with the aligned height: resized once, contents copied); a
  // resolve asks for a minimum size (`at_least`) and creates with the aligned size (`create_w/h`).
  HostImage* GuestImage(Frame& f, uint32_t base, uint32_t width, uint32_t height, DkImageFormat format, bool at_least,
                        uint32_t create_w = 0, uint32_t create_h = 0) {
    auto it = guest_images_.find(base);
    if (it != guest_images_.end()) {
      HostImage* img = it->second;
      if (img->format != format) {
        // The memory was reused with another format (a texture over an old resolve destination or the
        // reverse): a new image. ponytail: the old image's pool memory is not reclaimed (bump pool);
        // a free list when the pool fills in a stage.
        HostImage* fresh = CreateImage(std::max(width, create_w), std::max(height, create_h), format, DkImageFlags_UsageRender, false);
        if (!fresh) return nullptr;
        fresh->guest_base = base;
        Log("image at %08X recreated: format %u -> %u, %ux%u -> %ux%u", base, uint32_t(img->format), uint32_t(format), img->width,
            img->height, fresh->width, fresh->height);
        NoteImage("swap %u frame %u recreated %08X fmt %u->%u %ux%u->%ux%u", swaps_ + 1, frame_, base, uint32_t(img->format), uint32_t(format), img->width,
                  img->height, fresh->width, fresh->height);
        it->second = fresh;
        RecycleImage(img);
        return fresh;
      }
      if (img->width == width && img->height == height) return img;
      if (at_least && width <= img->width && height <= img->height) return img;
      // gh: a resolve rect is a multiple of 8 (320x184) but the texture fetch is exact (320x180): growing here made the
      // image ping-pong between the two sizes (a new image and a copy, several times per frame). The resolve clips instead.
      if (at_least && width <= img->width + 7 && height <= img->height + 7) return img;
      if (at_least) {
        width = std::max(width, img->width);
        height = std::max(height, img->height);
      }
      HostImage* resized = CreateImage(width, height, img->format, DkImageFlags_UsageRender, false);
      if (!resized) return img;
      uint32_t cw = std::min(width, img->width), ch = std::min(height, img->height);
      DkImageView sv, dv;
      dkImageViewDefaults(&sv, &img->image);
      dkImageViewDefaults(&dv, &resized->image);
      DkImageRect sr{0, 0, 0, cw, ch, 1}, dr{0, 0, 0, cw, ch, 1};
      // gh: only a resolve destination keeps its picture. A texture of another size at the same address is another
      // texture: the old bytes are wrong for it, the upload fills it (content_serial 0). Both GPU hangs (0.85 level 3,
      // 0.115 title) were in the frame of such a texture resize copy (576x192 -> 64x160 k_8, 1280x720 -> 672x720 DXT1);
      // 0.95 did one (96x64 -> 64x64) without a hang, so the copy is a suspect, not a proven cause.
      if (img->from_resolve) {
        dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufCopyImage(f.cmdbuf, &sv, &sr, &dv, &dr, 0);
        dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
        frame_resize_copies_++;
        resized->from_resolve = true;
        resized->content_serial = img->content_serial;
        resized->checked_serial = img->checked_serial;
      }
      resized->guest_base = base;
      Log("image at %08X resized %ux%u -> %ux%u", base, img->width, img->height, width, height);
      NoteImage("swap %u frame %u resized %08X %ux%u->%ux%u (%s)", swaps_ + 1, frame_, base, img->width, img->height, width, height,
                img->from_resolve ? "copied" : "texture, no copy");
      it->second = resized;
      RecycleImage(img);  // gh: its pool memory is reused by the next request of its size
      return resized;
    }
    HostImage* img = CreateImage(std::max(width, create_w), std::max(height, create_h), format,
                                 DkImageFlags_UsageRender, false);
    if (!img) return nullptr;
    img->guest_base = base;
    guest_images_.emplace(base, img);
    return img;
  }

  HostImage* TextureImage(Frame& f, const TextureInfo& info, const xenos::xe_gpu_texture_fetch_t& fetch) {
    DkImageFormat format;
    if (!TextureFormatOf(info.format, format)) {
      LogOnce((std::string("tfmt ") + info.format_info()->name).c_str(), "unsupported texture format %s %ux%u at %08X: the draw keeps whatever was bound (stale)",
              info.format_info()->name, info.width + 1, info.height + 1, info.memory.base_address);
      return nullptr;
    }
    uint32_t base = info.memory.base_address;
    // TextureInfo keeps the fetch constant's fields: the size minus one.
    HostImage* img = GuestImage(f, base, info.width + 1, info.height + 1, format, false);
    if (!img) return nullptr;
    if (img->checked_serial == serial_) return img;  // already checked for this draw
    img->checked_serial = serial_;
    // Never filled, or written by the guest (or a file read) after the upload / resolve that filled it.
    uint32_t extent = info.memory.base_size;
    if (info.is_tiled) {
      const FormatInfo* fi = info.format_info();
      const uint32_t bw = (info.width + fi->block_width) / fi->block_width, bh = (info.height + fi->block_height) / fi->block_height;  // size minus one in the fetch
      extent = std::max(extent, gh_tex::TiledExtentBytes(bw, bh, GuestPitchBlocks(info.pitch, fi->block_width), fi->bytes_per_block()));
    }
    // gh: a palette is 1 KB: its guest bytes are compared at every bind, so a write the dirty bytes missed (a host write,
    // a store form without a mark) cannot leave a stale palette. Larger textures trust the dirty bytes.
    const bool small = extent <= 4096 && !img->from_resolve;
    const uint64_t hash = small ? ::gh::SpriteHash(memory_->TranslatePhysical(base), extent) : 0;
    if (img->content_serial && !WrittenSince(base, extent, img->content_serial)) {
      if (!small || hash == img->content_hash) return img;
      if (unmarked_changes_++ < 20) Log("texture at %08X (%ux%u) changed WITHOUT a dirty mark: uploaded again (a store path does not mark its page)",
                                        base, info.width + 1, info.height + 1);
    }
    // A failed re-upload (staging ring full) keeps the previous contents rather than unbinding the slot.
    if (!UploadTexture(f, *img, info, fetch.endianness)) return img->content_serial ? img : nullptr;
    if (img->content_serial) {
      reuploads_++;
      frame_reuploads_++;
      if (reuploads_ <= 8) Log("texture at %08X re-uploaded (%s, %ux%u): its memory changed", base,
                               img->from_resolve ? "was a resolve destination" : "was uploaded before", img->width, img->height);
    }
    img->from_resolve = false;
    img->content_serial = serial_;
    img->content_hash = hash;
    return img;
  }

  // gh: button glyphs, see the block above kGlyphW.
  void PatchGlyphs(const FormatInfo* fi, uint8_t* staged, uint32_t w, uint32_t h) {
    const std::string name(fi->name);
    constexpr size_t kTopBytes = size_t(75) * kAtlasW * 2;
    if (name == "k_4_4_4_4" && w == kAtlasW && h == kAtlasH) {
      atlas_top_.assign(staged, staged + kTopBytes);
      if (SwapAtlasGlyphs(staged)) Log("button glyphs: X<->Y swapped in the UI font atlas (A and B follow the name mapping)");
      else LogOnce("glyphs-atlas", "button glyphs: the UI font atlas does not look as expected, left alone");
    }
  }

  void DumpTexture(const FormatInfo* fi, const TextureInfo& info, const uint8_t* staged, uint32_t w, uint32_t h) {
    if (w < 16 || h < 8 || w > 4096 || h > 4096) return;
    const uint64_t key = (uint64_t(info.memory.base_address) << 24) ^ (uint64_t(w) << 12) ^ h;
    if (!dumped_tex_.insert(key).second || dumped_tex_.size() > 1500) return;
    RgbImage img;
    if (!DecodeStaged(fi->name, staged, w, h, img)) return;
    mkdir((debug_dir_ + "tex").c_str(), 0777);
    char path[160];
    std::snprintf(path, sizeof(path), "%stex/t%04zu_%08X_%ux%u_%s.ppm", debug_dir_.c_str(), dumped_tex_.size(), info.memory.base_address, w, h,
                  fi->name);
    if (FILE* fp = std::fopen(path, "wb")) {
      std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
      std::fwrite(img.rgb.data(), 1, img.rgb.size(), fp);
      std::fclose(fp);
    }
  }

  // gh: copies tightly packed texels into an image (the replacement sprites).
  // gh: a 256x1 image of zeros, made once: the hatch mask while hatching is off. nullptr when it did not fit this time.
  HostImage* NoHatchImage(Frame& f) {
    if (!no_hatch_) no_hatch_ = CreateImage(256, 1, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
    static const uint8_t zero[1024] = {};
    if (no_hatch_ && !no_hatch_ready_) no_hatch_ready_ = UploadRaw(f, *no_hatch_, zero, sizeof(zero), 256, 1);
    return no_hatch_ready_ ? no_hatch_ : nullptr;
  }

  bool UploadRaw(Frame& f, HostImage& img, const uint8_t* data, uint32_t bytes, uint32_t w, uint32_t h) {
    uint32_t off = f.staging.Alloc(bytes, 0x100);
    if (off == UINT32_MAX) { LogOnce("staging", "staging ring full: %u KB texture not uploaded", bytes >> 10); return false; }
    std::memcpy(f.staging.cpu + off, data, bytes);
    DkCopyBuf cb{f.staging.gpu + off, 0, 0};
    DkImageView view;
    dkImageViewDefaults(&view, &img.image);
    DkImageRect rect{0, 0, 0, w, h, 1};
    dkCmdBufCopyBufferToImage(f.cmdbuf, &cb, &view, &rect, 0);
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
    uploads_++;
    upload_bytes_ += bytes;
    return true;
  }

  // gh: the GPU images of a replacement sprite: an R8 index image at the replacement's size and a 256x1 palette in
  // the staged byte order of the game's own palette textures (B, G, R, A). Created and uploaded once; false when
  // something did not fit (the draw then keeps the original sprite and tries again next time).
  bool ModImages(Frame& f, ::gh::ModSprite* m, HostImage*& index, HostImage*& clut) {
    if (!m->index_image) m->index_image = CreateImage(m->w, m->h, DkImageFormat_R8_Unorm, DkImageFlags_UsageRender, false);
    if (!m->clut_image && !m->game_palette) m->clut_image = CreateImage(256, 1, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
    index = static_cast<HostImage*>(m->index_image);
    clut = static_cast<HostImage*>(m->clut_image);
    if (!index || (!clut && !m->game_palette)) return false;
    if (!(m->gpu_state & 1)) {
      if (!UploadRaw(f, *index, m->index.data(), m->w * m->h, m->w, m->h)) return false;
      m->gpu_state |= 1;
    }
    if (!m->game_palette && !(m->gpu_state & 2)) {
      uint8_t staged[256 * 4];
      for (int i = 0; i < 256; i++) {
        staged[i * 4 + 0] = m->palette[i * 4 + 2];
        staged[i * 4 + 1] = m->palette[i * 4 + 1];
        staged[i * 4 + 2] = m->palette[i * 4 + 0];
        staged[i * 4 + 3] = m->palette[i * 4 + 3];
      }
      if (!UploadRaw(f, *clut, staged, sizeof(staged), 256, 1)) return false;
      m->gpu_state |= 2;
    }
    return true;
  }

  // gh: the RGBA8 image of a true-colour replacement (mods/textures/), created and uploaded once; nullptr when it
  // did not fit this time (the draw keeps the game's texture and tries again next time).
  HostImage* ModRgbaImage(Frame& f, ::gh::ModSprite* m) {
    const uint32_t bytes = m->w * m->h * 4;
    if (bytes > kStagingBytes) {
      LogOnce("texmod-big", "texture mod: a %ux%u replacement is larger than the %u MB upload ring, not used", m->w, m->h, kStagingBytes >> 20);
      return nullptr;
    }
    if (!m->index_image) m->index_image = CreateImage(m->w, m->h, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
    HostImage* img = static_cast<HostImage*>(m->index_image);
    if (!img) return nullptr;
    if (!(m->gpu_state & 1)) {
      if (!UploadRaw(f, *img, m->rgba.data(), bytes, m->w, m->h)) return nullptr;
      m->gpu_state |= 1;
      std::vector<uint8_t>().swap(m->rgba);  // on the GPU now
    }
    return img;
  }

  // gh: diagnostics, the first 40 uploads of an unusual shape: what the staging decode assumes versus the fetch constant.
  // Look for: "packed 1" (mip tail textures: the base is at an offset the decode ignores), mips, a stride that differs
  // from the pitch, a texture that is not 2D.
  void UploadDiag(const FormatInfo* fi, const TextureInfo& info, uint32_t width, uint32_t height) {
    const uint32_t stride = GuestPitchBlocks(info.pitch, fi->block_width);
    const bool odd = info.has_packed_mips || info.mip_max_level > 0 || std::min(width, height) <= 16 ||
                     stride != info.pitch / fi->block_width || info.pitch != ((width + 31) & ~31u) ||
                     info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked;
    if (!odd || diag_count_ >= 40) return;
    diag_count_++;
    Log("texdiag %s %ux%u %s pitch %u (stride %u blocks) packed %u mips %u..%u dim %u stacked %u base %08X mip %08X endian %u", fi->name, width, height,
        info.is_tiled ? "tiled" : "linear", info.pitch, stride, info.has_packed_mips, info.mip_min_level, info.mip_max_level, uint32_t(info.dimension),
        info.is_stacked, info.memory.base_address, info.memory.mip_address, uint32_t(info.endianness));
  }
  uint32_t diag_count_ = 0;

  // gh: dump_tex=N: raw guest bytes (the memory behind the fetch constant, before detile and endian swap) and a .txt with the
  // layout, for tools/decode_texdump.py.
  uint32_t raw_dump_left_ = 0;
  std::unordered_set<uint64_t> raw_dumped_;
  void DumpRaw(const FormatInfo* fi, const TextureInfo& info, uint32_t width, uint32_t height, uint32_t stride, const uint8_t* src,
               xenos::Endian endian) {
    if (!raw_dump_left_ || width < 256 || height < 256) return;
    if (!raw_dumped_.insert((uint64_t(info.memory.base_address) << 24) ^ (uint64_t(width) << 12) ^ height).second) return;
    raw_dump_left_--;
    // The tiled extent: rows rounded up to whole 32 block tiles, stride blocks wide.
    const uint32_t bpb = fi->bytes_per_block();
    const uint32_t brows = (height + fi->block_height - 1) / fi->block_height;
    size_t bytes = size_t(stride) * ((brows + 31) & ~31u) * bpb;
    if (bytes < info.memory.base_size) bytes = info.memory.base_size;
    if (bytes > (32u << 20)) bytes = 32u << 20;
    char path[200];
    std::snprintf(path, sizeof(path), "%stexdump/t%08X_%ux%u_%s", debug_dir_.c_str(), info.memory.base_address, width, height, fi->name);
    std::string bin = std::string(path) + ".bin", txt = std::string(path) + ".txt";
    if (FILE* fp = std::fopen(bin.c_str(), "wb")) { std::fwrite(src, 1, bytes, fp); std::fclose(fp); }
    if (FILE* fp = std::fopen(txt.c_str(), "w")) {
      std::fprintf(fp, "format %s\nwidth %u\nheight %u\npitch %u\nstride_blocks %u\nblock_width %u\nblock_height %u\nbytes_per_block %u\nendian %u\ntiled %u\n"
                       "base %08X\nbase_size %u\npacked_mips %u\nmip_max %u\nbin_bytes %zu\n",
                   fi->name, width, height, info.pitch, stride, fi->block_width, fi->block_height, bpb, uint32_t(endian), info.is_tiled ? 1 : 0,
                   info.memory.base_address, uint32_t(info.memory.base_size), info.has_packed_mips ? 1 : 0, uint32_t(info.mip_max_level), bytes);
      std::fclose(fp);
    }
    Log("dump_tex: %s %ux%u pitch %u endian %u -> %s (%zu KB)", fi->name, width, height, info.pitch, uint32_t(endian), bin.c_str(), bytes >> 10);
  }

  bool UploadTexture(Frame& f, HostImage& img, const TextureInfo& info, xenos::Endian endian) {
    Tick perf_tick{perf_upload_};
    const FormatInfo* fi = info.format_info();
    const uint32_t width = info.width + 1, height = info.height + 1;  // TextureInfo keeps size minus one
    uint32_t bw = (width + fi->block_width - 1) / fi->block_width;
    uint32_t bh = (height + fi->block_height - 1) / fi->block_height;
    uint32_t bpb = fi->bytes_per_block();
    uint32_t bytes = bw * bh * bpb;
    uint32_t off = f.staging.Alloc(bytes, 0x100);
    if (off == UINT32_MAX) { LogOnce("staging", "staging ring full: %u KB texture not uploaded", bytes >> 10); return false; }
    uint8_t* dst = f.staging.cpu + off;
    const uint8_t* src = memory_->TranslatePhysical(info.memory.base_address);
    const uint32_t stride = GuestPitchBlocks(info.pitch, fi->block_width);  // gh: pitch is in texels, the tiles are 32 BLOCKS wide: round up (DXT 608 -> 160, not 152)
    DumpRaw(fi, info, width, height, stride, src, endian);
    {
      Tick detile_tick{perf_detile_};
      if (info.is_tiled) gh_tex::DetileBlocks(dst, src, bw, bh, stride, bpb, uint32_t(endian));
      else gh_tex::CopyLinearBlocks(dst, src, bw, bh, stride, bpb, uint32_t(endian));
    }
    img.mod = nullptr;
    img.mod_kind = 0;
    img.mod_epoch = rex_nx_mod_epoch;
    if (sprite_mod_on_ && std::strcmp(fi->name, "k_8") == 0) {
      // gh: 8 bit textures are the sprite frames (indices; the palette is a second texture). Identify by content.
      const uint64_t hash = ::gh::SpriteHash(dst, size_t(width) * height);
      img.mod_kind = 1;
      img.mod_hash = hash;
      img.mod = sprite_mod_.Find(width, height, hash);
      if (img.mod) {
        const std::string id = ::gh::SpriteId(width, height, hash);
        LogOnce(("spritemod " + id).c_str(), "sprite mod: %s replaced (%ux%u -> %ux%u)", id.c_str(), width, height, img.mod->w, img.mod->h);
      }
      else if (!sprite_mod_.last_error().empty()) { LogOnce("spritemod-err", "sprite mod: %s", sprite_mod_.last_error().c_str()); }
    }
    if (dump_tex_ && std::strcmp(fi->name, "k_8") == 0 && width * height >= 1024) {
      const std::string id = ::gh::SpriteId(width, height, ::gh::SpriteHash(dst, size_t(width) * height));
      LogOnce(("spriteid " + id).c_str(), "sprite id %s (k_8 texture at %08X)", id.c_str(), info.memory.base_address);
    }
    const bool true_colour_fmt = std::strcmp(fi->name, "k_8_8_8_8") == 0 || std::strcmp(fi->name, "k_DXT1") == 0 ||
                                 std::strcmp(fi->name, "k_DXT2_3") == 0 || std::strcmp(fi->name, "k_DXT4_5") == 0;
    const bool texture_lookup = texture_mod_on_ && true_colour_fmt && texture_mod_.HasSize(width, height);
    if (texture_lookup || (dump_tex_ && true_colour_fmt)) {
      // gh: true-colour textures (portraits): the same ids as the sprites, over all staged bytes. Hashed only when a
      // mod file has this size (or with the dump marker): big textures are uploaded often.
      const uint64_t hash = ::gh::SpriteHash(dst, bytes);
      const std::string id = ::gh::SpriteId(width, height, hash);
      if (dump_tex_) LogOnce(("textureid " + id).c_str(), "texture id %s (%s)", id.c_str(), fi->name);
      if (texture_lookup) {
        img.mod_kind = 2;
        img.mod_hash = hash;
        img.mod = texture_mod_.Find(width, height, hash);
        const std::string key = "texmod " + id;  // one line per id: replaced, load error or no file
        if (img.mod) LogOnce(key.c_str(), "texture mod: %s replaced (%ux%u -> %ux%u)", id.c_str(), width, height, img.mod->w, img.mod->h);
        else if (!texture_mod_.last_error().empty()) LogOnce(key.c_str(), "texture mod: %s", texture_mod_.last_error().c_str());
        else LogOnce(key.c_str(), "texture mod: no file for %s (%s at %08X)", id.c_str(), fi->name, info.memory.base_address);
      }
    }
    if (REXCVAR_GET(button_glyphs) == "switch") PatchGlyphs(fi, dst, width, height);
    if (dump_tex_) DumpTexture(fi, info, dst, width, height);
    UploadDiag(fi, info, width, height);
    DkCopyBuf cb{f.staging.gpu + off, 0, 0};
    DkImageView view;
    dkImageViewDefaults(&view, &img.image);
    DkImageRect rect{0, 0, 0, width, height, 1};
    // gh: the copy engine must not overwrite an image that earlier draws (this frame or the one in flight) still sample:
    // a re-upload in place, a recycled pool slot. One wait per draw is enough (the barrier after each copy covers later copies).
    if (serial_ != upload_barrier_serial_) { dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, 0); upload_barrier_serial_ = serial_; }
    dkCmdBufCopyBufferToImage(f.cmdbuf, &cb, &view, &rect, 0);
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
    uploads_++;
    upload_bytes_ += bytes;
    if (uploads_ <= 8) Log("texture %s %ux%u %s endian %u at %08X: %u KB uploaded", fi->name, width, height,
                           info.is_tiled ? "tiled" : "linear", uint32_t(endian), info.memory.base_address, bytes >> 10);
    return true;
  }

  uint32_t DescriptorFor(Frame& f, HostImage* img, uint32_t swizzle) {
    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(img)) ^ (uint64_t(swizzle) << 48);
    auto it = f.descriptors.find(key);
    if (it != f.descriptors.end()) return it->second;
    if (f.next_descriptor >= kMaxImageDescriptors) { LogOnce("desc", "image descriptors exhausted"); return UINT32_MAX; }
    DkImageView view;
    dkImageViewDefaults(&view, &img->image);
    if (swizzle) {
      view.swizzle[0] = SwizzleOf(swizzle);
      view.swizzle[1] = SwizzleOf(swizzle >> 3);
      view.swizzle[2] = SwizzleOf(swizzle >> 6);
      view.swizzle[3] = SwizzleOf(swizzle >> 9);
    }
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, false, false);
    uint32_t slot = f.next_descriptor++;
    std::memcpy(descriptors_.cpu + f.desc_offset + slot * sizeof(DkImageDescriptor), &desc, sizeof(desc));
    // The GPU caches texture headers: a slot rewritten by the CPU must be invalidated before the draw.
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_None, DkInvalidateFlags_Descriptors);
    f.descriptors.emplace(key, slot);
    return slot;
  }

  // ----- draws ----------------------------------------------------------------------------------
  bool CopyIndices(Frame& f, const IndexBufferInfo& ib, uint32_t& offset_out, uint32_t& vertex_count_out) {
    uint32_t bytes = uint32_t(ib.length);
    uint32_t off = f.data.Alloc(bytes, 64);
    if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (indices)"); return false; }
    const uint8_t* src = memory_->TranslatePhysical(ib.guest_base);
    uint32_t max_index = 0;
    if (ib.format == xenos::IndexFormat::kInt32) {
      const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
      uint32_t* d = reinterpret_cast<uint32_t*>(f.data.cpu + off);
      for (uint32_t i = 0; i < ib.count; i++) {
        uint32_t v = xenos::GpuSwap(s[i], ib.endianness);
        d[i] = v;
        if (v > max_index) max_index = v;
      }
    } else {
      const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
      uint16_t* d = reinterpret_cast<uint16_t*>(f.data.cpu + off);
      for (uint32_t i = 0; i < ib.count; i++) {
        uint16_t v = xenos::GpuSwap(s[i], ib.endianness);
        d[i] = v;
        if (v > max_index) max_index = v;
      }
    }
    offset_out = off;
    vertex_count_out = max_index + 1;
    return true;
  }

  bool BindUniforms(Frame& f, DkShaderObj* vs, DkShaderObj* ps, const draw_util::ViewportInfo& vi, const RegisterFile& regs) {
    DkCmdBuf cmd = f.cmdbuf;
    uint32_t vs_off = f.data.Alloc(4096, DK_UNIFORM_BUF_ALIGNMENT);
    uint32_t ps_off = f.data.Alloc(4096, DK_UNIFORM_BUF_ALIGNMENT);
    uint32_t sys_off = f.data.Alloc(sizeof(SysConstants), DK_UNIFORM_BUF_ALIGNMENT);
    if (vs_off == UINT32_MAX || ps_off == UINT32_MAX || sys_off == UINT32_MAX) { LogOnce("ring", "frame data ring full (uniforms)"); return false; }
    std::memcpy(f.data.cpu + vs_off, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], 4096);
    std::memcpy(f.data.cpu + ps_off, &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], 4096);
    SysConstants sys{};
    for (int i = 0; i < 3; i++) {
      sys.ndc_scale[i] = vi.ndc_scale[i];
      sys.ndc_offset[i] = vi.ndc_offset[i];
    }
    auto cc = regs.Get<reg::RB_COLORCONTROL>();
    sys.alpha[0] = cc.alpha_test_enable ? 1.0f : 0.0f;
    sys.alpha[1] = float(uint32_t(cc.alpha_func));
    sys.alpha[2] = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    std::memcpy(sys.bools, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], sizeof(sys.bools));
    std::memcpy(sys.loops, &regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], sizeof(sys.loops));
    std::memcpy(f.data.cpu + sys_off, &sys, sizeof(sys));
    DkBufExtents vs_bufs[2] = {{f.data.gpu + vs_off, 4096}, {f.data.gpu + sys_off, sizeof(SysConstants)}};
    DkBufExtents ps_bufs[2] = {{f.data.gpu + ps_off, 4096}, {f.data.gpu + sys_off, sizeof(SysConstants)}};
    dkCmdBufBindUniformBuffers(cmd, DkStage_Vertex, 0, vs_bufs, 2);
    dkCmdBufBindUniformBuffers(cmd, DkStage_Fragment, 0, ps_bufs, 2);
    (void)vs;
    (void)ps;
    return true;
  }

  // The XDK's Clear: rectangles (position xyz + color rgba, stride 7 words) with the depth
  // function "always"; half of them through the 2x MSAA half-pitch alias of the surface.
  bool IsClearDraw(DkShaderObj* vs, DkShaderObj* ps, xenos::PrimitiveType prim) {
    if (prim != xenos::PrimitiveType::kRectangleList || !ps->texture_bindings().empty()) return false;
    const auto& vbs = vs->vertex_bindings();
    if (vbs.size() != 1 || vbs[0].stride_words != 7 || vbs[0].attributes.size() != 2) return false;
    const auto& a0 = vbs[0].attributes[0].fetch_instr.attributes;
    const auto& a1 = vbs[0].attributes[1].fetch_instr.attributes;
    return a0.data_format == xenos::VertexFormat::k_32_32_32_FLOAT && a0.offset == 0 &&
           a1.data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT && a1.offset == 3;
  }

  bool ClearDraw(uint32_t vertex_count, xenos::EdramMode mode) {
    Tick perf_tick{perf_clear_};
    const RegisterFile& regs = *register_file_;
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    auto fetch = regs.GetVertexFetch(0);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    uint32_t scale = surf.msaa_samples == xenos::MsaaSamples::k4X ? 2 : 1;
    uint32_t pitch = EffectivePitch(surf);
    auto word = [&](uint32_t i) {
      uint32_t w = words[i];
      if (fetch.endian == xenos::Endian::k8in32) w = __builtin_bswap32(w);
      float v;
      std::memcpy(&v, &w, 4);
      return v;
    };
    for (uint32_t r = 0; r + 3 <= vertex_count && r * 7 + 21 <= fetch.size; r += 3) {
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (uint32_t v = 0; v < 3; v++) {
        float x = word((r + v) * 7), y = word((r + v) * 7 + 1);
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
      float z = word(r * 7 + 2);
      float color[4] = {word(r * 7 + 3), word(r * 7 + 4), word(r * 7 + 5), word(r * 7 + 6)};
      DkScissor sc;
      sc.x = uint32_t(std::max(0.0f, std::floor(x0 + 0.5f))) * scale;
      sc.y = uint32_t(std::max(0.0f, std::floor(y0 + 0.5f))) * scale;
      sc.width = uint32_t(std::max(0.0f, std::floor(x1 + 0.5f) - std::floor(x0 + 0.5f))) * scale;
      sc.height = uint32_t(std::max(0.0f, std::floor(y1 + 0.5f) - std::floor(y0 + 0.5f))) * scale;
      HostImage* color_img = nullptr;
      HostImage* depth_img = nullptr;
      if (mode == xenos::EdramMode::kColorDepth) {
        color_img = TargetImage(color_info.color_base, pitch, ColorTargetFormatOf(color_info.color_format), false);
        depth_img = TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true);
      } else if (mode == xenos::EdramMode::kDepthOnly) {
        // Depth-only through the alias: the XDK fills a color surface's EDRAM this way too.
        auto it = targets_.find((uint64_t(depth_info.depth_base) << 32) | (uint64_t(pitch) << 1));
        if (it != targets_.end()) color_img = it->second;
        else depth_img = TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true);
      }
      if (!color_img && !depth_img) continue;
      BindTargets(cmd, color_img, depth_img);
      uint32_t w = color_img ? color_img->width : depth_img->width, h = color_img ? color_img->height : depth_img->height;
      if (sc.x >= w || sc.y >= h) continue;
      if (sc.x + sc.width > w) sc.width = w - sc.x;
      if (sc.y + sc.height > h) sc.height = h - sc.y;
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      if (color_img) dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, color);
      if (depth_img) dkCmdBufClearDepthStencil(cmd, true, z, 0xFF, 0);
      {
        char key[96];
        std::snprintf(key, sizeof(key), "clear %u %u %u %u %u %u %u %u", uint32_t(mode), color_info.color_base, depth_info.depth_base, pitch,
                      sc.x, sc.y, sc.width, sc.height);
        LogOnce(key, "clear %s%s at EDRAM %u/%u pitch %u: rect %u,%u %ux%u color %g %g %g %g z %g (alias x%u)", color_img ? "color" : "",
                depth_img ? " depth" : "", color_info.color_base, depth_info.depth_base, pitch, sc.x, sc.y, sc.width, sc.height, color[0],
                color[1], color[2], color[3], z, scale);
      }
      clears_++;
      frame_clears_++;
    }
    return true;
  }

  bool Resolve() {
    Tick perf_tick{perf_copy_};
    const RegisterFile& regs = *register_file_;
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    draw_util::ResolveInfo info;
    if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) { LogOnce("resolve", "GetResolveInfo failed"); return true; }
    if (info.IsCopyingDepth()) {
      LogOnce("depthresolve", "depth resolve to %08X ignored", info.copy_dest_base);
      return true;
    }
    uint32_t src_pitch = info.color_edram_info.pitch_tiles * 80;
    HostImage* src = TargetImage(info.color_edram_info.base_tiles, src_pitch, DkImageFormat_RGBA8_Unorm, false);
    if (!src) return true;
    uint32_t x = info.coordinate_info.edram_offset_x_div_8 * 8, y = info.coordinate_info.edram_offset_y_div_8 * 8;
    uint32_t w = info.coordinate_info.width_div_8 * 8, h = info.height_div_8 * 8;
    uint32_t dest_pitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
    uint32_t dest_height = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
    (void)dest_pitch; (void)dest_height;
    uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8, dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    HostImage* dst = GuestImage(f, info.copy_dest_base, dx + w, dy + h, DkImageFormat_RGBA8_Unorm, true);  // gh: exact size (the aligned one made it ping-pong with the texture bind)
    if (!dst) return true;
    dst->from_resolve = true;
    dst->content_serial = serial_;
    dst->mod = nullptr;  // gh: rendered contents now, not the uploaded texture a mod replaced
    if (x + w > src->width) w = x < src->width ? src->width - x : 0;
    if (y + h > src->height) h = y < src->height ? src->height - y : 0;
    if (dx + w > dst->width) w = dx < dst->width ? dst->width - dx : 0;
    if (dy + h > dst->height) h = dy < dst->height ? dst->height - dy : 0;
    if (w && h) {
      DkImageView sv, dv;
      dkImageViewDefaults(&sv, &src->image);
      dkImageViewDefaults(&dv, &dst->image);
      DkImageRect sr{x, y, 0, w, h, 1}, dr{dx, dy, 0, w, h, 1};
      dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
      dkCmdBufCopyImage(cmd, &sv, &sr, &dv, &dr, 0);
      dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    }
    if (info.IsClearingColor()) {
      BindTargets(cmd, src, nullptr);
      DkScissor sc{x, y, w, h};
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      uint32_t c = info.rb_color_clear;
      float color[4] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f};
      dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, color);
    }
    {
      // Every distinct resolve (source rect, destination and offset) once: the frame structure.
      char key[96];
      std::snprintf(key, sizeof(key), "resolve %u %u %u %u %u %u %08X %u %u", info.color_edram_info.base_tiles, src_pitch, x, y, w, h,
                    info.copy_dest_base, dx, dy);
      LogOnce(key, "resolve EDRAM %u pitch %u rect %u,%u %ux%u -> %08X (%ux%u image) at %u,%u%s", info.color_edram_info.base_tiles, src_pitch,
              x, y, w, h, info.copy_dest_base, dst->width, dst->height, dx, dy, info.IsClearingColor() ? ", then cleared" : "");
    }
    resolves_++;
    frame_resolves_++;
    return true;
  }

  // ----- shaders --------------------------------------------------------------------------------
  // The same two files as Shader::DumpUcode (shader_<hash>.ucode.bin.<ext> and the disassembly
  // shader_<hash>.ucode.<ext>, the translator's input), written with stdio: the SDK's version goes
  // through std::filesystem::absolute, which turns sdmc:/x into /sdmc:/x on newlib (finding 0.41).
  void DumpMissing(const DkShaderObj& shader) {
    std::string dir = shader_dir_ + "missing/";
    mkdir(dir.c_str(), 0777);
    const char* ext = shader.type() == xenos::ShaderType::kVertex ? "vert" : "frag";
    char name[512];
    std::snprintf(name, sizeof(name), "%sshader_%016llX.ucode.bin.%s", dir.c_str(), (unsigned long long)shader.ucode_data_hash(), ext);
    if (FILE* fp = std::fopen(name, "wb")) {
      std::fwrite(shader.ucode_data().data(), 4, shader.ucode_data().size(), fp);
      std::fclose(fp);
    } else {
      Log("cannot write %s", name);
    }
    std::snprintf(name, sizeof(name), "%sshader_%016llX.ucode.%s", dir.c_str(), (unsigned long long)shader.ucode_data_hash(), ext);
    if (FILE* fp = std::fopen(name, "w")) {
      std::fwrite(shader.ucode_disassembly().data(), 1, shader.ucode_disassembly().size(), fp);
      std::fclose(fp);
    }
  }

  bool LoadDksh(const std::string& path, DkShader& out) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    struct { uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs; } hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.control_sz < sizeof(hdr) || hdr.control_sz > (1u << 20) || hdr.code_sz > (4u << 20)) {
      std::fclose(fp);
      return false;
    }
    std::vector<uint8_t> control(hdr.control_sz);
    std::rewind(fp);
    if (std::fread(control.data(), hdr.control_sz, 1, fp) != 1) { std::fclose(fp); return false; }
    uint32_t off = code_.Alloc(hdr.code_sz, DK_SHADER_CODE_ALIGNMENT);
    if (off == UINT32_MAX) { std::fclose(fp); Log("shader code pool full"); return false; }
    if (std::fread(code_.cpu + off, hdr.code_sz, 1, fp) != 1) { std::fclose(fp); return false; }
    std::fclose(fp);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, code_.block, off);
    sm.control = control.data();
    sm.programId = 0;
    dkShaderInitialize(&out, &sm);
    return true;
  }

  void LogOnce(const char* key, const char* fmt, ...) {
    if (!logged_.insert(key).second) return;
    va_list ap;
    va_start(ap, fmt);
    std::printf("[dk] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
  }

  DkDevice device_ = nullptr;
  DkQueue queue_ = nullptr;
  DkSwapchain swapchain_ = nullptr;
  MemBlock code_, images_, descriptors_, cmdmem_;
  uint32_t sampler_desc_offset_ = 0;
  Frame frames_[kFrames];
  uint32_t frame_ = 0;
  HostImage* swap_images_[kFrames] = {};
  DkImage const* swap_image_ptrs_[kFrames] = {};
  DkShader present_vs_shader_{}, present_fs_shader_{};
  bool present_vs_ = false, present_fs_ = false;
  bool present_target_ = false;
  bool dump_images_ = false;
  bool dump_tex_ = false;  // gh: marker dk_dump_tex
  bool draw_diag_ = false;  // gh: marker dk_diag
  bool skip_frame_ = false;  // gh: frame skip: this guest frame is not drawn
  bool gpu_dead_ = false;    // gh: a fence never signalled (BeginFrame): no GPU work any more
  uint32_t fence_timeouts_ = 0, frame_resize_copies_ = 0;
  char frame_note_[kFrames][192] = {};  // gh: what each frame slot last submitted, for the log of a hang
  uint32_t perf_skipped_ = 0, perf_skip_new_ = 0, last_front_[3] = {};
  uint64_t perf_upload_ = 0, perf_detile_ = 0;
  uint64_t perf_draw_ = 0, perf_tex_ = 0, perf_clear_ = 0, perf_copy_ = 0, perf_swap_ = 0, perf_acquire_ = 0, perf_fence_ = 0;
  uint64_t perf_t0_ = 0, perf_kind_[4] = {}, perf_poll_ = 0, perf_hooks_ = 0, perf_upload_bytes_ = 0;
  uint32_t perf_frames_ = 0, perf_draws_ = 0, perf_clears_ = 0, perf_resolves_ = 0, perf_uploads_ = 0;
  ::gh::SpriteMod sprite_mod_;  // gh: replacement sprites from the mod folder
  bool sprite_mod_on_ = false;
  HostImage* no_hatch_ = nullptr;
  bool no_hatch_ready_ = false;
  ::gh::SpriteMod texture_mod_;  // gh: replacement true-colour textures (RGBA, mods/textures/)
  bool texture_mod_on_ = false;
  std::unordered_set<uint64_t> dumped_tex_;
  std::vector<uint8_t> atlas_top_;  // gh: rows 0..74 of the UI font atlas as staged (little-endian u16), unswapped
  HostImage* overlay_[kFrames] = {};  // gh: the version / fps strip, one image per frame in flight
  HostImage* menu_img_[kFrames] = {};  // gh: the settings menu panel
  std::vector<uint32_t> menu_px_ = std::vector<uint32_t>(kMenuW * kMenuH);
  char overlay_text_[64] = {};
  uint64_t fps_tick_ = 0;
  uint32_t fps_frames_ = 0, fps_x10_ = 0;
  std::vector<uint32_t> overlay_px_ = std::vector<uint32_t>(kOverlayW * kOverlayH);
  std::string debug_dir_;
  std::string shader_dir_;
  std::vector<uint32_t> page_serial_ = std::vector<uint32_t>(0x20000);  // per 4 KB physical page: the draw serial of its last write
  uint32_t serial_ = 1;  // the draw serial (SyncDirtyPages)
  uint32_t upload_barrier_serial_ = 0;  // gh: the draw serial of the last wait before a texture copy
  uint64_t dirty_pages_ = 0, dirty_pages_at_swap_ = 0;
  std::unordered_map<uint64_t, DkShaderObj*> shaders_;
  std::unordered_map<uint64_t, HostImage*> targets_;       // EDRAM base/pitch -> render target
  std::unordered_map<uint32_t, HostImage*> guest_images_;  // guest address -> texture / resolve destination
  HostImage* bound_color_ = nullptr;
  HostImage* bound_depth_ = nullptr;
  std::unordered_set<std::string> logged_;
  uint32_t swaps_ = 0, draws_ = 0, skipped_ = 0, missing_draws_ = 0, missing_shaders_ = 0, clears_ = 0, resolves_ = 0;
  uint32_t image_count_ = 0, uploads_ = 0, reuploads_ = 0;
  uint32_t sprdiag_files_ = 0;
  uint32_t unmarked_changes_ = 0, sprdiag_lines_ = 0;  // gh: the small-texture hash check and the sprite diagnostics
  std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> sprdiag_;  // per mask texture: the last state hash, lines written
  uint64_t upload_bytes_ = 0;
  uint32_t frame_draws_ = 0, frame_clears_ = 0, frame_resolves_ = 0, frame_skipped_ = 0, frame_missing_ = 0, frame_reuploads_ = 0;
};

class Deko3dGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "deko3d"; }
  // The backend owns the default window's swapchain; the SDK keeps no presenter.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<Deko3dCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

rex::system::IGraphicsSystem* CreateDeko3dGraphicsSystem() { return new Deko3dGraphicsSystem(); }

}  // namespace rex::graphics::deko
#endif  // __SWITCH__
