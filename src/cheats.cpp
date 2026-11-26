// Cheats: player health / magic at maximum, 99 continues, all attributes at maximum.
// Guest addresses and logic come from the fork R19988088/guardian-heroes-recomp-public-c (46014a3),
// re-written for this port: no pause-menu page, no ABI patch. Switches: cvars cheat_* (gh.toml and the PC
// settings window) and, on the Switch, sdmc:/switch/gh/cheats.txt (src/cheats_config.h).
// Hooks: function hooks (REX_HOOK_RAW) and the midasm hooks of gh_manifest.toml (GhKeepHealthMax below).
#include "generated/default/gh_pch.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "cheats_config.h"

REXCVAR_DEFINE_BOOL(cheat_invincible, false, "Cheats", "Player health stays at its maximum");
REXCVAR_DEFINE_BOOL(cheat_mp, false, "Cheats", "Player magic stays at its maximum");
REXCVAR_DEFINE_BOOL(cheat_continues, false, "Cheats", "99 continues, never used up");
REXCVAR_DEFINE_BOOL(cheat_attributes, false, "Cheats", "All six attributes at their maximum");

DECLARE_REX_FUNC(sub_8243EEA0);  // max health from vitality
DECLARE_REX_FUNC(sub_82440228);  // max magic from mentality
DECLARE_REX_FUNC(sub_8243EEC0);
DECLARE_REX_FUNC(sub_8243FDC8);
DECLARE_REX_FUNC(sub_82495BC0);
DECLARE_REX_FUNC(sub_824DEA90);  // scene update, once per frame
DECLARE_REX_FUNC(sub_82478988);  // character update
DECLARE_REX_FUNC(sub_824411C0);  // hit damage
DECLARE_REX_FUNC(sub_824E4308);  // continue screen
DECLARE_REX_FUNC(sub_82468FA8);  // the four attribute rebuilds
DECLARE_REX_FUNC(sub_824418C8);
DECLARE_REX_FUNC(sub_824429A8);
DECLARE_REX_FUNC(sub_82441248);
DECLARE_REX_FUNC(sub_824E6730);  // Stage::Story::set_load_async(stage index)
DECLARE_REX_FUNC(sub_82495B30);
DECLARE_REX_FUNC(sub_8242AEA0);  // destroy the scene in a scene slot
DECLARE_REX_FUNC(sub_8248A1F0);

#if defined(__SWITCH__)
extern "C" {
extern volatile int rex_nx_cheat_invincible, rex_nx_cheat_mp, rex_nx_cheat_continues, rex_nx_cheat_attributes;
extern volatile int rex_nx_start_level, rex_nx_apply_level;
void rex_nx_load_settings(void);
extern volatile uint64_t rex_nx_guest_poll_ticks;
}
#include <switch.h>
// The [dk] perf line: sub_825D6A68 is the game's own GPU poll (one step of its spin loop, 6 callers). The time in it
// is the time the game's thread waits for the command processor.
DECLARE_REX_FUNC(sub_825D6A68);
REX_HOOK_RAW(sub_825D6A68) {
  const uint64_t t0 = armGetSystemTick();
  __imp__sub_825D6A68(ctx, base);
  rex_nx_guest_poll_ticks = rex_nx_guest_poll_ticks + (armGetSystemTick() - t0);
}
#endif

// 4-player story mode (story4p.cpp) shares these hooks; it calls the originals.
extern "C" REX_FUNC(Story4pHook_sub_8248A1F0);
extern "C" REX_FUNC(Story4pHook_sub_824DEA90);
extern "C" REX_FUNC(Story4pHook_sub_824E4308);
extern "C" REX_FUNC(Story4pHook_sub_82468FA8);

