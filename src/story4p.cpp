// 4-player story mode (experimental), by RedDude (github.com/RedDude/guardian-heroes-recomp-4p-story).
// On by default; off with the environment variable GH_4P=0 (Switch: four_players=0 in settings.txt).
//
// Guest data used here (Guardian Heroes XBLA, retail v2 xex):
//   GlobalWork 0x829CBBA8: +0x14 pad_kind[12] (player -> pad, -1 = empty), +0x2c join flag.
//   Story work 0x829CBFE8 (sub_82495B30): +i*0x24 party block i (+0 EXP, +0xc u16, +0xe level),
//     +0x3d8+i party kind (s8, -1 = none), +(slot+0xd1)*8 character registry (weak handles).
//   The story only uses party 0..2 (0 = AI ally, 1 = P1, 2 = P2). P3/P4 live in the hero slots 3/4
//   (kind at work+0x3d8+3/4, party block 3/4) and take the place of their hero's story NPC; with
//   GH_4P_HEROSLOT=0 they use slots 30/31 (party block 10/11) and the NPC copies stay.
//
// Features:
//   * Join: an unassigned pad pressing START while P2 exists runs the game's own "P2 joins" flow
//     (character select + parameter screen + stage restart) with P2 parked aside; the choice is then
//     moved to party 3 or 4 and P2 is restored.
//   * P3/P4 are created next to P1/P2 by the party routine (sub_824E2970) and their level/EXP are
//     saved with the others at the end of the stage (sub_82499F40).
//   * AI: the game attaches a "brain" object to a character with sub_82479E60(chara, code); the
//     character then ignores its pad and the brain drives it (story NPC heroes, the AI ally).
//     GH_AI=1234 (any of the digits) starts those players under the hero's own brain;
//     F5..F8 toggle P1..P4 while the game window has focus.
//   * The camera (sub_82498808) frames every focus point that players register each frame
//     (sub_8246B2D8 -> sub_82496BB0), so P3/P4 are framed once they are flagged as on screen.
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#if defined(__SWITCH__)
extern "C" volatile int rex_nx_four_players;  // switch/host/input/nx_input_driver.cpp, settings.txt
extern "C" void rex_nx_load_settings(void);
extern "C" volatile uint64_t rex_nx_hook_calls;  // switch/host/switch_nx.cpp
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

extern "C" REX_FUNC(__imp__sub_824E2970);   // create party (stage load)
extern "C" REX_FUNC(__imp__sub_824E2AC0);   // assign story-hero NPC slots (stage+0x1719c..)
extern "C" REX_FUNC(__imp__sub_824E3618);   // in-stage "P2 joins" check
extern "C" REX_FUNC(__imp__sub_82499F40);   // save party progress / checkpoint
extern "C" REX_FUNC(__imp__sub_82498808);   // camera update (once per frame in a stage)
extern "C" REX_FUNC(__imp__sub_82500658);   // parameter screen: next/previous hero index
extern "C" REX_FUNC(__imp__sub_82500710);   // parameter screen: pick a hero index not used by the other panel
extern "C" REX_FUNC(__imp__sub_824892C0);   // modes 5/6: parameter screen
extern "C" REX_FUNC(__imp__sub_824D0EF8);   // route choice menu constructor
extern "C" REX_FUNC(__imp__sub_82443478);   // stage prop base constructor (r3 = object)
extern "C" REX_FUNC(__imp__sub_82443F28);   // stage prop base constructor, 0x8200E570 family
extern "C" REX_FUNC(__imp__sub_82443550);   // prop destructor (vtables 0x8200E0B0..0x8200E530)
extern "C" REX_FUNC(__imp__sub_82443FF8);   // prop destructor (vtables 0x8200E570/0x8200E5B0)
extern "C" REX_FUNC(__imp__sub_82489160);   // mode 4: New Game screen
extern "C" REX_FUNC(__imp__sub_825022D8);   // parameter screen constructor (this, join mode)
extern "C" REX_FUNC(__imp__sub_824E4308);   // continue: downed P1/P2 spend a credit
extern "C" REX_FUNC(__imp__sub_825CF040);   // XamInputGetState(pad, 1, state)
extern "C" REX_FUNC(__imp__sub_8248A1F0);   // top-level mode machine (hook entry in cheats.cpp)
extern "C" REX_FUNC(__imp__sub_824DEA90);   // pause menu update (hook entry in cheats.cpp)
extern "C" REX_FUNC(sub_8243FCE0);  // init create-params struct
extern "C" REX_FUNC(sub_8247ACC0);  // create character (params, kind, slot, x, z)
extern "C" REX_FUNC(sub_824E5758);  // load the sprite pack of a kind now (stage, kind), as for P1/P2
extern "C" REX_FUNC(sub_8241BAF0);  // controller: bind to pad (type 1)
extern "C" REX_FUNC(sub_8249DD68);  // HUD panel for a player
extern "C" REX_FUNC(sub_8247AD30);  // companion for kind 3 (Randy's rabbit)
extern "C" REX_FUNC(sub_82479E60);  // attach brain (chara, code): 0 = none
extern "C" REX_FUNC(sub_82419C38);  // pad connected (pad)
extern "C" REX_FUNC(sub_8241B800);  // pad state pointer (pad); +0 bit 0x400000 = START pressed
extern "C" REX_FUNC(sub_82421B40);  // menu/pause active
extern "C" REX_FUNC(sub_8242B528);  // online session
extern "C" REX_FUNC(sub_82496728);  // set_pad_kind(gw, player, pad)
extern "C" REX_FUNC(__imp__sub_8249CC40);  // HUD: per-panel update (position in +0x888/+0x88c)
extern "C" REX_FUNC(__imp__sub_8249D2C0);  // HUD: layout of all panels (once per frame)
extern "C" REX_FUNC(__imp__sub_8247FE88);  // HUD: "Credit(s) N" sprite quads
extern "C" REX_FUNC(__imp__sub_824E0C10);  // pause menu: constructor (stage+0xa40)
extern "C" REX_FUNC(__imp__sub_824DEA90);  // pause menu: update
extern "C" REX_FUNC(__imp__sub_824E01D0);  // pause menu: draw
extern "C" REX_FUNC(__imp__sub_824A6C40);  // status window: open (vtable +0x40)
extern "C" REX_FUNC(__imp__sub_824A5158);  // window: show again
extern "C" REX_FUNC(__imp__sub_824A5188);  // window: close
extern "C" REX_FUNC(__imp__sub_82468FA8);  // hero: load stats from party block work+slot*0x24
extern "C" REX_FUNC(__imp__sub_82468E90);  // hero: store level/EXP/HP into work+slot*0x24
extern "C" REX_FUNC(__imp__sub_82512B90);  // Han: init (resets work+slot*0x24+0x1b)
extern "C" REX_FUNC(__imp__sub_824C0470);  // window: kind of the pad's player = work+0x3d8+(player+1)
extern "C" REX_FUNC(__imp__sub_8243E510);  // collects party kinds 0..11 to preload
extern "C" REX_FUNC(__imp__sub_824EB8D0);  // stage load: collects party kinds 0..11 to preload
extern "C" REX_FUNC(__imp__sub_8247A530);  // create character from a descriptor (+0 slot, +2 kind)
extern "C" REX_FUNC(__imp__sub_82418928);  // file buffer alloc (size, heap type) -> XMemAlloc
extern "C" REX_FUNC(__imp__sub_82570F70);  // stage 2 actor script (stage, slot, hero kind)
// actor scripts f(stage, slot, hero kind), traced with GH_4P_ACTLOG=1
extern "C" REX_FUNC(__imp__sub_8256EB60);
extern "C" REX_FUNC(__imp__sub_82572588);
extern "C" REX_FUNC(__imp__sub_82576980);
extern "C" REX_FUNC(__imp__sub_8257BEE0);
extern "C" REX_FUNC(__imp__sub_825806D8);
extern "C" REX_FUNC(__imp__sub_82581C00);
extern "C" REX_FUNC(__imp__sub_825841A0);
extern "C" REX_FUNC(__imp__sub_82585348);
extern "C" REX_FUNC(__imp__sub_82587080);
extern "C" REX_FUNC(__imp__sub_8258A3A0);
extern "C" REX_FUNC(__imp__sub_8258CD30);
extern "C" REX_FUNC(__imp__sub_8258F720);
extern "C" REX_FUNC(__imp__sub_82590FF8);
extern "C" REX_FUNC(__imp__sub_82593278);
extern "C" REX_FUNC(__imp__sub_82596758);
extern "C" REX_FUNC(__imp__sub_82598658);
extern "C" REX_FUNC(__imp__sub_8259B9B8);
extern "C" REX_FUNC(__imp__sub_825A3318);
extern "C" REX_FUNC(__imp__sub_825A6200);
extern "C" REX_FUNC(__imp__sub_825AA170);
extern "C" REX_FUNC(__imp__sub_825ACBF8);
extern "C" REX_FUNC(__imp__sub_825AEFE0);
extern "C" REX_FUNC(__imp__sub_825B0348);
extern "C" REX_FUNC(__imp__sub_825B2E88);
extern "C" REX_FUNC(__imp__sub_825B3748);
extern "C" REX_FUNC(__imp__sub_825B5098);
extern "C" REX_FUNC(__imp__sub_825B6AF8);
extern "C" REX_FUNC(__imp__sub_825B82B8);
extern "C" REX_FUNC(__imp__sub_825B9860);
extern "C" REX_FUNC(__imp__sub_825BB090);
extern "C" REX_FUNC(__imp__sub_825BD690);
extern "C" REX_FUNC(__imp__sub_825BE8E0);
extern "C" REX_FUNC(__imp__sub_825C0E98);
extern "C" REX_FUNC(__imp__sub_825C3948);
extern "C" REX_FUNC(__imp__sub_825C4C30);
extern "C" REX_FUNC(__imp__sub_825C6650);
extern "C" REX_FUNC(__imp__sub_825C74C0);
extern "C" REX_FUNC(__imp__sub_825C99E8);
extern "C" REX_FUNC(__imp__sub_825CB1F0);
extern "C" REX_FUNC(__imp__sub_825CD678);
extern "C" REX_FUNC(__imp__sub_8248A1F0);  // top-level mode dispatcher (once per frame)
extern "C" REX_FUNC(sub_8249A868);  // reset story work (work)
extern "C" REX_FUNC(__imp__sub_8249A868);
extern "C" REX_FUNC(sub_824968F0);  // put hero in party (work, slot, kind): kind + level 1 block
extern "C" REX_FUNC(__imp__sub_824968F0);
extern "C" REX_FUNC(__imp__sub_82501690);  // parameter screen panel: read its block (after a join)
extern "C" REX_FUNC(__imp__sub_82497950);  // restore the party snapshot (owner): load / continue
extern "C" REX_FUNC(__imp__sub_8249A638);  // write "System Data" (owner snapshot included) if changed
extern "C" REX_FUNC(__imp__sub_8249B198);  // read "System Data" into the owner
extern "C" REX_FUNC(__imp__sub_824380D0);  // write a content file (name, ..., buffer, size)
extern "C" REX_FUNC(__imp__sub_8256E290);  // stage 1 exit: P1/P2 past x 976 walk out (stage)
extern "C" REX_FUNC(sub_82497898);  // New Game exit step (owner 0x829caaf0)
extern "C" REX_FUNC(sub_82499F40);  // save party progress (owner, flag)
extern "C" REX_FUNC(sub_826A3900);  // operator new
extern "C" REX_FUNC(sub_824B2ED0);  // window base constructor
extern "C" REX_FUNC(sub_824B57E8);  // status window setup (window, player index), in-stage flavour
extern "C" REX_FUNC(sub_825122F0);  // window destructor (window, 1 = free)
extern "C" REX_FUNC(sub_824A5218);  // window update (vtable +4)
extern "C" REX_FUNC(sub_824AFC38);  // status window draw (vtable +0x30)

namespace {

constexpr uint32_t kGlobalWork = 0x829CBBA8;
constexpr uint32_t kPadKind = kGlobalWork + 0x14;  // s8[12]
constexpr uint32_t kWork = 0x829CBFE8;
constexpr uint32_t kKinds = kWork + 0x3d8;  // s8[32], indexed by registry slot
constexpr uint32_t kBlock = 0x24;
constexpr uint32_t kCamera = 0x829B2498;

inline uint8_t R8(uint8_t* b, uint32_t a) { return b[a]; }
inline int8_t S8(uint8_t* b, uint32_t a) { return (int8_t)b[a]; }
inline void W8(uint8_t* b, uint32_t a, uint8_t v) { b[a] = v; }
inline uint16_t R16(uint8_t* b, uint32_t a) {
  uint16_t v; std::memcpy(&v, b + a, 2); return __builtin_bswap16(v);
}
inline void W16(uint8_t* b, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v); std::memcpy(b + a, &v, 2);
}
inline uint32_t R32(uint8_t* b, uint32_t a) {
  uint32_t v; std::memcpy(&v, b + a, 4); return __builtin_bswap32(v);
}
inline void W32(uint8_t* b, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v); std::memcpy(b + a, &v, 4);
}
inline void WF(uint8_t* b, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); W32(b, a, v); }

// Weak handle (8 bytes): +4 -> ref record whose +0 is the object (sub_8242AF48).
uint32_t Deref(uint8_t* b, uint32_t handle) {
  uint32_t r = R32(b, handle + 4);
  return r ? R32(b, r) : 0;
}
uint32_t Chara(uint8_t* b, uint32_t slot) { return Deref(b, kWork + (slot + 0xd1) * 8); }

// Calls into guest code from a hook, with its own frame below the hooked function's stack pointer.
struct Guest {
  rex::CallFrame f;
  uint8_t* base;
  Guest(PPCContext& parent, uint8_t* b) : f(parent), base(b) {
    f.ctx.r1.u32 = parent.r1.u32 - 0x200;
    W32(base, f.ctx.r1.u32, parent.r1.u32);  // back chain
  }
  uint32_t stack(uint32_t off) const { return f.ctx.r1.u32 + 0x100 + off; }  // scratch area
  uint32_t operator()(PPCFunc* fn, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
                      uint64_t e = 0) {
    const uint32_t sp = f.ctx.r1.u32;
    f.ctx.r3.u64 = a; f.ctx.r4.u64 = b; f.ctx.r5.u64 = c; f.ctx.r6.u64 = d; f.ctx.r7.u64 = e;
    fn(f.ctx, base);
    f.ctx.r1.u32 = sp;
    return f.ctx.r3.u32;
  }
};

const char* Env(const char* n) { const char* s = std::getenv(n); return (s && *s) ? s : nullptr; }

bool Enabled() {
#if defined(__SWITCH__)
  rex_nx_hook_calls = rex_nx_hook_calls + 1;  // the [dk] perf line: every hook asks here first
#endif
  static int e = -1;
  if (e < 0) {
#if defined(__SWITCH__)
    rex_nx_load_settings();
    e = rex_nx_four_players ? 1 : 0;
#else
    const char* s = Env("GH_4P");
    e = (s && *s == '0') ? 0 : 1;
#endif
  }
  return e == 1;
}

// work+0x3ac: 0 = story (the HUD/pause code uses the same test).
inline bool Story(uint8_t* base) { return R8(base, 0x829CBFE8 + 0x3ac) == 0; }

// The original credits (finite, shown on the HUD) unless GH_4P_CREDITS=1.
bool InfiniteCredits() {
  static int v = -1;
  if (v < 0) { const char* s = std::getenv("GH_4P_CREDITS"); v = (s && *s == '1') ? 1 : 0; }
  return v == 1;
}

int MaxPlayers() {
  const char* s = Env("GH_4P_PLAYERS");
  int n = s ? std::atoi(s) : 4;
  return n < 2 ? 2 : (n > 4 ? 4 : n);
}

// ---------------------------------------------------------------------------------------------
// Extra players
// ---------------------------------------------------------------------------------------------

// P3/P4 take the place of their hero's story NPC (as P1/P2 do) unless GH_4P_HEROSLOT=0.
bool HeroSlots() {
  static int v = -1;
  if (v < 0) { const char* s = std::getenv("GH_4P_HEROSLOT"); v = (s && *s == '0') ? 0 : 1; }
  return v == 1;
}

// Registry slot of player p (2 = P3, 3 = P4). The story keeps slots 1..5 for the five heroes and the
// stage scripts keep per-actor state in arrays of 24 entries indexed by slot, so a P3/P4 that stands
// in for a hero in cut scenes must sit in a hero slot: 3/4 (the NPC heroes then use the free one).
// Without hero slots, 30/31 (never used by the stages).
uint32_t SlotFor(int p) {
  const char* es = Env(p == 2 ? "GH_4P_SLOT2" : "GH_4P_SLOT3");
  if (es) return (uint32_t)std::atoi(es);
  if (HeroSlots()) return p == 2 ? 3u : 4u;
  return p == 2 ? 30u : 31u;
}
// The game indexes work+0x3d8 (kind, 32 entries) by registry slot ("is the character in slot s a party
// member?"; stage scripts ask it for the NPC heroes in slots 3..5), and the party blocks
// work+i*0x24 (12 entries) by slot too. P3/P4 keep their kind at their own slots (30/31) and their
// block at 10/11 (no hero ever uses slots 6..11); see the shadow block hooks below.
inline uint32_t KindOf(int p) { return kKinds + SlotFor(p); }
// Party block: the slot's own block when the slot has one (0..11), else 10/11.
inline uint32_t BlockOf(int p) {
  const uint32_t slot = SlotFor(p);
  return kWork + (slot < 12 ? slot : (p == 2 ? 10u : 11u)) * kBlock;
}

uint32_t g_stage;  // stage object (seen in sub_824E2AC0)
// NPC hero moved by sub_824E2AC0's hook: g_npc_home[new slot] = original slot (0 = not moved).
uint8_t g_npc_home[24];
int g_kind[4] = {-1, -1, -1, -1};  // kind spawned this stage for P3/P4 (diagnostics, hero slots)
// Last progress seen on P3/P4's character while it was alive (level/EXP). The end-of-stage save
// (sub_82499F40) runs during the closing scene, when +0xDC is already 0 or the character is gone,
// so it uses this copy instead of reading the character then.
struct Prog { int kind = -1; uint32_t exp = 0; uint16_t d6 = 0; uint8_t lv = 0; } g_prog[4];
uint32_t g_cont[2][2];  // P3/P4: continue state, timer (see the sub_824E4308 hook)
void ClearRoster();     // saved heroes of P2/P3/P4 (see "Progress of P2/P3/P4 across saves")

// First pad not assigned to P1..P4 (or -1).
int FreePad(uint8_t* base) {
  int used = 0;
  for (int i = 0; i < 4; i++) {
    int8_t k = S8(base, kPadKind + i);
    if (k >= 0) used |= 1 << k;
  }
  for (int c = 0; c < 4; c++)
    if (!(used & (1 << c))) return c;
  return -1;
}

void SpawnExtra(Guest& g, int p) {
  uint8_t* base = g.base;
  const int kind = S8(base, KindOf(p));
  int8_t pad = S8(base, kPadKind + p);
  if (pad < 0) {
    pad = (int8_t)FreePad(base);
    if (pad < 0) { REXKRNL_WARN("story4p: no free pad for P{}", p + 1); return; }
    W8(base, kPadKind + p, (uint8_t)pad);
  }
  const uint32_t slot = SlotFor(p);
  const uint32_t blk = BlockOf(p);
  const uint32_t params = g.stack(0);

  // A stage loads the sprite packs of party 0..2 and of its own cast (the five heroes only where
  // they appear as NPCs); load P3/P4's now, or they (and later creations) find no animation data.
  g(sub_824E5758, 0, (uint64_t)(int64_t)kind);
  // Han: with work+0x3c4 bit 1 the game uses kind 7 ("hanu" pack) instead of 2 ("han"); the loader
  // above already swaps it, and the party routine (sub_824E2970) creates kind 7 too. Creating kind 2
  // here crashed in sub_82425830 (no animation data) when P3/P4 joined as Han mid-stage.
  int make = kind;
  if (kind == 2 && (R32(base, kWork + 0x3c4) & 2)) make = 7;
  g(sub_8243FCE0, params);
  W8(base, params + 1, 0);
  W16(base, params + 8, 0);                   // no brain: driven by its pad
  W8(base, params + 6, R8(base, blk + 0xe));  // level
  const uint32_t ch = g(sub_8247ACC0, params, (uint64_t)make, slot, 0x400000,
                        p == 2 ? 0x300000 : 0x900000);
  if (!ch) { REXKRNL_WARN("story4p: create failed for P{}", p + 1); return; }

  W8(base, ch + 0x140, 0xFF);
  W8(base, ch + 0xE0, (uint8_t)p);  // player index
  W8(base, ch + 0xDC, 1);           // is a player
  // +0xEF bit 0x40 = "has entered the camera window": the screen-edge clamp and the camera focus
  // (sub_8246B2D8) only engage after it, and P3/P4 may spawn outside the view.
  W8(base, ch + 0xEF, R8(base, ch + 0xEF) | 0x40);
  if (R8(base, blk + 0xe)) {  // carry EXP the way sub_82499F40 stores it
    W32(base, ch + 0xC0, R32(base, blk + 0));
    W16(base, ch + 0xD6, R16(base, blk + 0xc));
  }
  g(sub_8241BAF0, ch + 0x478);  // same order as the party routine: bind, then set the pad
  W8(base, ch + 0x48F, (uint8_t)pad);
  g(sub_8249DD68, ch);
  if (kind == 3) {
    uint32_t pet = g(sub_8247AD30, ch, R32(base, ch + 12), R32(base, ch + 20));
    if (pet) W8(base, pet + 0x140, 0xFF);
  }
  g_kind[p] = kind;
  REXKRNL_INFO("story4p: P{} kind {} level {} slot {} pad {} chara {:08X}", p + 1, kind,
               R8(base, blk + 0xe), slot, (int)pad, ch);
}

int AutoKind(uint8_t* base, int p) {
  const char* s = Env(p == 2 ? "GH_4P_KIND2" : "GH_4P_KIND3");
  if (s) return std::atoi(s);
  int used = 0;
  for (uint32_t slot : {1u, 2u, SlotFor(2), SlotFor(3)}) {
    int k = S8(base, kKinds + slot);
    if (k == 7) k = 2;
    if (k >= 0 && k < 31) used |= 1 << k;
  }
  for (int k = 1; k <= 5; k++)
    if (!(used & (1 << k))) return k;
  return 1;
}

// ---------------------------------------------------------------------------------------------
// Join: P3/P4 borrow the P2 join flow
// ---------------------------------------------------------------------------------------------

struct Parked {
  bool active = false;
  int player = -1;     // 2 = P3, 3 = P4
  int pad = -1;        // joining pad
  int8_t p2_pad = -1;  // P2's pad_kind
  int8_t p2_kind = -1;
  uint8_t p2_block[kBlock] = {};
} g_join;

void Park(uint8_t* base, int player, int pad) {
  g_join.active = true;
  g_join.player = player;
  g_join.pad = pad;
  g_join.p2_pad = S8(base, kPadKind + 1);
  g_join.p2_kind = S8(base, kKinds + 2);
  std::memcpy(g_join.p2_block, base + kWork + 2 * kBlock, kBlock);
  W8(base, kKinds + 2, 0xFF);  // the select screen sees an empty P2 seat
  std::memset(base + kWork + 2 * kBlock, 0, kBlock);
}

// Called before the party is created: move what was chosen for "P2" to the joining player.
void Unpark(uint8_t* base) {
  if (!g_join.active) return;
  const int p = g_join.player;
  const int8_t chosen = S8(base, kKinds + 2);
  if (chosen >= 0) {
    W8(base, KindOf(p), (uint8_t)chosen);
    std::memcpy(base + BlockOf(p), base + kWork + 2 * kBlock, kBlock);
    W8(base, kPadKind + p, (uint8_t)g_join.pad);
    REXKRNL_INFO("story4p: P{} joined with kind {} (pad {})", p + 1, (int)chosen, g_join.pad);
  } else {
    REXKRNL_INFO("story4p: P{} join cancelled", p + 1);
  }
  W8(base, kKinds + 2, (uint8_t)g_join.p2_kind);
  std::memcpy(base + kWork + 2 * kBlock, g_join.p2_block, kBlock);
  W8(base, kPadKind + 1, (uint8_t)g_join.p2_pad);
  g_join.active = false;
}

// Parameter screen hero index (panel +0x81c) -> kind: Han, Randy, Ginjirou, Nicole, Serena.
constexpr int kPanelKind[5] = {2, 3, 4, 5, 1};