namespace {
constexpr uint32_t kGame = 0x829CBFE8;  // game state (what sub_82495B30 returns); +942 continues, +1672 player slots

// Switch: the in-game menu owns the switches (switch/host/input/nx_input_driver.cpp, saved in settings.txt; it also
// reads a hand-made cheats.txt). PC: the cvars.
#if defined(__SWITCH__)
bool On(volatile int& flag) { rex_nx_load_settings(); return flag != 0; }
bool CheatInvincible() { return On(rex_nx_cheat_invincible); }
bool CheatMp() { return On(rex_nx_cheat_mp); }
bool CheatContinues() { return On(rex_nx_cheat_continues); }
bool CheatAttributes() { return On(rex_nx_cheat_attributes); }
#else
bool CheatInvincible() { return REXCVAR_GET(cheat_invincible); }
bool CheatMp() { return REXCVAR_GET(cheat_mp); }
bool CheatContinues() { return REXCVAR_GET(cheat_continues); }
bool CheatAttributes() { return REXCVAR_GET(cheat_attributes); }
#endif

// A character owned by a human player: input source 1 (0 and 2 are inactive / AI) and listed in the player slots.
bool IsPlayer(uint8_t* base, uint32_t character) {
  if (!character || REX_LOAD_U8(character + 1166) != 1) return false;
  for (uint32_t slot = 0; slot < 12; ++slot) {
    const uint32_t node = REX_LOAD_U32(kGame + 1672 + slot * 8 + 4);
    if (node && REX_LOAD_U32(node) == character) return true;
  }
  return false;
}

// Health to maximum, also when the damage just took it to 0 (that is the point: no death).
bool KeepHealth(uint8_t* base, uint32_t character) {
  if (!CheatInvincible() || !IsPlayer(base, character)) return false;
  const int16_t maximum = REX_LOAD_U16(character + 204);
  if (maximum <= 0) return false;
  REX_STORE_U16(character + 202, maximum);
  return true;
}

// Health and magic of a living player (a dead one is not revived).
void RefillPlayer(uint8_t* base, uint32_t character) {
  if (!IsPlayer(base, character) || static_cast<int16_t>(REX_LOAD_U16(character + 202)) <= 0) return;
  KeepHealth(base, character);
  if (CheatMp()) {
    const int16_t maximum = REX_LOAD_U16(character + 208);
    if (maximum > 0) REX_STORE_U16(character + 206, maximum);
  }
}

void RefillContinues(uint8_t* base) {
  if (CheatContinues()) REX_STORE_U8(kGame + 942, 99);
}

// A context for calling native functions from a hook: own stack area below the caller's.
PPCContext CallContext(const PPCContext& context) {
  PPCContext call{};
  call.r1.u32 = context.r1.u32 - 0x400;
  call.r13 = context.r13;
  call.fpscr = context.fpscr;
  return call;
}

// Attributes: [226..231] = strength, vitality, intelligence, mentality, agility, luck (the fork's order).
struct AttributeOverride {
  uint32_t character = 0;
  uint32_t handle = 0;
  std::array<uint8_t, 6> natural{}, applied{};
};
std::array<AttributeOverride, 12> attributes{};

void SetMaximum(uint8_t* base, uint32_t character, uint32_t offset, uint16_t maximum) {
  REX_STORE_U16(character + offset, maximum);
  const int16_t current = REX_LOAD_U16(character + offset - 2);
  if (current > static_cast<int16_t>(maximum)) REX_STORE_U16(character + offset - 2, maximum);
}

// Recompute what depends on vitality / mentality / agility with the game's own functions.
void RefreshAttributes(const PPCContext& context, uint8_t* base, uint32_t character,
                       const std::array<uint8_t, 6>& before) {
  auto call = CallContext(context);
  const uint32_t level = REX_LOAD_U8(character + 225);
  const uint32_t vitality = REX_LOAD_U8(character + 227);
  const uint32_t mentality = REX_LOAD_U8(character + 229);
  if (before[1] != vitality) {
    call.r3.u32 = character; call.r4.u32 = level; call.r5.u32 = vitality;
    sub_8243EEA0(call, base);
    SetMaximum(base, character, 204, call.r3.u16);
  }
  if (before[3] != mentality) {
    call.r3.u32 = character; call.r4.u32 = level; call.r5.u32 = mentality;
    sub_82440228(call, base);
    SetMaximum(base, character, 208, call.r3.u16);
  }
  if (before[1] != vitality || before[3] != mentality) {
    call.r3.u32 = character; call.r4.u32 = level;
    call.r5.u32 = mentality; call.r6.u32 = vitality;
    sub_8243EEC0(call, base);
    SetMaximum(base, character, 212, call.r3.u16);
  }
  if (before[2] != REX_LOAD_U8(character + 228)) {
    call.r3.u32 = character;
    sub_8243FDC8(call, base);
  }
}

// Raise the attributes to the game's cap (32, plus the item bonus when the game allows it); keep the natural
// values so they can be put back when the cheat is off or before the game rebuilds the character.
void UpdateAttributes(const PPCContext& context, uint8_t* base) {
  const bool on = CheatAttributes();
  for (uint32_t slot = 0; slot < attributes.size(); ++slot) {
    auto& state = attributes[slot];
    const uint32_t handle = REX_LOAD_U32(kGame + 1672 + slot * 8);
    const uint32_t node = REX_LOAD_U32(kGame + 1676 + slot * 8);
    const uint32_t character = node ? REX_LOAD_U32(node) : 0;
    if (!character) { state = {}; continue; }  // slot empty: the character is gone
    // A failed player check for a frame keeps the saved natural values (never store raised ones as natural).
    if (!IsPlayer(base, character)) continue;
    std::array<uint8_t, 6> before;
    for (uint32_t i = 0; i < 6; ++i) before[i] = REX_LOAD_U8(character + 226 + i);
    if (state.character != character || state.handle != handle) state = {character, handle, before, before};
    const uint32_t resource = REX_LOAD_U32(character + 168);
    for (uint32_t i = 0; i < 6; ++i) {
      if (before[i] != state.applied[i]) state.natural[i] = before[i];  // the game changed it (level up)
      uint32_t maximum = 32;
      if (REX_LOAD_U8(kGame + 1177) & 2) {
        maximum += REX_LOAD_U8(resource + 14 + i);
        if (REX_LOAD_U8(character + 84) == 7) {
          auto call = CallContext(context);
          call.r3.u32 = call.r1.u32 + 0x100;  // output buffer: inside the reserved area above the callee's stack
          sub_82495BC0(call, base);
          maximum += REX_LOAD_U8(call.r1.u32 + 0x100 + i);
        }
      }
      state.applied[i] = on ? std::max<uint32_t>(state.natural[i], maximum) : state.natural[i];
      REX_STORE_U8(character + 226 + i, state.applied[i]);
    }
    RefreshAttributes(context, base, character, before);
  }
}

void BeforeAttributeRebuild(const PPCContext& context, uint8_t* base, uint32_t character) {
  for (auto& state : attributes) {
    if (state.character != character) continue;
    for (uint32_t i = 0; i < 6; ++i) REX_STORE_U8(character + 226 + i, state.natural[i]);
    RefreshAttributes(context, base, character, state.applied);
    state = {};
  }
}

void ApplyCheats(const PPCContext& context, uint8_t* base) {
  UpdateAttributes(context, base);
  RefillContinues(base);
  for (uint32_t slot = 0; slot < 12; ++slot) {
    const uint32_t node = REX_LOAD_U32(kGame + 1676 + slot * 8);
    if (node) RefillPlayer(base, REX_LOAD_U32(node));
  }
}
}  // namespace

// Midasm hooks (gh_manifest.toml): right after the game wrote the health in its damage code.
void GhKeepHealthMax(PPCRegister& character) {
  KeepHealth(rex::system::kernel_memory()->virtual_membase(), character.u32);
}

// Same, at a store that takes its value from r11: r11 gets the health (now the maximum).
void GhKeepSharedHealthMax(PPCRegister& character, PPCRegister& health) {
  auto* base = rex::system::kernel_memory()->virtual_membase();
  if (KeepHealth(base, character.u32)) health.u64 = REX_LOAD_U16(character.u32 + 202);
}

// Menu "apply" (Switch): jump to the chosen stage by running the game's own "Story > New game confirmed" sequence.
// Evidence (generated code):
//  - The game's top-level mode machine is sub_8248A1F0: it calls sub_82435950 on the state record at 0x829B2184, then
//    switches on the u16 state at +0 (case 4 = sub_82489160 new game, case 8 = sub_824894E8, case 3 = sub_82488F18 ...).
//  - sub_82435950: when +7 != 0 or +2 != +0 it copies +0 -> +4, +2 -> +0, sets "entered" +6 = 1 and clears +7.
//    So a handler (and we) request a state by writing +2 (and +7 = 1 to force it).
//  - sub_82489160 (state 4) when its dialog result is 1 does: sub_82495AF0, sub_82497898, sub_82495AF0, sub_82499F40(r4=0),
//    zeroes u16 +6 and +10 of the three 36-byte records at game state +0, then requests state 8 with +7 = 1.
//    Before that, on entry, it clears the game state (sub_8249A868), sets/clears flags in +964 and stores the stage byte.
// GhApplyJump requests state 4 from the mode machine's own hook (the game thread, between two state steps).
// The log lines "[gh] mode state A -> B" record the transitions.
#if defined(__SWITCH__)
constexpr uint32_t kMode = 0x829B2184;
// The game's own stage select (what sub_82495B90 returns): +15 on (u8), +16 index (u32) into the table at 0x82017AE0
// (entry i holds stage i). sub_82489160 (state 4) reads it for the stage byte. sub_824894E8 (state 8) reads it again
// on entry, clears +15, clears +964 bit 2 for stages after 1-3, resets the story stats (sub_82496DF8) and starts
// the story scene.
constexpr uint32_t kSelect = 0x82913D0C;
// gh: the start level applies once per visit of the title. 0.111 wrote it at every story entry (sub_82500128 also runs
// between two stages), so the chosen stage started again and again. g_level_armed: the next story entry may still
// apply start_level (Continue from the title); a new game, a Continue or a stage load clears it, the title sets it.
// g_jump_level: the level of "go to level" for the new-game hook (it must win over start_level).
bool g_level_armed = true;
int g_jump_level = 0;
void ConsumeLevel(const char* why) {
  if (!g_level_armed) return;
  g_level_armed = false;
  std::printf("[gh] start level consumed (%s): the story goes on to the next stage\n", why);
}