// While P3/P4 join through the P2 panel, the heroes of P2/P3/P4 are taken too (the screen itself
// only avoids P1's).
bool PanelTaken(uint8_t* base, int index) {
  if (index < 0 || index > 4) return false;
  const int kind = kPanelKind[index];
  auto same = [&](int k) { return k >= 0 && (k == 7 ? 2 : k) == kind; };
  if (same(g_join.p2_kind)) return true;
  for (int p = 2; p < 4; p++)
    if (p != g_join.player && same(S8(base, KindOf(p)))) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------------------------

// Brain code (sub_82479E60) of each hero, from the brain classes that live next to the hero's
// own class: 2 Serena, 3 Han, 4 Randy, 0x21 Ginjirou, 0x28 Nicole.
int BrainFor(int kind) {
  switch (kind) {
    case 1: return 0x02;
    case 2: case 7: return 0x03;
    case 3: return 0x04;
    case 4: return 0x21;
    case 5: return 0x28;
    default: return 0x02;
  }
}

bool g_ai[4];
uint32_t g_brain[4];  // brain we attached (to detach only our own)
int g_retry[4];
uint32_t g_frame;

void InitAi() {
  static bool done = false;
  if (done) return;
  done = true;
  if (const char* s = Env("GH_AI"))
    for (; *s; s++)
      if (*s >= '1' && *s <= '4') g_ai[*s - '1'] = true;
}

uint32_t FindPlayer(uint8_t* base, int p, uint32_t* slot_out = nullptr) {
  for (uint32_t s = 0; s < 32; s++) {
    uint32_t ch = Chara(base, s);
    if (ch && R8(base, ch + 0xDC) == 1 && S8(base, ch + 0xE0) == p) {
      if (slot_out) *slot_out = s;
      return ch;
    }
  }
  return 0;
}

// Debug commands: F9 = enemies down to 1 HP, F10 = clear the stage (go to the next one).
bool g_kill_req, g_skip_req;

void PollKeys() {
#ifdef _WIN32
  static bool prev[4], prev_dbg[2];
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  const bool focus = pid == GetCurrentProcessId();
  for (int i = 0; i < 4; i++) {
    bool down = focus && (GetAsyncKeyState(VK_F5 + i) & 0x8000);
    if (down && !prev[i]) {
      g_ai[i] = !g_ai[i];
      REXKRNL_INFO("story4p: AI P{} {}", i + 1, g_ai[i] ? "on" : "off");
    }
    prev[i] = down;
  }
  for (int i = 0; i < 2; i++) {
    bool down = focus && (GetAsyncKeyState(VK_F9 + i) & 0x8000);
    if (down && !prev_dbg[i]) {
      (i == 0 ? g_kill_req : g_skip_req) = true;
      REXKRNL_INFO("story4p: debug {}", i == 0 ? "enemies to 1 HP" : "clear stage");
    }
    prev_dbg[i] = down;
  }
#endif
}

// Enemies = characters on another team (+0xde, -1 = none) than P1. HP 1: the next hit downs them
// through the game's own damage, death and EXP code.
int WeakenEnemies(uint8_t* base) {
  uint32_t p1 = 0;
  for (uint32_t s = 0; s < 32 && !p1; s++) {
    uint32_t ch = Chara(base, s);
    if (ch && R8(base, ch + 0xDC) == 1) p1 = ch;
  }
  if (!p1) return 0;
  const int8_t team = S8(base, p1 + 0xDE);
  int n = 0;
  for (uint32_t s = 0; s < 32; s++) {
    uint32_t ch = Chara(base, s);
    if (!ch || R8(base, ch + 0xDC)) continue;
    const int8_t t = S8(base, ch + 0xDE);
    const int16_t hp = (int16_t)R16(base, ch + 0xCA);
    if (t == team || t == -1 || hp <= 1) continue;
    W16(base, ch + 0xCA, 1);
    n++;
  }
  return n;
}

// GH_AUTO_KILL=1: enemies always at 1 HP. GH_AUTO_SKIP=N: clear each stage after N seconds of play.
void AutoDebug(uint8_t* base) {
  static int kill = -1, skip = -1;
  static uint32_t played;
  if (kill < 0) {
    const char* k = Env("GH_AUTO_KILL");
    const char* s = Env("GH_AUTO_SKIP");
    kill = (k && *k == '1') ? 1 : 0;
    skip = s ? std::atoi(s) : 0;
  }
  const bool event = R8(base, kWork + 0x3b4) != 0;
  if (g_kill_req || (kill && !event)) {
    int n = WeakenEnemies(base);
    if (g_kill_req) REXKRNL_INFO("story4p: {} enemies at 1 HP", n);
    g_kill_req = false;
  }
  static int played_stage = -1;  // the count restarts with each stage (it used to carry over)
  const int stage_now = R8(base, kWork + 0x3af);
  if (stage_now != played_stage) { played_stage = stage_now; played = 0; }
  if (!event) played++;
  if (skip > 0 && played >= (uint32_t)skip * 60) {
    played = 0;
    g_skip_req = true;
    REXKRNL_INFO("story4p: auto clear after {} s", skip);
  }
}

// Last frame each player was seen (diagnostics: a player that vanishes outside of an event).
struct Seen { uint32_t ch, slot; int hp, x, st; bool brain; } g_seen[4];

void ReportLost(uint8_t* base, int p) {
  const Seen& s = g_seen[p];
  const uint32_t now = Chara(base, s.slot);
  std::string what = "slot empty";
  if (now)
    what = fmt::format("slot holds {:08X} kind {} dc {} e0 {} hp {} st {:02x}", now, R8(base, now + 0x54),
                       R8(base, now + 0xDC), S8(base, now + 0xE0), (int16_t)R16(base, now + 0xCA),
                       R8(base, now + 0xDF));
  REXKRNL_WARN("story4p: P{} lost (event {}): last {:08X} slot {} hp {} x {} st {:02x}{} -> {}", p + 1,
               R8(base, kWork + 0x3b4), s.ch, s.slot, s.hp, s.x, s.st, s.brain ? " ai" : "", what);
}

// ---------------------------------------------------------------------------------------------
// AI players standing still: walk them on
// ---------------------------------------------------------------------------------------------
// The hero brains only fight; with no enemy in view they stand where they are, and many stages only
// go on when the players walk right (the camera scrolls and the next wave comes). An AI player that
// stays within a few pixels for GH_AI_IDLE seconds (default 5) with no enemy in view walks right on
// its pad (brain released meanwhile). If the camera does not scroll in 3 s, it walks left instead
// (stages that go left) for up to 4 s. It hands back to the brain when an enemy shows up, when the
// camera stops after having scrolled, or when the time runs out. GH_AI_IDLE=0 turns it off.
struct Idle {
  int32_t anchor = 0;     // x (pixels) where the player was last seen moving
  uint32_t since = 0;     // frame when it got there
  int dir = 0;            // +1 walking right, -1 left, 0 = brain
  uint32_t dir_since = 0;
  int32_t cam = 0;        // camera centre when the walk started / last scrolled
  bool scrolled = false;
  uint32_t target = 0;    // breakable prop being hit while walking (0 = none)
  uint32_t target_since = 0;
  uint32_t jump_until = 0;  // jumping over a prop that did not break (frame)
  int flips = 0;          // turns without the camera scrolling
  int32_t jump_x0 = 0, jump_top = 0;  // diagnostics
} g_idle[4];

// Breakable stage props (barrels, crates, stools...): placed by sub_824D3EC8/sub_824D3AF8, removed by
// their destructors. Breakable = vtable 0x8200E0F0..0x8200E4F0 or 0x8200E5B0 (the classes that set
// HP; 0x8200E0B0/0x8200E570 are fixed scenery, 0x8200E530 are pickups), HP at +0xCA.
constexpr int kMaxProps = 512;
uint32_t g_props[kMaxProps];       // every stage prop object (filtered by vtable when used)
uint32_t g_props_skip[kMaxProps];  // frame until which a prop is left alone (jumped over)

// Shared walking direction (+1 right, -1 left): the way that scrolled the camera last, for every AI
// player. Back to right on each new stage.
int g_way = 1;

bool BreakableVt(uint32_t vt) {
  return (vt >= 0x8200E0F0 && vt <= 0x8200E4F0 && (vt & 0x3F) == 0x30) || vt == 0x8200E5B0;
}

void AddProp(uint8_t* base, uint32_t obj) {
  (void)base;
  if (!obj) return;
  for (uint32_t& o : g_props) if (o == obj) return;
  for (int i = 0; i < kMaxProps; i++)
    if (!g_props[i]) { g_props[i] = obj; g_props_skip[i] = 0; return; }
}

void RemoveProp(uint32_t obj) {
  for (int i = 0; i < kMaxProps; i++)
    if (g_props[i] == obj) { g_props[i] = 0; g_props_skip[i] = 0; }
}

// A breakable prop with HP left right ahead of x (pixels) in direction dir, in the same depth lane.
uint32_t PropAhead(uint8_t* base, int32_t x, int32_t z, int dir) {
  for (int i = 0; i < kMaxProps; i++) {
    const uint32_t o = g_props[i];
    if (!o || g_props_skip[i] > g_frame) continue;
    if (!BreakableVt(R32(base, o))) continue;
    if ((int16_t)R16(base, o + 0xCA) <= 0) continue;
    const int32_t ox = (int32_t)R32(base, o + 0xC) >> 16, oz = (int32_t)R32(base, o + 0x14) >> 16;
    const int32_t ahead = (ox - x) * dir;
    if (ahead >= -8 && ahead <= 72 && oz - z >= -40 && oz - z <= 40) return o;
  }
  return 0;
}

void SkipProp(uint32_t obj) {
  for (int i = 0; i < kMaxProps; i++)
    if (g_props[i] == obj) g_props_skip[i] = g_frame + 180;  // 3 s, then hit it again if still there
}

int IdleSeconds() {
  static int v = -2;
  if (v == -2) { const char* s = Env("GH_AI_IDLE"); v = s ? std::atoi(s) : 5; }
  return v;
}

// An enemy (another team than P1, alive, not a player) inside the camera window.
bool EnemyInView(uint8_t* base) {
  uint32_t p1 = 0;
  for (uint32_t s = 0; s < 32 && !p1; s++) {
    uint32_t ch = Chara(base, s);
    if (ch && R8(base, ch + 0xDC) == 1) p1 = ch;
  }
  if (!p1) return false;
  const int8_t team = S8(base, p1 + 0xDE);
  const int32_t wl = (int32_t)R32(base, kCamera + 0x318), wr = (int32_t)R32(base, kCamera + 0x31C);
  for (uint32_t s = 0; s < 32; s++) {
    uint32_t ch = Chara(base, s);
    if (!ch || R8(base, ch + 0xDC)) continue;
    const int8_t t = S8(base, ch + 0xDE);
    if (t == team || t == -1 || (int16_t)R16(base, ch + 0xCA) <= 0) continue;
    const int32_t x = (int32_t)R32(base, ch + 0xC);
    if (x >= wl && x <= wr) return true;
  }
  return false;
}

void StopWalk(int p, const char* why) {
  if (g_idle[p].dir) REXKRNL_INFO("story4p: AI P{} walk stops ({})", p + 1, why);
  g_idle[p].dir = 0;
  g_idle[p].target = 0;
  g_idle[p].jump_until = 0;
  g_idle[p].since = g_frame;
  g_retry[p] = 0;
}

// Per frame for an AI player that is up; returns true while it walks (pad-driven).
bool UpdateIdle(uint8_t* base, int p, uint32_t ch) {
  Idle& w = g_idle[p];
  const int secs = IdleSeconds();
  const int32_t x = (int32_t)R32(base, ch + 0xC) >> 16;
  const int32_t cam = (int32_t)R32(base, kCamera + 0x304) >> 16;
  static int way_stage = -1;
  const int stage = R8(base, kWork + 0x3af);
  if (stage != way_stage) { way_stage = stage; g_way = 1; }
  if (secs <= 0 || R8(base, kWork + 0x3b4) || !Story(base)) {
    if (w.dir) StopWalk(p, "event");
    w.anchor = x;
    w.since = g_frame;
    return false;
  }
  if (!w.dir) {
    if (x < w.anchor - 32 || x > w.anchor + 32) { w.anchor = x; w.since = g_frame; return false; }
    if (g_frame - w.since < (uint32_t)secs * 60 || EnemyInView(base)) return false;
    w.dir = g_way;
    w.dir_since = g_frame;
    w.cam = cam;
    w.scrolled = false;
    w.flips = 0;
    REXKRNL_INFO("story4p: AI P{} idle {} s at x {}: walking {}", p + 1, secs, x, w.dir > 0 ? "right" : "left");
    return true;
  }
  if (EnemyInView(base)) { StopWalk(p, "enemy in view"); w.anchor = x; return false; }
  if (w.dir != g_way) {  // another player found the way: follow it
    w.dir = g_way;
    w.dir_since = g_frame;
    w.cam = cam;
    w.scrolled = false;
    w.target = 0;
  }
  // A breakable prop in the way: stop and hit it (the pad taps X, see the pad hook). If it is still
  // there after 1 s, jump over it; it is left alone for 3 s, then hit again if it is still in the way.
  // The walk timers wait meanwhile.
  if (w.target && (!BreakableVt(R32(base, w.target)) || (int16_t)R16(base, w.target + 0xCA) <= 0)) {
    REXKRNL_INFO("story4p: AI P{} broke the prop", p + 1);
    w.target = 0;
  }
  if (!w.target && w.jump_until <= g_frame) {
    const int32_t z = (int32_t)R32(base, ch + 0x14) >> 16;
    if (uint32_t o = PropAhead(base, x, z, w.dir)) {
      w.target = o;
      w.target_since = g_frame;
      REXKRNL_INFO("story4p: AI P{} hits a prop at x {} (vt {:08X} hp {})", p + 1,
                   (int32_t)R32(base, o + 0xC) >> 16, R32(base, o), (int16_t)R16(base, o + 0xCA));
    }
  }
  if (w.target) {
    if (g_frame - w.target_since > 60) {
      REXKRNL_INFO("story4p: AI P{} jumps over the prop", p + 1);
      SkipProp(w.target);
      w.target = 0;
      w.jump_until = g_frame + 45;
    } else {
      w.dir_since = g_frame;
      w.since = g_frame;
      return true;
    }
  }
  if (w.jump_until > g_frame) {
    w.dir_since = g_frame;  // the jump is not "walking without scrolling"
    const int32_t y = (int32_t)R32(base, ch + 0x10) >> 16;
    if (w.jump_until == g_frame + 44) { w.jump_x0 = x; w.jump_top = y; }
    if (y < w.jump_top) w.jump_top = y;
    if (w.jump_until == g_frame + 1)
      REXKRNL_INFO("story4p: AI P{} jump: x {} -> {}, height {}, st {:02x}", p + 1, w.jump_x0, x, -w.jump_top,
                   R8(base, ch + 0xDF));
  }
  if (cam > w.cam + 8 || cam < w.cam - 8) {  // the camera follows: keep going while it scrolls
    w.cam = cam;
    w.dir_since = g_frame;
    w.scrolled = true;
    w.flips = 0;
    if (g_way != w.dir) REXKRNL_INFO("story4p: way is now {} (P{})", w.dir > 0 ? "right" : "left", p + 1);
    g_way = w.dir;
  }
  const uint32_t held = g_frame - w.dir_since;
  if (held >= 180) {
    if (w.scrolled) { StopWalk(p, "camera stopped"); w.anchor = x; return false; }
    if (++w.flips > 1) {
      StopWalk(p, "neither way scrolled");
      g_way = 1;
      w.anchor = x;
      return false;
    }
    g_way = -w.dir;  // shared: everyone turns
    w.dir = g_way;
    w.dir_since = g_frame;
    w.cam = cam;
    REXKRNL_INFO("story4p: AI P{} did not scroll: everyone walks {}", p + 1, w.dir > 0 ? "right" : "left");
  }
  return true;
}

void UpdateAi(Guest& g) {
  uint8_t* base = g.base;
  for (int p = 0; p < 4; p++) {
    uint32_t slot = 0;
    uint32_t ch = FindPlayer(base, p, &slot);
    if (!ch) {
      // During events the players read +0xdc = 0 and are not found; the brain we attached stays on
      // the character, so g_brain is kept (it used to be cleared here, and a downed AI player then
      // kept a brain we no longer recognised: the game removed it like an NPC).
      if (g_seen[p].ch) ReportLost(base, p);
      g_seen[p].ch = 0;
      if (g_idle[p].dir) StopWalk(p, "player not found");
      continue;
    }
    g_seen[p] = {ch, slot, (int16_t)R16(base, ch + 0xCA), (int32_t)R32(base, ch + 0xC) >> 16,
                 R8(base, ch + 0xDF), Deref(base, ch + 0x174) != 0};
    uint32_t brain = Deref(base, ch + 0x174);
    // HP (+0xca) below zero = down. The game's continue (sub_824E4308) skips characters with a
    // brain, and a downed character with a brain is removed like an NPC: hand it back to its pad
    // until the continue has revived it. Any brain goes (also one the stage put on it).
    const bool down = R16(base, ch + 0xCA) > 0x7fff;
    static int downlog = -1;
    if (downlog < 0) downlog = Env("GH_4P_DOWNLOG") ? 1 : 0;
    if (downlog && down && g_stage) {
      // The state the continue tests (vtable +0x1c), through a guest call.
      PPCFunc* fn = rex::runtime::ResolveIndirectFunction(R32(base, R32(base, ch) + 0x1c));
      const uint32_t st = fn ? g(fn, ch) : 0xFFFF;
      const int seat = p < 2 ? p : p - 2;
      REXKRNL_INFO("story4p: down P{} f{} hp {} st {:#x} df {:02x} 140 {:02x} 4e {} cont {} {}", p + 1, g_frame,
                   (int16_t)R16(base, ch + 0xCA), st, R8(base, ch + 0xDF), R8(base, ch + 0x140),
                   (int16_t)R16(base, ch + 0x4E),
                   p < 2 ? R32(base, g_stage + 0x171b8 + seat * 4) : g_cont[seat][0],
                   p < 2 ? R32(base, g_stage + 0x171c0 + seat * 4) : g_cont[seat][1]);
    }
    if (down) {
      if (brain) {
        g(sub_82479E60, ch, 0);
        REXKRNL_INFO("story4p: P{} down, brain {:08X}{} released for the continue", p + 1, brain,
                     brain == g_brain[p] ? " (AI)" : "");
      }
      g_brain[p] = 0;
      g_retry[p] = 30;
      if (g_idle[p].dir) StopWalk(p, "down");
      continue;
    }
    if (g_ai[p] && UpdateIdle(base, p, ch)) {
      if (brain && brain == g_brain[p]) g(sub_82479E60, ch, 0);  // the pad drives it meanwhile
      if (brain == g_brain[p]) g_brain[p] = 0;
      continue;
    }
    if (!g_ai[p] && g_idle[p].dir) StopWalk(p, "AI off");
    if (g_ai[p]) {
      if (brain) continue;  // ours, or a story script is driving this character
      if (g_retry[p] > 0) { g_retry[p]--; continue; }
      const int kind = R8(base, ch + 0x54);
      const char* forced = Env("GH_AI_CODE");
      const int code = forced ? (int)std::strtol(forced, nullptr, 0) : BrainFor(kind);
      g(sub_82479E60, ch, (uint64_t)code);
      g_brain[p] = Deref(base, ch + 0x174);
      g_retry[p] = 30;  // if the brain ends itself, try again in half a second
      REXKRNL_INFO("story4p: AI P{} kind {} brain {:#x} -> {:08X}", p + 1, kind, code, g_brain[p]);
    } else if (brain && brain == g_brain[p]) {
      g(sub_82479E60, ch, 0);
      g_brain[p] = 0;
      REXKRNL_INFO("story4p: AI P{} released", p + 1);
    }
  }
}

void LogPlayers(uint8_t* base) {
  static int every = -1;
  if (every < 0) { const char* s = Env("GH_4P_LOG"); every = s ? std::atoi(s) : 0; }
  if (every <= 0 || g_frame % (uint32_t)(every * 60) != 0) return;
  const int cx = (int32_t)R32(base, kCamera + 0x304) >> 16;
  std::string line = fmt::format("story4p: f{} stage {} cam {} event {}", g_frame,
                                 (int)R8(base, kWork + 0x3af), cx, (int)R8(base, kWork + 0x3b4));
  for (int p = 0; p < 4; p++) {
    uint32_t slot = 0;
    uint32_t ch = FindPlayer(base, p, &slot);
    if (!ch) continue;
    line += fmt::format(" | P{} k{} s{} x{} hp{} st{:02x}{}", p + 1, R8(base, ch + 0x54), slot,
                        (int32_t)R32(base, ch + 0xC) >> 16, (int16_t)R16(base, ch + 0xCA),
                        R8(base, ch + 0xDF), Deref(base, ch + 0x174) ? " ai" : "");
  }
  // During events: the stage scripts' per-actor state (stage+0x170dc+slot*4) and counter (+0x1713c).
  if (g_stage && R8(base, kWork + 0x3b4)) {
    line += " | acts";
    for (uint32_t s = 0; s < 13; s++)
      line += fmt::format(" {}:{}/{}", s, (int32_t)R32(base, g_stage + 0x170dc + s * 4),
                          (int32_t)R32(base, g_stage + 0x1713c + s * 4));
  }
  REXKRNL_INFO("{}", line);
}

}  // namespace

// Party creation: P3/P4 (party 3/4) are created after the original P1/P2.
REX_HOOK_RAW(sub_824E2970) {
  if (!Enabled()) { __imp__sub_824E2970(ctx, base); return; }  // mod off: the game's own function
  if (!Enabled()) { __imp__sub_824E2970(ctx, base); return; }
  if (g_join.active) {
    // A join from inside a stage: the restart loaded the sprite packs while P2's seat was parked
    // (empty), so P2's hero pack is missing unless the stage cast happens to use it. Load it now, or
    // P2 is created without animation data and the game crashes (sub_82425830, read of 0x442).
    const int8_t p2 = g_join.p2_kind;
    Unpark(base);
    if (p2 >= 0) {
      PPCContext c = ctx;
      Guest g(c, base);
      g(sub_824E5758, 0, (uint64_t)(int64_t)p2);
    }
  }
  if (Env("GH_4P_AUTO") && S8(base, kKinds + 2) >= 0) {  // test mode: P3/P4 without joining
    for (int p = 2; p < MaxPlayers(); p++) {
      if (S8(base, KindOf(p)) >= 0) continue;
      W8(base, KindOf(p), (uint8_t)AutoKind(base, p));
      // Fresh block with P1's level/EXP (attribute points belong to P1's hero).
      const uint32_t blk = BlockOf(p), p1 = kWork + 1 * kBlock;
      std::memset(base + blk, 0, kBlock);
      W32(base, blk + 0, R32(base, p1 + 0));
      W16(base, blk + 0xc, R16(base, p1 + 0xc));
      W8(base, blk + 0xe, R8(base, p1 + 0xe));
    }
  }
  PPCContext saved = ctx;
  __imp__sub_824E2970(ctx, base);
  for (auto& k : g_kind) k = -1;
  Guest g(saved, base);
  for (int p = 2; p < MaxPlayers(); p++)
    if (S8(base, KindOf(p)) >= 0) SpawnExtra(g, p);
}

// Story work reset (New Game, loading a save, continuing after a game over): the kinds go to -1, so
// P3/P4 leave the party (as P2 does: the snapshot restore sub_82497950 also empties P2's seat); free
// their pads too, or those pads would stay "taken" and could never join again.
REX_HOOK_RAW(sub_8249A868) {
  if (!Enabled()) { __imp__sub_8249A868(ctx, base); return; }  // mod off: the game's own function
  __imp__sub_8249A868(ctx, base);
  if (!Enabled()) return;
  for (int p = 2; p < 4; p++) W8(base, kPadKind + p, 0xFF);
  for (Prog& pr : g_prog) pr = Prog{};
  std::memset(g_cont, 0, sizeof(g_cont));
  ClearRoster();  // New Game forgets the saved heroes; a load restores them right after (sub_82497950)
  REXKRNL_INFO("story4p: story work reset, P3/P4 seats cleared");
}

// Stage "P2 joins" check (stage state 4): with P2 present, a free pad pressing START joins as P3/P4.
REX_HOOK_RAW(sub_824E3618) {
  if (!Enabled()) { __imp__sub_824E3618(ctx, base); return; }  // mod off: the game's own function
  const uint32_t stage = ctx.r3.u32;
  if (Enabled() && g_skip_req && R8(base, kWork + 0x3b4) == 0) {
    // The play state (sub_824E4918) ends the stage when the scripts put a negative end code in
    // work+0x3b1; it then saves the party and the story module loads the next stage.
    g_skip_req = false;
    W8(base, kWork + 0x3b1, 0xFF);
    REXKRNL_INFO("story4p: stage {} cleared by debug command", (int)R8(base, kWork + 0x3af));
  }
  // The original join needs stage+0x172ac ("P2 seat open and no event"), which stays 0 while P2
  // plays. Use work+0x3b4 instead (1 while an event/dialogue runs): joining during an event leaves
  // the stage stuck behind it.
  if (Enabled() && R8(base, kWork + 0x3b4) == 0 && S8(base, kPadKind + 1) >= 0 &&
      S8(base, kKinds + 2) >= 0) {
    int player = -1;
    for (int p = 2; p < MaxPlayers(); p++)
      if (S8(base, KindOf(p)) < 0) { player = p; break; }
    if (player >= 0) {
      Guest g(ctx, base);
      if (!(g(sub_82421B40) & 0xff)) {
        for (int pad = 0; pad < 4; pad++) {
          bool taken = false;
          for (int i = 0; i < 4; i++)  // a P3/P4 pad only counts while that player is in the party
            taken |= S8(base, kPadKind + i) == pad && (i < 2 || S8(base, KindOf(i)) >= 0);
          if (taken || !(g(sub_82419C38, pad) & 1)) continue;
          if (!(R32(base, g(sub_8241B800, pad)) & 0x400000)) continue;
          REXKRNL_INFO("story4p: pad {} pressed START, joining as P{}", pad, player + 1);
          Park(base, player, pad);
          g(sub_82496728, kGlobalWork, 1, pad);
          W8(base, kGlobalWork + 0x2c, 0);
          W16(base, stage + 0xe, (g(sub_8242B528) & 0xff) ? 8 : 9);
          ctx.r3.u64 = 0;
          return;
        }
      }
    }
  }
  __imp__sub_824E3618(ctx, base);
}

// Parameter screen hero cycling (sub_82500658(screen, index, next)): skip heroes in use.
REX_HOOK_RAW(sub_82500658) {
  if (!Enabled()) { __imp__sub_82500658(ctx, base); return; }  // mod off: the game's own function
  if (!Enabled() || !g_join.active) { __imp__sub_82500658(ctx, base); return; }
  const PPCContext in = ctx;
  __imp__sub_82500658(ctx, base);
  for (int i = 0; i < 5 && PanelTaken(base, ctx.r3.s32); i++) {
    PPCContext c = in;
    c.r4.s64 = ctx.r3.s32;
    __imp__sub_82500658(c, base);
    ctx.r3 = c.r3;
  }
}

// Parameter screen hero pick (sub_82500710(screen, panel, index, next, step)): the initial pick
// (step 0) only calls sub_82500658 when the index clashes with the other panel (P1), so P4 was
// offered (and could take) P3's hero. Re-check the result against P1..P4 here.
REX_HOOK_RAW(sub_82500710) {
  if (!Enabled()) { __imp__sub_82500710(ctx, base); return; }  // mod off: the game's own function
  if (!Enabled() || !g_join.active) { __imp__sub_82500710(ctx, base); return; }
  const PPCContext in = ctx;
  __imp__sub_82500710(ctx, base);
  const uint32_t screen = in.r3.u32;
  int idx = ctx.r3.s32;
  for (int i = 0; i < 6; i++) {
    bool clash = PanelTaken(base, idx);
    for (int k = 0; k < 2; k++) {
      if (k == in.r4.s32) continue;
      const uint32_t panel = R32(base, screen + 0x1a630 + k * 4);
      if (panel && (int32_t)R32(base, panel + 0x81c) == idx) clash = true;
    }
    if (!clash) break;
    PPCContext c = in;
    c.r4.s64 = idx;
    c.r5 = in.r6;  // direction
    __imp__sub_82500658(c, base);
    idx = c.r3.s32;
  }
  ctx.r3.s64 = idx;
}

// Continue (sub_824E4308(stage), every frame): a downed P1/P2 spends a credit and gets up. It only
// knows party 1/2 (registry slots 1/2, state words stage+0x171b8/bc, timers +0x171c0/c4), so it is
// run a second time with P3/P4 swapped into those places. With no credit left the original ends the
// game, for P3/P4 too.
// Hooked in cheats.cpp, which calls this in place of the original.
extern "C" REX_FUNC(Story4pHook_sub_824E4308) {
  if (!Enabled()) { __imp__sub_824E4308(ctx, base); return; }  // mod off: the game's own function
  const uint32_t stage = ctx.r3.u32;
  PPCContext saved = ctx;
  if (Enabled()) {
    // The continue only takes a downed player without a brain; one that finishes falling with a
    // brain lies down for good and is removed like an NPC. Release the brain here, right before the
    // continue looks, for every player (found by +0xe0, which also holds during events, when
    // +0xdc reads 0 and UpdateAi does not see the players).
    Guest g(saved, base);
    for (uint32_t s = 1; s < 32; s++) {
      const uint32_t ch = Chara(base, s);
      if (!ch) continue;
      const int8_t p = S8(base, ch + 0xE0);
      if (p < 0 || p > 3 || R16(base, ch + 0xCA) <= 0x7fff) continue;
      if (s != 1 && s != 2 && s != SlotFor(2) && s != SlotFor(3)) continue;
      const uint32_t brain = Deref(base, ch + 0x174);
      if (!brain) continue;
      g(sub_82479E60, ch, 0);
      if (brain == g_brain[p]) g_brain[p] = 0;
      g_retry[p] = 30;
      REXKRNL_INFO("story4p: P{} down (event {}), brain {:08X} released before the continue", p + 1,
                   R8(base, kWork + 0x3b4), brain);
    }
  }
  __imp__sub_824E4308(ctx, base);
  // With no credit left the second run ends the game for a downed P3/P4, as the original does for a
  // downed P1/P2 (even with the other one up): the original rule, for the four players.
  if (!Enabled() || R8(base, stage + 0x171c8)) return;
  bool any = false;
  for (int p = 2; p < 4; p++) any |= S8(base, KindOf(p)) >= 0;
  if (!any) return;

  auto swap_bytes = [&](uint32_t a, uint32_t b, uint32_t n) {
    uint8_t t[8];
    std::memcpy(t, base + a, n);
    std::memmove(base + a, base + b, n);
    std::memcpy(base + b, t, n);
  };
  auto swap_all = [&]() {
    for (int i = 0; i < 2; i++) {
      const int p = i + 2;
      swap_bytes(kWork + (1 + i + 0xd1) * 8, kWork + (SlotFor(p) + 0xd1) * 8, 8);
      swap_bytes(kKinds + 1 + i, KindOf(p), 1);
      uint32_t st = R32(base, stage + 0x171b8 + i * 4), tm = R32(base, stage + 0x171c0 + i * 4);
      W32(base, stage + 0x171b8 + i * 4, g_cont[i][0]);
      W32(base, stage + 0x171c0 + i * 4, g_cont[i][1]);
      g_cont[i][0] = st;
      g_cont[i][1] = tm;
    }
  };
  swap_all();
  ctx = saved;
  __imp__sub_824E4308(ctx, base);
  swap_all();
}

// Pad read (sub_825CF040(pad, state), XINPUT_STATE with wButtons at +4). When every player in the
// game runs under the AI (pure autoplay), tap A on their pads every 40 polls so dialogues, the
// parameter screens and the route choice go on by themselves. In combat the brains ignore the pad.
REX_HOOK_RAW(sub_825CF040) {
  if (!Enabled()) { __imp__sub_825CF040(ctx, base); return; }  // mod off: the game's own function
  const uint32_t pad = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_825CF040(ctx, base);
  if (!Enabled() || ctx.r3.u32 != 0 || pad > 3) return;
  InitAi();
  static uint32_t polls;
  if (pad == 0) polls++;
  bool everyone = true, mine = false;
  for (int p = 0; p < 4; p++) {
    const int8_t k = S8(base, kPadKind + p);
    if (k < 0) continue;
    everyone &= g_ai[p];
    mine |= k == (int8_t)pad && g_ai[p];
  }
  // AI players walking on (UpdateIdle): hold the d-pad and the stick on their pad.
  for (int p = 0; p < 4; p++) {
    if (!g_idle[p].dir || S8(base, kPadKind + p) != (int8_t)pad) continue;
    if (g_idle[p].target) {  // hitting a prop: no direction, tap X (attack)
      const uint16_t b = R16(base, state + 4) & ~0x000Fu;
      W16(base, state + 4, (polls % 16) < 4 ? (b | 0x4000) : b);
      W16(base, state + 8, 0);
      W16(base, state + 10, 0);
      continue;
    }
    uint16_t b = R16(base, state + 4) & ~0x000Fu;
    // Jumping over a prop: UP is jump in this game (A only hops); held for the first frames of the
    // jump, direction held throughout.
    const bool up = g_idle[p].jump_until > g_frame && g_idle[p].jump_until - g_frame > 30;
    if (up) b |= 0x0001;                                               // DPAD_UP
    W16(base, state + 4, b | (g_idle[p].dir > 0 ? 0x0008 : 0x0004));  // DPAD_RIGHT / DPAD_LEFT
    W16(base, state + 8, g_idle[p].dir > 0 ? 0x7FFF : 0x8001);       // sThumbLX
    W16(base, state + 10, up ? 0x7FFF : 0);                           // sThumbLY
  }
  // Not on the pad of a player walking on: A is jump, it hopped along instead of walking.
  bool walking = false;
  for (int p = 0; p < 4; p++) walking |= g_idle[p].dir && S8(base, kPadKind + p) == (int8_t)pad;
  const char* menus = Env("GH_AI_MENUS");  // GH_AI_MENUS=0 turns the tapping off
  if (everyone && mine && !walking && !(menus && *menus == '0') && polls % 40 < 3)
    W16(base, state + 4, R16(base, state + 4) | 0x1000);  // XINPUT_GAMEPAD_A
  // GH_SKIP_TALK=1 (tests): while an event/dialogue runs (work+0x3b4), P1's pad taps BACK (skip
  // the scene) and A (next line), alternately, whatever the AI setting.
  static int skip = -1;
  if (skip < 0) { const char* s = Env("GH_SKIP_TALK"); skip = (s && *s == '1') ? 1 : 0; }
  if (skip && (int8_t)pad == S8(base, kPadKind) && R8(base, kWork + 0x3b4) && Story(base)) {
    const uint32_t ph = polls % 60;
    if (ph < 3) W16(base, state + 4, R16(base, state + 4) | 0x0020);              // BACK
    else if (ph >= 30 && ph < 33) W16(base, state + 4, R16(base, state + 4) | 0x1000);  // A
  }
}

// ---------------------------------------------------------------------------------------------
// Progress of P2/P3/P4 across saves (per hero)
// ---------------------------------------------------------------------------------------------
// The save keeps P1 (and P2's block), but loading empties the P2 seat; whoever joins with START
// gets a fresh block (level = P1's / 2, no points spent: sub_82501D98 state 4). P3/P4 are not in the
// save at all. The blocks of the heroes played by P2/P3/P4 are kept here, per hero, following the
// game's own snapshot (owner+0x1014, taken by sub_82499F40 and restored by sub_82497950) and its
// disk file (sub_8249A638 writes, sub_8249B198 reads): file gh4p_party.bin in the user data folder.
// A player who joins with a hero that has a block gets it back (when its level is not below the fresh
// one). New Game (sub_8249A868 then a snapshot) forgets them. GH_4P_KEEP=0 turns this off.
namespace {

struct Roster {
  uint8_t has[6] = {};             // by hero 1..5 (Han 7 -> 2)
  uint8_t block[6][kBlock] = {};
};
Roster g_snap;    // mirrors the game's snapshot (what the next disk save writes)
Roster g_roster;  // live: restored from the snapshot, waiting for their heroes to join again
Roster g_disk;    // last content written to / read from the file
uint32_t g_fresh;  // block just set up by sub_824968F0 (0 = none)

bool KeepProgress() {
  static int v = -1;
  if (v < 0) { const char* s = Env("GH_4P_KEEP"); v = (s && *s == '0') ? 0 : 1; }
  return v == 1 && Enabled();
}

int HeroKey(int kind) { return kind == 7 ? 2 : (kind >= 1 && kind <= 5 ? kind : 0); }

std::filesystem::path RosterPath() {
  rex::Runtime* rt = rex::Runtime::instance();
  return (rt ? rt->user_data_root() : std::filesystem::path(".")) / "gh4p_party.bin";
}

std::string RosterText(const Roster& r) {
  std::string s;
  for (int k = 1; k <= 5; k++)
    if (r.has[k]) s += fmt::format(" hero {} lv {}", k, r.block[k][0xe]);
  return s.empty() ? " (empty)" : s;
}

void WriteRoster() {
  if (!std::memcmp(&g_disk, &g_snap, sizeof(Roster))) return;
  const auto path = RosterPath();
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) { REXKRNL_WARN("story4p: cannot write {}", path.string()); return; }
  f.write("GH4P", 4);
  f.write(reinterpret_cast<const char*>(&g_snap), sizeof(Roster));
  g_disk = g_snap;
  REXKRNL_INFO("story4p: party progress saved:{}", RosterText(g_snap));
}

void ReadRoster() {
  g_snap = Roster{};
  const auto path = RosterPath();
  std::ifstream f(path, std::ios::binary);
  char magic[4] = {};
  Roster r;
  if (f && f.read(magic, 4) && !std::memcmp(magic, "GH4P", 4) &&
      f.read(reinterpret_cast<char*>(&r), sizeof(Roster)))
    g_snap = r;
  g_disk = g_snap;
  g_roster = g_snap;
  REXKRNL_INFO("story4p: party progress loaded:{}", RosterText(g_snap));
}

void ClearRoster() { g_roster = Roster{}; }

// After the game's snapshot: the heroes of P2/P3/P4 now, plus those still waiting in the roster.
void TakeSnapshot(uint8_t* base) {
  g_snap = g_roster;
  struct Seat { uint32_t kind, blk; };
  const Seat seats[3] = {{kKinds + 2, kWork + 2 * kBlock}, {KindOf(2), BlockOf(2)}, {KindOf(3), BlockOf(3)}};
  for (const Seat& s : seats) {
    const int k = HeroKey(S8(base, s.kind));
    if (!k) continue;
    g_snap.has[k] = 1;
    std::memcpy(g_snap.block[k], base + s.blk, kBlock);
  }
  const int p1 = HeroKey(S8(base, kKinds + 1));
  if (p1) g_snap.has[p1] = 0;  // P1's hero is in the game's own save
  g_roster = g_snap;
}

}  // namespace