void SelectStage(uint8_t* base, int level) {
  REX_STORE_U32(kSelect + 16, static_cast<uint32_t>(level - 1));
  REX_STORE_U8(kSelect + 15, 1);
}

void GhApplyJump(const PPCContext& ctx, uint8_t* base) {
  const int level = rex_nx_apply_level;
  if (!level) return;
  rex_nx_apply_level = 0;
  std::printf("[gh] apply start level: %d (mode state %u, request %u, flag6 %u)\n", level, REX_LOAD_U16(kMode),
              REX_LOAD_U16(kMode + 2), REX_LOAD_U8(kMode + 6));
  if (level < 1 || level > 40) { std::printf("[gh] apply start level: out of range, ignored\n"); std::fflush(stdout); return; }
  // The old jump forced state 8 by hand. It skipped the new-game scene (sub_824F48E8: character select, stage load
  // sub_824F3328), left +964 bit 2 set for every stage and never destroyed the running scene (sub_8242AFD0 only drops
  // the holder of the old scene). Now: destroy the scene the way every state handler does before it leaves
  // (sub_8242AEA0 on the scene slot), turn the game's own stage select on and enter state 4 (new game). States 4 and 8
  // do the rest themselves. Not proven without hardware: that the scene destructor is safe in the middle of a stage.
  auto call = CallContext(ctx);
  call.r3.u32 = kMode + 12;
  sub_8242AEA0(call, base);
  SelectStage(base, level);
  g_jump_level = level;
  REX_STORE_U16(kMode + 2, 4);
  REX_STORE_U8(kMode + 7, 1);
  std::printf("[gh] apply: scene destroyed, stage select %d, requested mode state 4\n", level - 1);
  std::fflush(stdout);
}
#else
void GhApplyJump(const PPCContext&, uint8_t*) {}
#endif

REX_HOOK_RAW(sub_8248A1F0) {
#if defined(__SWITCH__)
  static uint16_t last_state = 0xFFFF;
  GhApplyJump(ctx, base);
  Story4pHook_sub_8248A1F0(ctx, base);  // story4p.cpp, which calls the original
  const uint16_t now = REX_LOAD_U16(kMode);
  if (now != last_state) {
    if (now == 3 && !g_level_armed) { g_level_armed = true; std::printf("[gh] title: start level armed again\n"); }
    std::printf("[gh] mode state %u -> %u (stage byte %u)\n", last_state, now, REX_LOAD_U8(kGame + 0x3AF));
    std::fflush(stdout);
    last_state = now;
  }
#else
  Story4pHook_sub_8248A1F0(ctx, base);  // story4p.cpp, which calls the original
#endif
}


REX_HOOK_RAW(sub_824DEA90) {
  ApplyCheats(ctx, base);
  Story4pHook_sub_824DEA90(ctx, base);  // story4p.cpp, which calls the original
}

REX_HOOK_RAW(sub_82478988) {
  const uint32_t character = ctx.r3.u32;
  RefillPlayer(base, character);
  __imp__sub_82478988(ctx, base);
  RefillPlayer(base, character);
}

REX_HOOK_RAW(sub_824411C0) {
  const uint32_t character = ctx.r3.u32;
  KeepHealth(base, character);
  __imp__sub_824411C0(ctx, base);
  KeepHealth(base, character);
}

REX_HOOK_RAW(sub_824E4308) {
  RefillContinues(base);
  Story4pHook_sub_824E4308(ctx, base);  // story4p.cpp, which calls the original
  RefillContinues(base);
}

// The game builds a character's attributes itself (new game, level up, load): it must see the natural values.
#define ATTRIBUTE_REBUILD(name)                          \
  REX_HOOK_RAW(name) {                                   \
    BeforeAttributeRebuild(ctx, base, ctx.r3.u32);       \
    __imp__##name(ctx, base);                            \
  }
// sub_82468FA8 is also hooked by story4p.cpp: call its handler, which calls the original.
REX_HOOK_RAW(sub_82468FA8) {
  BeforeAttributeRebuild(ctx, base, ctx.r3.u32);
  Story4pHook_sub_82468FA8(ctx, base);
}
ATTRIBUTE_REBUILD(sub_824418C8)
ATTRIBUTE_REBUILD(sub_824429A8)
ATTRIBUTE_REBUILD(sub_82441248)