// End of stage / checkpoint: keep P3/P4 level and EXP in their party blocks.
REX_HOOK_RAW(sub_82499F40) {
  if (!Enabled()) { __imp__sub_82499F40(ctx, base); return; }  // mod off: the game's own function
  if (Enabled())
    REXKRNL_INFO("story4p: party save r4 {} stage {} prog P3 k{} lv{} P4 k{} lv{}", ctx.r4.u32 & 0xff,
                 (int)R8(base, kWork + 0x3af), g_prog[2].kind, g_prog[2].lv, g_prog[3].kind, g_prog[3].lv);
  if (Enabled() && (ctx.r4.u32 & 0xff)) {
    for (int p = 2; p < 4; p++) {
      const int kind = S8(base, KindOf(p));
      if (kind < 0 || g_prog[p].kind != kind) continue;
      const uint32_t blk = BlockOf(p);
      if (g_prog[p].lv < R8(base, blk + 0xe)) continue;  // never lower a saved level
      W32(base, blk + 0, g_prog[p].exp);
      W16(base, blk + 0xc, g_prog[p].d6);
      W8(base, blk + 0xe, g_prog[p].lv);
      REXKRNL_INFO("story4p: P{} progress kept: kind {} level {} exp {}", p + 1, kind, g_prog[p].lv,
                   g_prog[p].exp);
    }
  }
  __imp__sub_82499F40(ctx, base);
  g_fresh = 0;
  if (KeepProgress() && !g_join.active) TakeSnapshot(base);
}

// Snapshot restored (save loaded, continue after a game over): the saved heroes wait for their players.
REX_HOOK_RAW(sub_82497950) {
  if (!Enabled()) { __imp__sub_82497950(ctx, base); return; }  // mod off: the game's own function
  __imp__sub_82497950(ctx, base);
  for (Prog& pr : g_prog) pr = Prog{};
  if (!KeepProgress()) return;
  g_roster = g_snap;
  REXKRNL_INFO("story4p: party progress restored:{}", RosterText(g_roster));
}

// "System Data" written (r4 != 0: reset to defaults, the snapshot is gone too). The function returns
// early when nothing changed; the file is only written when it reaches the content write
// (sub_824380D0), so gh4p_party.bin follows the save on disk.
bool g_save_write;
REX_HOOK_RAW(sub_824380D0) {
  if (!Enabled()) { __imp__sub_824380D0(ctx, base); return; }  // mod off: the game's own function
  g_save_write = true;
  __imp__sub_824380D0(ctx, base);
}

REX_HOOK_RAW(sub_8249A638) {
  if (!Enabled()) { __imp__sub_8249A638(ctx, base); return; }  // mod off: the game's own function
  const bool reset = ctx.r4.u32 & 0xff;
  g_save_write = false;
  __imp__sub_8249A638(ctx, base);
  if (!KeepProgress() || !g_save_write) return;
  if (reset) g_snap = Roster{};
  WriteRoster();
}

REX_HOOK_RAW(sub_8249B198) {
  if (!Enabled()) { __imp__sub_8249B198(ctx, base); return; }  // mod off: the game's own function
  __imp__sub_8249B198(ctx, base);
  if (KeepProgress()) ReadRoster();
}

// A hero put in the party: remember the block for the panel that shows it next (the join).
REX_HOOK_RAW(sub_824968F0) {
  if (!Enabled()) { __imp__sub_824968F0(ctx, base); return; }  // mod off: the game's own function
  const uint32_t blk = ctx.r3.u32 + ctx.r4.u32 * kBlock;
  __imp__sub_824968F0(ctx, base);
  g_fresh = blk;
}

// Parameter screen panel reads its block: right after a join (sub_82501D98 state 4: fresh block,
// level = P1's / 2), a saved block of the same hero replaces it.
REX_HOOK_RAW(sub_82501690) {
  if (!Enabled()) { __imp__sub_82501690(ctx, base); return; }  // mod off: the game's own function
  const uint32_t panel = ctx.r3.u32;
  const uint32_t seat = R32(base, panel + 0x818);
  const uint32_t blk = kWork + (seat + 1) * kBlock;
  if (KeepProgress() && g_fresh && g_fresh == blk && seat < 2) {
    const int k = HeroKey(S8(base, kKinds + 1 + seat));
    if (k && g_roster.has[k] && k != HeroKey(S8(base, kKinds + 1))) {
      const uint8_t* saved = g_roster.block[k];
      if (saved[0xe] >= R8(base, blk + 0xe)) {
        std::memcpy(base + blk, saved, kBlock);
        int spent = 0;
        for (uint32_t a = 0x15; a <= 0x1a; a++) spent += saved[a];
        REXKRNL_INFO("story4p: hero {} joins with its saved block (level {}, points spent {})", k, saved[0xe],
                     spent);
      } else {
        REXKRNL_INFO("story4p: hero {} saved level {} below the fresh {}: not used", k, saved[0xe],
                     R8(base, blk + 0xe));
      }
    }
  }
  g_fresh = 0;
  __imp__sub_82501690(ctx, base);
}

// End of stage 1 (sub_8256E290(stage), every frame): P1/P2 past x 976 get the "walk out" brain 0x31
// and lose their player/team fields; once both are out the stage ends (stage+0x15180). P3/P4 do the
// same past x 976, and any still in walk out with them when the stage ends (only looks; the end still
// waits only for P1/P2).
REX_HOOK_RAW(sub_8256E290) {
  if (!Enabled()) { __imp__sub_8256E290(ctx, base); return; }  // mod off: the game's own function
  const uint32_t stage = ctx.r3.u32;
  const PPCContext saved = ctx;
  // stage+0x15180 also holds the opening scene's object; the exit test only runs while it is 0, and
  // the stage ends when this call sets it.
  const bool testing = R32(base, stage + 0x15180) == 0;
  __imp__sub_8256E290(ctx, base);
  if (!Enabled() || !Story(base) || !testing) return;
  const bool ending = R32(base, stage + 0x15180) != 0;
  Guest g(const_cast<PPCContext&>(saved), base);
  for (int p = 2; p < 4; p++) {
    if (S8(base, KindOf(p)) < 0) continue;
    const uint32_t ch = Chara(base, SlotFor(p));
    if (!ch || S8(base, ch + 0xE0) != p || R16(base, ch + 0xCA) > 0x7fff) continue;
    const int32_t x = (int32_t)R32(base, ch + 0xC);
    if (!ending && (x <= 0x3cfffff || R8(base, ch + 0x55))) continue;
    g(sub_82479E60, ch, 0x31);
    W8(base, ch + 0xDE, 0xFF);
    W8(base, ch + 0xDD, 0xFF);
    W8(base, ch + 0xE0, 0xFF);
    W8(base, ch + 0x140, 0);
    if (g_brain[p]) g_brain[p] = 0;
    REXKRNL_INFO("story4p: stage 1 exit: P{} walks out (x {}{})", p + 1, x >> 16, ending ? ", stage ending" : "");
  }
}

// Stage scripts only move the hero slots 1..5 in cut scenes; P3/P4 stay where they spawned (often
// far off screen). When an event ends, an off-screen P3/P4 is put next to P1.
static void BringExtrasAfterEvent(uint8_t* base) {
  static uint8_t prev;
  const uint8_t ev = R8(base, kWork + 0x3b4);
  const bool ended = prev && !ev;
  prev = ev;
  if (!ended || !Story(base)) return;
  const uint32_t p1 = Chara(base, 1);
  if (!p1) return;
  const int32_t wl = (int32_t)R32(base, kCamera + 0x318), wr = (int32_t)R32(base, kCamera + 0x31C);
  for (int p = 2; p < 4; p++) {
    const uint32_t ch = Chara(base, SlotFor(p));
    if (!ch || R8(base, ch + 0xDC) != 1) continue;
    const int32_t x = (int32_t)R32(base, ch + 0xC);
    if (x >= wl && x <= wr) continue;
    int32_t nx = (int32_t)R32(base, p1 + 0xC) + (p == 2 ? -0x300000 : 0x300000);
    if (nx < wl + 0x200000) nx = wl + 0x200000;
    if (nx > wr - 0x200000) nx = wr - 0x200000;
    W32(base, ch + 0xC, (uint32_t)nx);
    W32(base, ch + 0x10, R32(base, p1 + 0x10) + (p == 2 ? 0x100000 : (uint32_t)-0x100000));
    W32(base, ch + 0x14, R32(base, p1 + 0x14));
    W8(base, ch + 0xEF, R8(base, ch + 0xEF) | 0x40);
    REXKRNL_INFO("story4p: P{} brought next to P1 after the event (x {} -> {})", p + 1, x >> 16, nx >> 16);
  }
}

// Once per frame while a stage runs: AI toggles and the optional status log.
REX_HOOK_RAW(sub_82498808) {
  if (!Enabled()) { __imp__sub_82498808(ctx, base); return; }  // mod off: the game's own function
  if (Enabled()) {
    InitAi();
    PollKeys();
    g_frame++;
    BringExtrasAfterEvent(base);
    // Infinite credits (work+0x3ae): kept at 9; the HUD counter is hidden (sub_8247FE88).
    if (InfiniteCredits() && Story(base) && R8(base, kWork + 0x3ae) < 9) W8(base, kWork + 0x3ae, 9);
    for (int p = 2; p < 4; p++) {
      const int kind = S8(base, KindOf(p));
      const uint32_t ch = kind >= 0 ? Chara(base, SlotFor(p)) : 0;
      const int ck = ch ? R8(base, ch + 0x54) : -1;
      if (ch && (ck == kind || (kind == 2 && ck == 7)) && R8(base, ch + 0xE1) > 0) {
        g_prog[p] = Prog{kind, R32(base, ch + 0xC0), R16(base, ch + 0xD6), R8(base, ch + 0xE1)};
        // The level-up (sub_82468E90) writes P1/P2's block at once; P3/P4's block did not follow, so
        // the next stage created them at the old level. Keep their block up to date here.
        const uint32_t blk = BlockOf(p);
        const uint8_t lv = g_prog[p].lv, blv = R8(base, blk + 0xe);
        if (lv > blv || (lv == blv && g_prog[p].exp > R32(base, blk + 0))) {
          if (lv > blv) REXKRNL_INFO("story4p: P{} level {} -> block (was {})", p + 1, lv, blv);
          W32(base, blk + 0, g_prog[p].exp);
          W16(base, blk + 0xc, g_prog[p].d6);
          W8(base, blk + 0xe, lv);
        }
      }
    }
    Guest g(ctx, base);
    UpdateAi(g);
    AutoDebug(base);
    // GH_4P_DOWN=34 (tests): every GH_4P_DOWN_SECS (default 20) s of play, those players (1..4) are
    // downed through the game's own death (HP -1, vtable +0x2d8), as when they land after a hit.
    if (const char* dl = Env("GH_4P_DOWN")) {
      static uint32_t played;
      const char* ds = Env("GH_4P_DOWN_SECS");
      const uint32_t every = (ds ? std::atoi(ds) : 20) * 60;
      if (!R8(base, kWork + 0x3b4) && Story(base) && ++played % every == 0)
        for (; *dl; dl++) {
          const int p = *dl - '1';
          if (p < 0 || p > 3) continue;
          const uint32_t ch = FindPlayer(base, p);
          if (!ch || R16(base, ch + 0xCA) > 0x7fff) continue;
          PPCFunc* fn = rex::runtime::ResolveIndirectFunction(R32(base, R32(base, ch) + 0x2d8));
          if (!fn) continue;
          W16(base, ch + 0xCA, 0xFFFF);
          g(fn, ch);
          REXKRNL_INFO("story4p: debug: P{} downed (credits {})", p + 1, R8(base, kWork + 0x3ae));
        }
    }
    LogPlayers(base);
  }
  __imp__sub_82498808(ctx, base);
}

// The story keeps one registry slot per hero (kinds 1..5) in stage+0x1719c+(kind-1)*4: the party
// member's slot when that hero is played (sub_82496990 only looks at P1/P2), else the first free
// slot 1..5, where the story spawns the hero as an NPC that leaves when the stage starts. The scripts
// ask work+0x3d8+slot ("is it a party member?") before driving or removing that character, so
// pointing the heroes of P3/P4 at their slots makes them take the NPC's place, as for P1/P2; the
// heroes nobody plays get the hero slots left over. GH_4P_HEROSLOT=0 keeps the NPC copies.
REX_HOOK_RAW(sub_824E2AC0) {
  if (!Enabled()) { __imp__sub_824E2AC0(ctx, base); return; }  // mod off: the game's own function
  const uint32_t stage = ctx.r3.u32;
  __imp__sub_824E2AC0(ctx, base);
  if (g_stage != stage) std::memset(g_cont, 0, sizeof(g_cont));  // new stage object: as P1/P2's
  g_stage = stage;
  std::memset(g_npc_home, 0, sizeof(g_npc_home));
  if (!Enabled() || !HeroSlots()) return;
  int owner[6] = {0, 0, 0, 0, 0, 0};  // hero kind -> slot of the player that plays it (0 = NPC)
  bool used[32] = {};
  for (uint32_t slot : {1u, 2u, SlotFor(2), SlotFor(3)}) {
    int k = S8(base, kKinds + slot);
    if (k == 7) k = 2;
    if (k < 1 || k > 5 || !Chara(base, slot) || slot >= 32) continue;
    owner[k] = (int)slot;
    used[slot] = true;
  }
  if (!owner[1] && !owner[2] && !owner[3] && !owner[4] && !owner[5]) return;
  for (int k = 1; k <= 5; k++) {
    const uint32_t at = stage + 0x1719c + (k - 1) * 4;
    const uint32_t old = R32(base, at);
    uint32_t slot = (uint32_t)owner[k];
    if (!slot) {
      for (uint32_t s = 1; s <= 5; s++)
        if (!used[s]) { slot = s; break; }
      if (!slot) slot = old;
      if (slot < 32) used[slot] = true;
    }
    W32(base, at, slot);
    if (slot != old) REXKRNL_INFO("story4p: hero {} slot {} -> {}", k, old, slot);
    if (!owner[k] && slot != old && slot < 24 && old >= 1 && old < 24) g_npc_home[slot] = (uint8_t)old;
  }
}

// ---------------------------------------------------------------------------------------------
// HUD: the four player panels on the top row, credits hidden
// ---------------------------------------------------------------------------------------------
// sub_8249D2C0 lays out the HUD panels every frame. In story mode the panels of registry slots 1/2
// (P1/P2) sit at fixed spots on the top row (x 410/688, y 32; "Credit(s)" on the right); every
// other big panel (P3/P4 in slots 30/31, the story heroes) flows on the second row from x 140. With
// three or more players, the four player panels share the top row, using the room of the credits.
// To keep P3/P4 out of the flow, their character's slot (+0xda) reads 1 for the layout test made
// right after sub_8249CC40 returns; the next panel update (or the end of the layout) restores it.
namespace {

int PartyPlayers(uint8_t* base) {
  int n = 0;
  for (uint32_t slot : {1u, 2u, SlotFor(2), SlotFor(3)}) n += S8(base, kKinds + slot) >= 0;
  return n;
}

uint32_t g_da_ch;
uint8_t g_da_old;
void RestoreDa(uint8_t* base) {
  if (g_da_ch) { W8(base, g_da_ch + 0xDA, g_da_old); g_da_ch = 0; }
}

}  // namespace

REX_HOOK_RAW(sub_8249CC40) {
  if (!Enabled()) { __imp__sub_8249CC40(ctx, base); return; }  // mod off: the game's own function
  const uint32_t panel = ctx.r3.u32;
  RestoreDa(base);
  __imp__sub_8249CC40(ctx, base);
  if (!Enabled() || !Story(base) || R8(base, panel + 0x8a4) || R32(base, panel + 0x89c) != 0) return;
  const int n = PartyPlayers(base);
  if (n < 3) return;
  const uint32_t ch = Deref(base, panel + 0x18);
  if (!ch || R8(base, ch + 0xDC) != 1) return;
  int p = S8(base, ch + 0xE0);
  if (p < 0 || p > 3) {
    // Walking out at the end of stage 1 (sub_8256E290 clears +0xe0): find the player by its slot, or
    // its panel would jump to the original layout for the last frames of the stage.
    const uint32_t slot = R8(base, ch + 0xDA);
    p = slot == 1 ? 0 : slot == 2 ? 1 : -1;
    for (int q = 2; q < 4 && p < 0; q++)
      if (slot == SlotFor(q) && S8(base, KindOf(q)) >= 0) p = q;
    if (p < 0) return;
  }
  // Panel = portrait + bars, ~225 px wide; same 278 px pitch as P1/P2, centred on the 1280 screen.
  static const float x4[4] = {110, 388, 666, 944}, x3[4] = {250, 528, 806, 944};
  WF(base, panel + 0x888, n >= 4 ? x4[p] : x3[p]);
  WF(base, panel + 0x88c, 32);
  const uint8_t da = R8(base, ch + 0xDA);
  if (da == 0 || da > 2) {
    g_da_ch = ch;
    g_da_old = da;
    W8(base, ch + 0xDA, 1);
  }
}

REX_HOOK_RAW(sub_8249D2C0) {
  if (!Enabled()) { __imp__sub_8249D2C0(ctx, base); return; }  // mod off: the game's own function
  __imp__sub_8249D2C0(ctx, base);
  RestoreDa(base);
}

// "Credit(s) N" (up to 3 quads counted in +0x130): nothing to draw while credits are infinite.
// With finite credits and 3+ players the counter sat on P4's panel: it goes to the bottom right
// corner for the call (+0x84 right edge x, +0x88 y of the counter; the quads are built from them).
REX_HOOK_RAW(sub_8247FE88) {
  if (!Enabled()) { __imp__sub_8247FE88(ctx, base); return; }  // mod off: the game's own function
  const uint32_t obj = ctx.r3.u32;
  const bool lower = Enabled() && !InfiniteCredits() && Story(base) && PartyPlayers(base) >= 3;
  const uint32_t y = R32(base, obj + 0x88);
  if (lower) {
    static bool logged;
    if (!logged) {
      logged = true;
      float fx, fy; uint32_t x = R32(base, obj + 0x84);
      std::memcpy(&fx, &x, 4); std::memcpy(&fy, &y, 4);
      REXKRNL_INFO("story4p: credits counter at x {} y {}: moved to the bottom", fx, fy);
    }
    const char* cy = Env("GH_4P_CREDITS_Y");
    WF(base, obj + 0x88, cy ? (float)std::atof(cy) : 650.0f);
  }
  __imp__sub_8247FE88(ctx, base);
  if (lower) W32(base, obj + 0x88, y);
  if (Enabled() && InfiniteCredits() && Story(base)) W32(base, obj + 0x130, 0);
}

// ---------------------------------------------------------------------------------------------
// Pause: status windows for P3/P4
// ---------------------------------------------------------------------------------------------
// The pause menu (stage+0xa40, constructor sub_824E0C10) owns two status windows (vtable
// 0x8201d928, 0x6f0 bytes) at +0x12f0c (P1) and +0x135fc (P2), set up by sub_824B57E8(window,
// index). A window shows party index+1 and the character in registry slot index+1, centred on
// x = +0x268. Two more windows (index 2/3) are created for P3/P4; they follow every open/close of
// the P2 window and are updated/drawn after the menu's own, with registry slot 3/4 swapped with
// 30/31 for the call. With 3+ players the windows go P1 P2 | menu | P3 P4.
namespace {

constexpr uint32_t kStatusVtable = 0x8201d928;
constexpr uint32_t kStatusSize = 0x6f0;
uint32_t g_pause;
uint32_t g_extra[2];

void SwapRegistry(uint8_t* base, int i) {  // i = 0 (P3) / 1 (P4)
  const uint32_t a = kWork + (3 + i + 0xd1) * 8, b = kWork + (SlotFor(2 + i) + 0xd1) * 8;
  uint8_t t[8];
  std::memcpy(t, base + a, 8);
  std::memcpy(base + a, base + b, 8);
  std::memcpy(base + b, t, 8);
}

void CallExtras(Guest& g, PPCFunc* fn, bool swap) {
  for (int i = 0; i < 2; i++) {
    if (!g_extra[i]) continue;
    // With three players there is no P4: its slot holds a story NPC, keep its window closed.
    if (S8(g.base, KindOf(2 + i)) < 0) continue;
    // The window reads kind work+0x3d8+(index+1) and the character in slot index+1.
    const uint8_t k = R8(g.base, kKinds + 3 + i);
    if (swap) { SwapRegistry(g.base, i); W8(g.base, kKinds + 3 + i, R8(g.base, KindOf(2 + i))); }
    g(fn, g_extra[i]);
    if (swap) { SwapRegistry(g.base, i); W8(g.base, kKinds + 3 + i, k); }
  }
}

bool IsP2Window(uint32_t w) { return Enabled() && g_pause && w == g_pause + 0x135fc; }

void PlacePauseWindows(uint8_t* base, uint32_t pause) {
  if (PartyPlayers(base) < 3) return;
  WF(base, pause + 0x12f0c + 0x268, 167);
  WF(base, pause + 0x135fc + 0x268, 408);
  if (g_extra[0]) WF(base, g_extra[0] + 0x268, 872);
  if (g_extra[1]) WF(base, g_extra[1] + 0x268, 1113);
}

}  // namespace

REX_HOOK_RAW(sub_824E0C10) {
  if (!Enabled()) { __imp__sub_824E0C10(ctx, base); return; }  // mod off: the game's own function
  const uint32_t pause = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_824E0C10(ctx, base);
  if (!Enabled() || !Story(base)) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  for (auto& w : g_extra) {
    if (w) g(sub_825122F0, w, 1);  // the previous stage's pause is gone
    w = 0;
  }
  g_pause = pause;
  for (int i = 0; i < 2; i++) {
    const uint32_t w = g(sub_826A3900, kStatusSize);
    if (!w) continue;
    g(sub_824B2ED0, w);
    W32(base, w, kStatusVtable);
    g(sub_824B57E8, w, (uint64_t)(2 + i));
    g_extra[i] = w;
  }
  REXKRNL_INFO("story4p: pause {:08X}, P3/P4 status windows {:08X} {:08X}", pause, g_extra[0], g_extra[1]);
}

// Hooked in cheats.cpp, which calls this in place of the original.
extern "C" REX_FUNC(Story4pHook_sub_824DEA90) {
  if (!Enabled()) { __imp__sub_824DEA90(ctx, base); return; }  // mod off: the game's own function
  const uint32_t pause = ctx.r3.u32;
  const bool mine = Enabled() && pause == g_pause && Story(base) && R8(base, pause) != 0;
  if (mine) PlacePauseWindows(base, pause);
  const PPCContext saved = ctx;
  __imp__sub_824DEA90(ctx, base);
  if (!mine) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  CallExtras(g, sub_824A5218, true);
}

REX_HOOK_RAW(sub_824E01D0) {
  if (!Enabled()) { __imp__sub_824E01D0(ctx, base); return; }  // mod off: the game's own function
  const uint32_t pause = ctx.r3.u32;
  const bool mine = Enabled() && pause == g_pause && Story(base) && (int16_t)R16(base, pause + 0x9c8) >= 2;
  const PPCContext saved = ctx;
  __imp__sub_824E01D0(ctx, base);
  if (!mine) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  CallExtras(g, sub_824AFC38, true);
}

REX_HOOK_RAW(sub_824A6C40) {
  if (!Enabled()) { __imp__sub_824A6C40(ctx, base); return; }  // mod off: the game's own function
  const uint32_t w = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_824A6C40(ctx, base);
  if (!IsP2Window(w)) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  CallExtras(g, __imp__sub_824A6C40, false);
}

REX_HOOK_RAW(sub_824A5158) {
  if (!Enabled()) { __imp__sub_824A5158(ctx, base); return; }  // mod off: the game's own function
  const uint32_t w = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_824A5158(ctx, base);
  if (!IsP2Window(w)) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  CallExtras(g, __imp__sub_824A5158, false);
}

REX_HOOK_RAW(sub_824A5188) {
  if (!Enabled()) { __imp__sub_824A5188(ctx, base); return; }  // mod off: the game's own function
  const uint32_t w = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_824A5188(ctx, base);
  if (!IsP2Window(w)) return;
  Guest g(const_cast<PPCContext&>(saved), base);
  CallExtras(g, __imp__sub_824A5188, false);
}

// ---------------------------------------------------------------------------------------------
// P3/P4 party blocks and kinds where the game expects party index == registry slot
// ---------------------------------------------------------------------------------------------
namespace {

// work+slot*0x24 for slots 30/31 lands on other per-slot arrays (work+0x438.., 0x47a..). While the
// game reads/writes "the block of slot 30/31", that place temporarily holds the P3/P4 block.
struct Shadow {
  uint8_t* base = nullptr;
  uint32_t region = 0, blk = 0;
  uint8_t saved[kBlock];
  Shadow(uint8_t* b, uint32_t ch) {
    if (!Enabled() || !ch) return;
    const uint32_t slot = R8(b, ch + 0xDA);
    const int p = slot == SlotFor(2) ? 2 : (slot == SlotFor(3) ? 3 : -1);
    if (p < 0 || slot < 12) return;  // slots 0..11 have their own block
    base = b;
    region = kWork + slot * kBlock;
    blk = BlockOf(p);
    std::memcpy(saved, b + region, kBlock);
    std::memcpy(b + region, b + blk, kBlock);
  }
  ~Shadow() {
    if (!base) return;
    std::memcpy(base + blk, base + region, kBlock);
    std::memcpy(base + region, saved, kBlock);
  }
};

// Kinds of P3/P4 copied into kind entries a/b for one call (restored after).
struct Expose {
  uint8_t* base = nullptr;
  uint32_t at[2];
  uint8_t old[2];
  Expose(uint8_t* b, uint32_t a0, uint32_t a1) {
    if (!Enabled() || !Story(b)) return;
    if (SlotFor(2) == a0 && SlotFor(3) == a1) return;  // already there
    if (SlotFor(2) < 12 && SlotFor(3) < 12 && a0 >= 10) return;  // preloads already see 0..11
    base = b;
    at[0] = a0; at[1] = a1;
    for (int i = 0; i < 2; i++) {
      old[i] = R8(b, kKinds + at[i]);
      W8(b, kKinds + at[i], R8(b, KindOf(2 + i)));
    }
  }
  ~Expose() {
    if (base)
      for (int i = 0; i < 2; i++) W8(base, kKinds + at[i], old[i]);
  }
};

}  // namespace

// Hooked in cheats.cpp, which calls this in place of the original.
extern "C" REX_FUNC(Story4pHook_sub_82468FA8) { Shadow s(base, ctx.r3.u32); __imp__sub_82468FA8(ctx, base); }
REX_HOOK_RAW(sub_82468E90) { Shadow s(base, ctx.r3.u32); __imp__sub_82468E90(ctx, base); }
REX_HOOK_RAW(sub_82512B90) { Shadow s(base, ctx.r3.u32); __imp__sub_82512B90(ctx, base); }

// Player index p -> party p+1 (Repertoire etc.): P3/P4 read entries 3/4.
REX_HOOK_RAW(sub_824C0470) { Expose e(base, 3, 4); __imp__sub_824C0470(ctx, base); }
// Asset preload loops only look at entries 0..11; 10/11 are never hero kinds.
REX_HOOK_RAW(sub_8243E510) { Expose e(base, 10, 11); __imp__sub_8243E510(ctx, base); }
REX_HOOK_RAW(sub_824EB8D0) { Expose e(base, 10, 11); __imp__sub_824EB8D0(ctx, base); }

// ---------------------------------------------------------------------------------------------
// Stage actor scripts: P3/P4 run as P2
// ---------------------------------------------------------------------------------------------
// The stages drive each hero in cut scenes with an actor script f(stage, slot, kind), whose state
// lives in stage+0x170dc+slot*4 (step) and stage+0x1713c+slot*4 (scratch), 24 entries each. Some
// steps only let a party member go on when slot == 1 or slot == 2 (stage 2, sub_82570F70 step 5:
// P1 after a delay, P2 at once; any other party slot waits for ever, which froze the opening of
// stage 2 with P3/P4 in slots 3/4). For a P3/P4 slot, the script runs as slot 2: registry entry,
// kind and both state entries of slots 2 and 3/4 are swapped for the call, as the pause does for
// the status windows. GH_4P_ASP2=0 turns this off.
namespace {

bool ScriptsAsP2() {
  static int v = -1;
  if (v < 0) { const char* s = std::getenv("GH_4P_ASP2"); v = (s && *s == '0') ? 0 : 1; }
  return v == 1;
}

void SwapSlots(uint8_t* base, uint32_t stage, uint32_t a, uint32_t b) {
  auto swap = [&](uint32_t x, uint32_t y, uint32_t n) {
    uint8_t t[8];
    std::memcpy(t, base + x, n);
    std::memcpy(base + x, base + y, n);
    std::memcpy(base + y, t, n);
  };
  swap(kWork + (a + 0xd1) * 8, kWork + (b + 0xd1) * 8, 8);  // registry
  swap(kKinds + a, kKinds + b, 1);                          // party kind
  swap(stage + 0x170dc + a * 4, stage + 0x170dc + b * 4, 4);  // script step
  swap(stage + 0x1713c + a * 4, stage + 0x1713c + b * 4, 4);  // script scratch
}

void RunAsP2(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t stage = ctx.r3.u32, slot = ctx.r4.u32;
  const int p = slot == SlotFor(2) ? 2 : (slot == SlotFor(3) ? 3 : -1);
  // During the opening event of a stage the players have +0xdc = 0; +0xe0 keeps the player index.
  uint32_t ch = 0;
  if (!Enabled() || !ScriptsAsP2() || !Story(base) || p < 0 || slot < 3 || slot >= 24 ||
      S8(base, kKinds + slot) < 0 || !(ch = Chara(base, slot)) || S8(base, ch + 0xE0) != p) {
    original(ctx, base);
    return;
  }
  static uint32_t logged;  // first time per slot and stage (bit = slot)
  static uint8_t logged_stage = 0xff;
  if (logged_stage != R8(base, kWork + 0x3af)) { logged_stage = R8(base, kWork + 0x3af); logged = 0; }
  if (!(logged & (1u << slot))) {
    logged |= 1u << slot;
    REXKRNL_INFO("story4p: stage {} script for slot {} (kind {}) runs as slot 2", (int)logged_stage, slot,
                 ctx.r5.u32);
  }
  SwapSlots(base, stage, 2, slot);
  ctx.r4.u64 = 2;
  original(ctx, base);
  SwapSlots(base, stage, 2, slot);
}

}  // namespace

static void RunAsP2Hook(PPCContext& ctx, uint8_t* base) { RunAsP2(ctx, base, __imp__sub_82570F70); }