// Level select (Switch setting start_level, 0 = normal flow). Evidence from the unpacked XEX image:
//  - sub_82489160 starts a new story game: it calls sub_8249A868 (clears the game state, which zeroes the stage
//    byte at game state +0x3AF) and, when the stage-select flag is set, stores the chosen stage there (0x824891E8).
//  - sub_824E6730 (Stage::Story::set_load_async, string at 0x8201DC3C) takes the stage byte as r3, logs
//    "[%s] Stage : %s<%d>" with the name from the table at 0x82914668 (1-1, 1-2, 1-3, 2-1, ...) and loads it;
//    its callers pass lbz 0x3AF(sub_82495B30()) (sub_824F3328, 0x82500178).
// So the stage byte after sub_82489160 decides the first stage. The midasm hook runs at 0x824891EC, after both
// paths joined. Not proven without hardware: that every stage index loads from a fresh game (the log lines say).
#if defined(__SWITCH__)
void GhStartLevel() {
  rex_nx_load_settings();
  auto* base = rex::system::kernel_memory()->virtual_membase();
  const int level = g_jump_level ? g_jump_level : rex_nx_start_level;
  g_jump_level = 0;
  const uint8_t before = REX_LOAD_U8(kGame + 0x3AF);
  if (level >= 1 && level <= 40) {
    REX_STORE_U8(kGame + 0x3AF, static_cast<uint8_t>(level - 1));
    SelectStage(base, level);  // state 8 then runs the game's own stage-select setup
  }
  std::printf("[gh] new game: stage byte %u -> %u (start level %d)\n", before, REX_LOAD_U8(kGame + 0x3AF), level);
  ConsumeLevel("new game");
  std::fflush(stdout);
}
#else
void GhStartLevel() {}
#endif

// Evidence log: every story stage load (stage index, stage byte), to check the hook point on the console.
REX_HOOK_RAW(sub_824E6730) {
  std::printf("[gh] stage load: index %d (game state byte +0x3AF = %u, +964 %08X, caller %08X)\n",
              static_cast<int>(ctx.r3.s32), REX_LOAD_U8(kGame + 0x3AF), REX_LOAD_U32(kGame + 964),
              static_cast<uint32_t>(ctx.lr));
  std::fflush(stdout);
#if defined(__SWITCH__)
  ConsumeLevel("stage load");
#endif
  __imp__sub_824E6730(ctx, base);
}

// Continue / new game with start_level set. Evidence (generated code): sub_824F3328 and sub_82500128 are the two
// story-entry steps that read the stage byte (game state +0x3AF) and pass it to sub_824E6730 (the 25 other callers
// pass +0x3B0, the "next stage" byte, which the game computes after the first stage; the console log shows the first
// load "index 7 (byte 7)", the later ones "index 8 (byte 7)"). The override is written into memory only, before the
// original runs, so the save file on disk is not touched (a later autosave may store the chosen stage).
#if defined(__SWITCH__)
namespace {
void OverrideStoryEntry(uint8_t* base, const char* where) {
  rex_nx_load_settings();
  const int level = rex_nx_start_level;
  const uint8_t before = REX_LOAD_U8(kGame + 0x3AF);
  static uint8_t last_logged = 0xFF;
  if (g_level_armed && level >= 1 && level <= 40 && before != level - 1) {
    REX_STORE_U8(kGame + 0x3AF, static_cast<uint8_t>(level - 1));
    std::printf("[gh] continue: stage %u -> %u (start_level %d, in %s)\n", before, level - 1, level, where);
    ConsumeLevel("continue");
    std::fflush(stdout);
  } else if (before != last_logged) {
    std::printf("[gh] story entry in %s: stage byte %u, start_level %d (unchanged)\n", where, before, level);
    std::fflush(stdout);
  }
  last_logged = REX_LOAD_U8(kGame + 0x3AF);
}
}  // namespace
#else
namespace { void OverrideStoryEntry(uint8_t*, const char*) {} }
#endif

DECLARE_REX_FUNC(sub_824F3328);
DECLARE_REX_FUNC(sub_82500128);

REX_HOOK_RAW(sub_824F3328) {
  OverrideStoryEntry(base, "sub_824F3328");
  __imp__sub_824F3328(ctx, base);
}

REX_HOOK_RAW(sub_82500128) {
  OverrideStoryEntry(base, "sub_82500128");
  __imp__sub_82500128(ctx, base);
}