// Every stage actor script f(stage, slot, kind) goes through ActorScript.
//
// NPC heroes moved to another slot (Serena 3 -> 5 when P3 takes slot 3) run their script as their
// original slot: the scripts pick positions from per-slot tables, and a slot the table does not
// list reads an uninitialised stack value (stage 21, sub_8259B9B8 step 4 "walk to (x, z)": slot 5
// walked to a random spot and the opening froze when it was unreachable). Same swap as RunAsP2.
// GH_4P_NPCHOME=0 turns this off.
//
// GH_4P_ACTLOG=1 (debugging): log every step change per slot (with the script's address), and while
// a slot keeps the same step a dump of its character's state fields every 4 s.
namespace {
void ActorScript(uint32_t fn, PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  static int on = -1, home_on = -1;
  if (on < 0) { const char* s = std::getenv("GH_4P_ACTLOG"); on = (s && *s == '1') ? 1 : 0; }
  if (home_on < 0) { const char* s = std::getenv("GH_4P_NPCHOME"); home_on = (s && *s == '0') ? 0 : 1; }
  const uint32_t stage = ctx.r3.u32, slot = ctx.r4.u32, kind = ctx.r5.u32;
  if (slot >= 24 || !Enabled()) { original(ctx, base); return; }
  uint32_t home = 0, hc = 0;
  if (home_on && Enabled() && g_npc_home[slot] && S8(base, kKinds + slot) < 0 && (hc = Chara(base, slot)) &&
      R8(base, hc + 0x54) == kind)
    home = g_npc_home[slot];
  const uint32_t at = stage + 0x170dc + slot * 4;
  const int32_t before = (int32_t)R32(base, at);
  if (home) {
    static uint64_t logged;  // bit = slot, once per stage object
    static uint32_t logged_stage;
    if (logged_stage != stage) { logged_stage = stage; logged = 0; }
    if (!(logged & (1ull << slot))) {
      logged |= 1ull << slot;
      REXKRNL_INFO("story4p: script {:08X}: NPC hero kind {} in slot {} runs as slot {}", fn, kind, slot, home);
    }
    SwapSlots(base, stage, home, slot);
    ctx.r4.u64 = home;
    original(ctx, base);
    SwapSlots(base, stage, home, slot);
  } else {
    original(ctx, base);
  }
  if (!on) return;
  const int32_t after = (int32_t)R32(base, at);
  const uint32_t ch = Chara(base, slot);
  static uint32_t since[24];
  if (before != after) {
    REXKRNL_INFO("story4p: act {:08X} f{} slot {} kind {} step {} -> {} chara {:08X}", fn, g_frame, slot, kind,
                 before, after, ch);
    since[slot] = g_frame;
  }
  const uint32_t held = g_frame - since[slot];
  if (ch && held >= 240 && held % 240 == 0) {
    std::string b;
    for (uint32_t o : {0x28u, 0x2Cu, 0x30u, 0x34u, 0xD8u, 0xDCu, 0xE0u})
      b += fmt::format(" +{:x}={:08X}", o, R32(base, ch + o));
    REXKRNL_INFO("story4p: act {:08X} f{} slot {} step {} held {} frames:{} brain {:08X} x {} y {} z {}", fn,
                 g_frame, slot, after, held, b, R32(base, ch + 0x174), (int32_t)R32(base, ch + 0xC) >> 16,
                 (int32_t)R32(base, ch + 0x10) >> 16, (int32_t)R32(base, ch + 0x14) >> 16);
  }
}
}  // namespace
REX_HOOK_RAW(sub_8256EB60) { ActorScript(0x8256EB60, ctx, base, __imp__sub_8256EB60); }
REX_HOOK_RAW(sub_82572588) { ActorScript(0x82572588, ctx, base, __imp__sub_82572588); }
REX_HOOK_RAW(sub_82576980) { ActorScript(0x82576980, ctx, base, __imp__sub_82576980); }
REX_HOOK_RAW(sub_8257BEE0) { ActorScript(0x8257BEE0, ctx, base, __imp__sub_8257BEE0); }
REX_HOOK_RAW(sub_825806D8) { ActorScript(0x825806D8, ctx, base, __imp__sub_825806D8); }
REX_HOOK_RAW(sub_82581C00) { ActorScript(0x82581C00, ctx, base, __imp__sub_82581C00); }
REX_HOOK_RAW(sub_825841A0) { ActorScript(0x825841A0, ctx, base, __imp__sub_825841A0); }
REX_HOOK_RAW(sub_82585348) { ActorScript(0x82585348, ctx, base, __imp__sub_82585348); }
REX_HOOK_RAW(sub_82587080) { ActorScript(0x82587080, ctx, base, __imp__sub_82587080); }
REX_HOOK_RAW(sub_8258A3A0) { ActorScript(0x8258A3A0, ctx, base, __imp__sub_8258A3A0); }
REX_HOOK_RAW(sub_8258CD30) { ActorScript(0x8258CD30, ctx, base, __imp__sub_8258CD30); }
REX_HOOK_RAW(sub_8258F720) { ActorScript(0x8258F720, ctx, base, __imp__sub_8258F720); }
REX_HOOK_RAW(sub_82590FF8) { ActorScript(0x82590FF8, ctx, base, __imp__sub_82590FF8); }
REX_HOOK_RAW(sub_82593278) { ActorScript(0x82593278, ctx, base, __imp__sub_82593278); }
REX_HOOK_RAW(sub_82596758) { ActorScript(0x82596758, ctx, base, __imp__sub_82596758); }
REX_HOOK_RAW(sub_82598658) { ActorScript(0x82598658, ctx, base, __imp__sub_82598658); }
REX_HOOK_RAW(sub_8259B9B8) { ActorScript(0x8259B9B8, ctx, base, __imp__sub_8259B9B8); }
REX_HOOK_RAW(sub_825A3318) { ActorScript(0x825A3318, ctx, base, __imp__sub_825A3318); }
REX_HOOK_RAW(sub_825A6200) { ActorScript(0x825A6200, ctx, base, __imp__sub_825A6200); }
REX_HOOK_RAW(sub_825AA170) { ActorScript(0x825AA170, ctx, base, __imp__sub_825AA170); }
REX_HOOK_RAW(sub_825ACBF8) { ActorScript(0x825ACBF8, ctx, base, __imp__sub_825ACBF8); }
REX_HOOK_RAW(sub_825AEFE0) { ActorScript(0x825AEFE0, ctx, base, __imp__sub_825AEFE0); }
REX_HOOK_RAW(sub_825B0348) { ActorScript(0x825B0348, ctx, base, __imp__sub_825B0348); }
REX_HOOK_RAW(sub_825B2E88) { ActorScript(0x825B2E88, ctx, base, __imp__sub_825B2E88); }
REX_HOOK_RAW(sub_825B3748) { ActorScript(0x825B3748, ctx, base, __imp__sub_825B3748); }
REX_HOOK_RAW(sub_825B5098) { ActorScript(0x825B5098, ctx, base, __imp__sub_825B5098); }
REX_HOOK_RAW(sub_825B6AF8) { ActorScript(0x825B6AF8, ctx, base, __imp__sub_825B6AF8); }
REX_HOOK_RAW(sub_825B82B8) { ActorScript(0x825B82B8, ctx, base, __imp__sub_825B82B8); }
REX_HOOK_RAW(sub_825B9860) { ActorScript(0x825B9860, ctx, base, __imp__sub_825B9860); }
REX_HOOK_RAW(sub_825BB090) { ActorScript(0x825BB090, ctx, base, __imp__sub_825BB090); }
REX_HOOK_RAW(sub_825BD690) { ActorScript(0x825BD690, ctx, base, __imp__sub_825BD690); }
REX_HOOK_RAW(sub_825BE8E0) { ActorScript(0x825BE8E0, ctx, base, __imp__sub_825BE8E0); }
REX_HOOK_RAW(sub_825C0E98) { ActorScript(0x825C0E98, ctx, base, __imp__sub_825C0E98); }
REX_HOOK_RAW(sub_825C3948) { ActorScript(0x825C3948, ctx, base, __imp__sub_825C3948); }
REX_HOOK_RAW(sub_825C4C30) { ActorScript(0x825C4C30, ctx, base, __imp__sub_825C4C30); }
REX_HOOK_RAW(sub_825C6650) { ActorScript(0x825C6650, ctx, base, __imp__sub_825C6650); }
REX_HOOK_RAW(sub_825C74C0) { ActorScript(0x825C74C0, ctx, base, __imp__sub_825C74C0); }
REX_HOOK_RAW(sub_825C99E8) { ActorScript(0x825C99E8, ctx, base, __imp__sub_825C99E8); }
REX_HOOK_RAW(sub_825CB1F0) { ActorScript(0x825CB1F0, ctx, base, __imp__sub_825CB1F0); }
REX_HOOK_RAW(sub_825CD678) { ActorScript(0x825CD678, ctx, base, __imp__sub_825CD678); }
REX_HOOK_RAW(sub_82570F70) { ActorScript(0x82570F70, ctx, base, RunAsP2Hook); }

// ---------------------------------------------------------------------------------------------
// Quick start (tests): GH_QUICK=<stage> skips the menus and starts the story at that stage
// ---------------------------------------------------------------------------------------------
// Top-level modes (sub_8248A1F0): u16 0x829b2184 current, 0x829b2186 next, 0x829b2188 previous,
// u8 0x829b218a "just entered". Mode 4 = New Game screen, 8 = story stage. When the New Game screen
// ends it resets the work (sub_8249A868), sets work+0x3c4 bit 1 (Han -> kind 7), puts each chosen
// hero in the party (sub_824968F0: kind + level 1 block), then sub_82497898 / sub_82499F40(owner,0)
// and mode 8, which loads the stage in work+0x3af. The quick start does the same the first time the
// mode GH_QUICK_MODE (default 3, the main menu) is entered.
//   GH_QUICK=N        stage index as in the logs (0 = first stage, 1 = stage 2, ...)
//   GH_QUICK_PARTY    hero kind per player, default "2354" (Han, Randy, Nicole, Ginjirou);
//                     1 Serena, 2 Han, 3 Randy, 4 Ginjirou, 5 Nicole. Its length = number of players.
//   GH_QUICK_PADS     pad per player, default "0123".
// Works without GH_4P for 1-2 players; P3/P4 need GH_4P=1.
namespace {

constexpr uint32_t kModeCur = 0x829b2184, kModeNext = 0x829b2186;
constexpr uint32_t kOwner = 0x829caaf0;

void QuickStart(Guest& g, int stage) {
  uint8_t* base = g.base;
  const char* party = Env("GH_QUICK_PARTY");
  const char* pads = Env("GH_QUICK_PADS");
  if (!party) party = "2354";
  if (!pads) pads = "0123";
  const int n = (int)std::min<size_t>(std::strlen(party), Enabled() ? 4 : 2);

  g(sub_8249A868, kWork);
  W32(base, kWork + 0x3c4, (R32(base, kWork + 0x3c4) | 2) & ~4u);
  for (int p = 0; p < 4; p++) W8(base, kPadKind + p, 0xFF);
  for (int p = 0; p < n; p++) {
    const int kind = party[p] - '0';
    const int pad = (p < (int)std::strlen(pads)) ? pads[p] - '0' : p;
    if (kind < 1 || kind > 5) continue;
    if (p < 2) {
      g(sub_824968F0, kWork, (uint64_t)(p + 1), (uint64_t)kind);
    } else {
      const uint32_t slot = SlotFor(p);
      const uint32_t at = slot < 12 ? slot : (p == 2 ? 10u : 11u);
      g(sub_824968F0, kWork, at, (uint64_t)kind);
      if (at != slot) { W8(base, kKinds + slot, (uint8_t)kind); W8(base, kKinds + at, 0xFF); }
    }
    g(sub_82496728, kGlobalWork, (uint64_t)p, (uint64_t)pad);
  }
  W8(base, kWork + 0x3ae, 9);  // credits
  W8(base, kWork + 0x3af, (uint8_t)stage);
  g(sub_82497898, kOwner);
  g(sub_82499F40, kOwner, 0);
  for (uint32_t i = 0; i < 3; i++) {  // as the New Game exit: clear +6/+0xa of blocks 0..2
    W16(base, kWork + i * kBlock + 6, 0);
    W16(base, kWork + i * kBlock + 10, 0);
  }
  W16(base, kModeNext, 8);
  REXKRNL_INFO("story4p: quick start: stage {} party {} pads {}", stage, party, pads);
}

// ---------------------------------------------------------------------------------------------
// Parameter screen after a stage: a second pass for P3/P4
// ---------------------------------------------------------------------------------------------
// Mode 5 (sub_824892C0, modes 5/6) runs the parameter screen (sub_825022D8, param_edit.mrg) after
// each stage, after a continue and after loading a save. Its two panels serve seat i = 0/1, i.e.
// party i+1: kind work+0x3d9+i, block work+(i+1)*0x24, pad pad_kind[i]. Free points are
// level - points spent (block +0x15..+0x1a, sub_82500018), so P3/P4 only need their level in their
// block (kept by the sub_82499F40 hook). When the P1/P2 screen ends towards the stage (next mode 8),
// mode 5 is entered again (as the game itself does for result 3) with P3/P4 swapped into the seats;
// a missing P3 or P4 leaves its seat empty. The swap is undone when that screen ends.
// GH_4P_POINTS=0 turns this off.
constexpr uint32_t kModeEntered = 0x829b218a;

bool PointsPass() {
  static int v = -1;
  if (v < 0) { const char* s = Env("GH_4P_POINTS"); v = (s && *s == '0') ? 0 : 1; }
  return v == 1;
}

int g_points_pass;  // 0 = off, 1 = P1/P2 screen with P3/P4 waiting, 2 = P3/P4 screen
struct Seats {
  int player[2] = {-1, -1};  // player sitting in seat 0/1 during pass 2 (-1 = seat left empty)
  int8_t kind[2] = {}, pad[2] = {};
  uint8_t block[2][kBlock] = {};
} g_seats;

bool HasExtras(uint8_t* base) {
  for (int p = 2; p < MaxPlayers(); p++)
    if (S8(base, KindOf(p)) >= 0) return true;
  return false;
}

// Free points of a block (as sub_82500018 counts them).
int FreePoints(uint8_t* base, uint32_t blk) {
  int n = R8(base, blk + 0xe);
  for (uint32_t a = 0x15; a <= 0x1a; a++) n -= R8(base, blk + a);
  return n;
}

void LogExtras(uint8_t* base, const char* when) {
  for (int p = 2; p < 4; p++) {
    if (S8(base, KindOf(p)) < 0) continue;
    const uint32_t b = BlockOf(p);
    REXKRNL_INFO("story4p: points {} P{} kind {} level {} spent {} {} {} {} {} {} free {}", when, p + 1,
                 S8(base, KindOf(p)), R8(base, b + 0xe), R8(base, b + 0x15), R8(base, b + 0x16),
                 R8(base, b + 0x17), R8(base, b + 0x18), R8(base, b + 0x19), R8(base, b + 0x1a),
                 FreePoints(base, b));
  }
}

void SeatsIn(uint8_t* base) {
  int n = 0;
  g_seats.player[0] = g_seats.player[1] = -1;
  for (int p = 2; p < MaxPlayers(); p++)
    if (S8(base, KindOf(p)) >= 0) g_seats.player[n++] = p;
  for (int i = 0; i < 2; i++) {
    const uint32_t kind = kKinds + 1 + i, blk = kWork + (1 + i) * kBlock, pad = kPadKind + i;
    g_seats.kind[i] = S8(base, kind);
    g_seats.pad[i] = S8(base, pad);
    std::memcpy(g_seats.block[i], base + blk, kBlock);
    const int p = g_seats.player[i];
    if (p >= 0) {
      W8(base, kind, R8(base, KindOf(p)));
      W8(base, pad, R8(base, kPadKind + p));
      std::memcpy(base + blk, base + BlockOf(p), kBlock);
    } else {
      W8(base, kind, 0xFF);  // no panel for this seat
      W8(base, pad, 0xFF);
    }
  }
}

void SeatsOut(uint8_t* base) {
  for (int i = 0; i < 2; i++) {
    const uint32_t kind = kKinds + 1 + i, blk = kWork + (1 + i) * kBlock, pad = kPadKind + i;
    const int p = g_seats.player[i];
    if (p >= 0) std::memcpy(base + BlockOf(p), base + blk, kBlock);
    W8(base, kind, (uint8_t)g_seats.kind[i]);
    W8(base, pad, (uint8_t)g_seats.pad[i]);
    std::memcpy(base + blk, g_seats.block[i], kBlock);
  }
}

// ---------------------------------------------------------------------------------------------
// New Game screen: P3/P4 wait, then choose their hero
// ---------------------------------------------------------------------------------------------
// Mode 4 (sub_82489160) runs the New Game screen (sub_824F4678, new_game.mrg): two hero panels; a
// pad that is not P1's pressing START there becomes P2 (screen state 2, sub_824F33C0). While it
// runs, any other free pad pressing a button (directions do not count) is put in a queue and an
// overlay shows "Player N is waiting". When the screen ends towards the first stage (next mode 8),
// each queued pad goes through the game's own "P2 joins" screen (mode 6: hero choice + points),
// with P2 parked as for a join in a stage (Park/Unpark); with no P2 yet, the first one simply
// becomes P2. GH_4P_WAIT=0 turns this off.
bool WaitEnabled() {
  static int v = -1;
  if (v < 0) { const char* s = Env("GH_4P_WAIT"); v = (s && *s == '0') ? 0 : 1; }
  return v == 1 && Enabled() && MaxPlayers() > 2;
}

int g_wait[3];               // queued pads, in the order they pressed
int g_nwait;
bool g_wait_chain;           // a mode 6 screen started from the queue is running
std::atomic<int> g_wait_label[3];  // player number shown by the overlay (0 = nothing)
std::atomic<int> g_press_label[3];  // "Player N press START": connected pads that have not joined yet

void WaitPublish(int first_player) {
  for (int i = 0; i < 3; i++) g_wait_label[i] = i < g_nwait ? first_player + i : 0;
}

void PressClear() {
  for (auto& l : g_press_label) l = 0;
}

void WaitClear() {
  PressClear();
  g_nwait = 0;
  g_wait_chain = false;
  WaitPublish(0);
}

// Seats taken on the New Game screen: P1, plus P2 once a pad joined there.
int SeatedOnNewGame(uint8_t* base) { return S8(base, kPadKind + 1) >= 0 ? 2 : 1; }

void WaitScan(Guest& g) {
  uint8_t* base = g.base;
  for (int pad = 0; pad < 4; pad++) {
    if (SeatedOnNewGame(base) + g_nwait >= MaxPlayers()) break;
    bool taken = S8(base, kPadKind + 0) == pad || S8(base, kPadKind + 1) == pad;
    for (int i = 0; i < g_nwait; i++) taken |= g_wait[i] == pad;
    if (taken || !(g(sub_82419C38, pad) & 1)) continue;
    // Bits 0x01..0x80 are the directions (stick included), 0x1000 A, 0x2000 B, 0x400000 START...
    if (!(R32(base, g(sub_8241B800, pad)) & 0xFFFFFF00u)) continue;
    g_wait[g_nwait++] = pad;
    REXKRNL_INFO("story4p: pad {} waiting on the New Game screen", pad);
  }
  // Connected pads that are neither seated nor queued: the overlay asks them to press START, with
  // the seat they would get.
  int n = 0;
  for (int pad = 0; pad < 4 && n < 3; pad++) {
    const int player = SeatedOnNewGame(base) + g_nwait + n + 1;
    if (player > MaxPlayers()) break;
    bool taken = S8(base, kPadKind + 0) == pad || S8(base, kPadKind + 1) == pad;
    for (int i = 0; i < g_nwait; i++) taken |= g_wait[i] == pad;
    if (taken || !(g(sub_82419C38, pad) & 1)) continue;
    g_press_label[n++] = player;
  }
  for (int i = n; i < 3; i++) g_press_label[i] = 0;
}

// A queued pad that became P2 on the screen (it pressed START) leaves the queue.
void WaitDedupe(uint8_t* base) {
  int n = 0;
  for (int i = 0; i < g_nwait; i++)
    if (g_wait[i] != S8(base, kPadKind + 1)) g_wait[n++] = g_wait[i];
  g_nwait = n;
}

// Starts the join screen (next mode 6) for the first queued pad; false when nobody is left.
bool WaitNextJoin(Guest& g) {
  uint8_t* base = g.base;
  while (g_nwait > 0) {
    const int pad = g_wait[0];
    for (int i = 1; i < g_nwait; i++) g_wait[i - 1] = g_wait[i];
    g_nwait--;
    int seat = -1;  // 1 = P2, 2/3 = P3/P4
    if (S8(base, kPadKind + 1) < 0 && S8(base, kKinds + 2) < 0) {
      seat = 1;
    } else {
      for (int p = 2; p < MaxPlayers(); p++)
        if (S8(base, KindOf(p)) < 0) { seat = p; break; }
    }
    if (seat < 0) break;
    if (seat >= 2) Park(base, seat, pad);
    g(sub_82496728, kGlobalWork, 1, (uint64_t)pad);
    W8(base, kGlobalWork + 0x2c, 0);
    W16(base, kModeNext, 6);
    WaitPublish(seat + 2);
    REXKRNL_INFO("story4p: pad {} chooses a hero as P{}", pad, seat + 1);
    return true;
  }
  WaitClear();
  return false;
}

}  // namespace

// Mode 4: New Game screen (created on the first frame; the next mode is set when it ends).
REX_HOOK_RAW(sub_82489160) {
  if (!Enabled()) { __imp__sub_82489160(ctx, base); return; }  // mod off: the game's own function
  if (!WaitEnabled()) { __imp__sub_82489160(ctx, base); return; }
  const bool entered = R8(base, kModeEntered) != 0;
  if (entered) WaitClear();
  const PPCContext saved = ctx;
  if (!entered) {
    Guest g(ctx, base);
    WaitScan(g);
  }
  const uint16_t next = R16(base, kModeNext);
  W16(base, kModeNext, 0xFFFF);  // sentinel: the original writes the next mode when the screen ends
  __imp__sub_82489160(ctx, base);
  const uint16_t now = R16(base, kModeNext);
  WaitDedupe(base);
  if (now == 0xFFFF) {
    W16(base, kModeNext, next);
    WaitPublish(SeatedOnNewGame(base) + 1);
    return;
  }
  PressClear();  // the screen ended
  if (now == 8 && g_nwait > 0) {
    Guest g(const_cast<PPCContext&>(saved), base);
    g_wait_chain = WaitNextJoin(g);
  } else {
    WaitClear();
  }
}

// Hooked in cheats.cpp, which calls this in place of the original.
extern "C" REX_FUNC(Story4pHook_sub_8248A1F0) {
  if (!Enabled()) { __imp__sub_8248A1F0(ctx, base); return; }  // mod off: the game's own function
  const PPCContext saved = ctx;
  __imp__sub_8248A1F0(ctx, base);
  static int quick = -2, trigger = 3;
  static bool done = false;
  static uint16_t last = 0xffff;
  if (quick == -2) {
    const char* q = Env("GH_QUICK");
    const char* m = Env("GH_QUICK_MODE");
    quick = q ? std::atoi(q) : -1;
    if (m) trigger = std::atoi(m);
  }
  // Route log (always): story stage changes, and the mode when it changes.
  {
    static int last_stage = -1;
    static uint16_t last_mode = 0xffff;
    const int st = R8(base, kWork + 0x3af);
    const uint16_t md = R16(base, kModeCur);
    if (md != last_mode || st != last_stage) {
      REXKRNL_INFO("story4p: route mode {} stage {} next {}", (int)md, st, (int)R8(base, kWork + 0x3b0));
      last_mode = md;
      last_stage = st;
    }
  }
  if (quick < 0) return;
  const uint16_t cur = R16(base, kModeCur);
  if (cur != last) {
    REXKRNL_INFO("story4p: mode {} -> {}", last == 0xffff ? -1 : (int)last, (int)cur);
    last = cur;
    if (cur == trigger && !done) {
      done = true;
      Guest g(const_cast<PPCContext&>(saved), base);
      QuickStart(g, quick);
    }
  }
}

// Modes 5/6: parameter screen (created on the first frame; the next mode is set when it ends).
REX_HOOK_RAW(sub_824892C0) {
  if (!Enabled()) { __imp__sub_824892C0(ctx, base); return; }  // mod off: the game's own function
  if (Enabled() && g_wait_chain) {  // join screen started from the New Game queue
    const PPCContext saved = ctx;
    const uint16_t next = R16(base, kModeNext);
    W16(base, kModeNext, 0xFFFF);
    __imp__sub_824892C0(ctx, base);
    const uint16_t now = R16(base, kModeNext);
    if (now == 0xFFFF) { W16(base, kModeNext, next); return; }
    if (now == 6) return;  // the game shows the join screen again
    Unpark(base);
    g_wait_chain = false;
    if (now == 8 && g_nwait > 0) {
      Guest g(const_cast<PPCContext&>(saved), base);
      g_wait_chain = WaitNextJoin(g);
    } else {
      WaitClear();
    }
    return;
  }
  if (!Enabled() || !PointsPass() || !Story(base)) { __imp__sub_824892C0(ctx, base); return; }
  const uint16_t mode = R16(base, kModeCur);
  if (R8(base, kModeEntered) && mode == 5) {
    if (g_points_pass == 2) {
      LogExtras(base, "before");
      SeatsIn(base);
      REXKRNL_INFO("story4p: parameter screen for P3/P4 (seats: P{} P{})", g_seats.player[0] + 1,
                   g_seats.player[1] + 1);
    } else {
      g_points_pass = HasExtras(base) ? 1 : 0;
    }
  }
  const uint16_t next = R16(base, kModeNext);
  W16(base, kModeNext, 0xFFFF);  // sentinel: the original writes the next mode when the screen ends
  __imp__sub_824892C0(ctx, base);
  const uint16_t now = R16(base, kModeNext);
  if (now == 0xFFFF) { W16(base, kModeNext, next); return; }
  if (g_points_pass == 2) {
    SeatsOut(base);
    g_points_pass = 0;
    LogExtras(base, "after");
  } else if (g_points_pass == 1 && mode == 5 && now == 8) {
    W16(base, kModeNext, 5);  // once more, for P3/P4
    g_points_pass = 2;
  } else {
    g_points_pass = 0;
  }
}

// Parameter screen constructor (this, join mode). Outside the join flow, a pad that is not P1's
// pressing START on the screen joins as P2 (screen +0x2c, sub_82500528); with P3/P4 in the game that
// pad may be theirs, and in the P3/P4 pass the seats hold other players. Players join in the stage.
REX_HOOK_RAW(sub_825022D8) {
  if (!Enabled()) { __imp__sub_825022D8(ctx, base); return; }  // mod off: the game's own function
  const bool join = ctx.r4.u32 & 0xff;
  __imp__sub_825022D8(ctx, base);
  if (Enabled() && !join && g_points_pass && ctx.r3.u32) W8(base, ctx.r3.u32 + 0x2c, 0);
}

// Debug (GH_4P_LOGCREATE=1): every character creation (slot, kind, level, brain code).
REX_HOOK_RAW(sub_8247A530) {
  if (!Enabled()) { __imp__sub_8247A530(ctx, base); return; }  // mod off: the game's own function
  const uint32_t d = ctx.r3.u32;
  static int on = -1;
  if (on < 0) on = Env("GH_4P_LOGCREATE") ? 1 : 0;
  if (on) {
    REXKRNL_INFO("story4p: create slot {} kind {} lvl {} brain {:#x} stage {} event {}", S8(base, d), R8(base, d + 2),
                 R8(base, d + 6), R16(base, d + 8), R8(base, kWork + 0x3af), R8(base, kWork + 0x3b4));
    // Resource manager 0x829CC79C: +0x18/+0x1c vector of packs (+0 data, +4 std::string name, +0x20 purge).
    constexpr uint32_t kRes = 0x829CC79C;
    std::string packs;
    for (uint32_t it = R32(base, kRes + 0x18); it && it < R32(base, kRes + 0x1c); it += 4) {
      const uint32_t e = R32(base, it);
      uint32_t str = e + 4;
      if (R32(base, e + 0x18) > 0xf) str = R32(base, str);
      packs += fmt::format(" {}{}{}", (const char*)(base + str), R32(base, e) ? "" : "(-)", R8(base, e + 0x20) ? "*" : "");
    }
    REXKRNL_INFO("story4p:   packs:{}", packs);
  }
  __imp__sub_8247A530(ctx, base);
}

// Debug: failed file-buffer allocations (the async loader retries them every frame).
REX_HOOK_RAW(sub_82418928) {
  if (!Enabled()) { __imp__sub_82418928(ctx, base); return; }  // mod off: the game's own function
  const uint32_t size = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_82418928(ctx, base);
  static uint32_t fails;
  if (ctx.r3.u32 == 0 && (fails++ % 300) == 0)
    REXKRNL_WARN("story4p: file buffer alloc failed: {:#x} bytes (type {}), {} failures", size, type, fails);
}


#if !defined(__SWITCH__)
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

// "Player N is waiting" over the New Game screen and the join screens that follow it, and at the
// top of the New Game screen "Player N press START" for connected pads that have not joined.
namespace {
class WaitOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit WaitOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    DrawPressStart(io);
    char text[96];
    int len = 0;
    for (int i = 0; i < 3; i++) {
      const int n = g_wait_label[i];
      if (n <= 0) continue;
      len += std::snprintf(text + len, sizeof(text) - len, "%sPlayer %d is waiting", len ? "\n" : "", n);
    }
    if (!len) return;
    const float scale = std::max(1.5f, io.DisplaySize.y / 280.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.93f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##gh4p_wait", nullptr, flags)) {
      ImGui::SetWindowFontScale(scale);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
      ImGui::TextUnformatted(text);
      ImGui::PopStyleColor();
    }
    ImGui::End();
  }

 private:
  static constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoFocusOnAppearing;

  void DrawPressStart(ImGuiIO& io) {
    char text[96];
    int len = 0;
    for (int i = 0; i < 3; i++) {
      const int n = g_press_label[i];
      if (n <= 0) continue;
      len += std::snprintf(text + len, sizeof(text) - len, "%sPlayer %d press START", len ? "    " : "", n);
    }
    if (!len) return;
    const float scale = std::max(1.5f, io.DisplaySize.y / 280.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##gh4p_press", nullptr, kFlags)) {
      ImGui::SetWindowFontScale(scale);
      // Blink (0.6 s on, 0.4 s off), like an arcade "press start".
      const float a = std::fmod((float)ImGui::GetTime(), 1.0f) < 0.6f ? 1.0f : 0.35f;
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, a));
      ImGui::TextUnformatted(text);
      ImGui::PopStyleColor();
    }
    ImGui::End();
  }
};
}  // namespace

// Called by GhApp::OnCreateDialogs; the dialog is deleted with the drawer.
void Story4pCreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (Enabled()) new WaitOverlay(drawer);
}
#endif

// Route choice menu (constructor 0x824D0EF8: r3 = menu, r4 = menu id 0..18; tables at 0x82915408,
// option index at +0xB84, chosen stage written to work+0x3B0 when A is pressed).
// GH_ROUTE="id:opt,id:opt,...": preselect option opt of menu id (tests confirm it with the automatic A).
REX_HOOK_RAW(sub_824D0EF8) {
  if (!Enabled()) { __imp__sub_824D0EF8(ctx, base); return; }  // mod off: the game's own function
  const uint32_t menu = ctx.r3.u32;
  const int id = (int)ctx.r4.s32;
  __imp__sub_824D0EF8(ctx, base);
  int opt = -1;
  if (const char* r = Env("GH_ROUTE")) {
    for (const char* q = r; *q;) {
      int a = std::atoi(q);
      const char* c = std::strchr(q, ':');
      if (!c) break;
      if (a == id) { opt = std::atoi(c + 1); break; }
      const char* n = std::strchr(c, ',');
      if (!n) break;
      q = n + 1;
    }
  }
  const int count = (id >= 0 && id < 19) ? (int)R32(base, 0x8291540C + id * 8) : 0;
  if (opt >= count) opt = count - 1;
  if (opt > 0) W32(base, menu + 0xB84, (uint32_t)opt);
  std::string opts;
  const uint32_t tab = (id >= 0 && id < 19) ? R32(base, 0x82915408 + id * 8) : 0;
  for (int i = 0; i < count; i++) opts += fmt::format(" {}", (int)R16(base, tab + i * 4));
  REXKRNL_INFO("story4p: route menu {} at stage {} options{} preselect {}", id, (int)R8(base, kWork + 0x3af),
               opts, opt < 0 ? 0 : opt);
}

// Breakable props: keep the list used by the walking AI (UpdateIdle).
// Every stage prop goes through one of the two base constructors (the subclasses call them first),
// whether the stage table (sub_824D4A48) or a stage script creates it.
REX_HOOK_RAW(sub_82443478) {
  if (!Enabled()) { __imp__sub_82443478(ctx, base); return; }  // mod off: the game's own function
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82443478(ctx, base);
  AddProp(base, obj);
}
REX_HOOK_RAW(sub_82443F28) {
  if (!Enabled()) { __imp__sub_82443F28(ctx, base); return; }  // mod off: the game's own function
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82443F28(ctx, base);
  AddProp(base, obj);
}
REX_HOOK_RAW(sub_82443550) {
  if (!Enabled()) { __imp__sub_82443550(ctx, base); return; }  // mod off: the game's own function
  RemoveProp(ctx.r3.u32);
  __imp__sub_82443550(ctx, base);
}
REX_HOOK_RAW(sub_82443FF8) {
  if (!Enabled()) { __imp__sub_82443FF8(ctx, base); return; }  // mod off: the game's own function
  RemoveProp(ctx.r3.u32);
  __imp__sub_82443FF8(ctx, base);
}
