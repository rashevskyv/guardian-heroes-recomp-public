#include <rex/input/nx/nx_input_driver.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <rex/input/flags.h>
#include <rex/logging.h>

#include "../../../src/cheats_config.h"  // gh: GhCheatFlags, GhCheatParseLine, GhCheatFormat
#include "alt_controls.h"                // gh: GhAltControls

// gh: the settings menu drawn by the deko3d backend (src/gpu/deko3d_backend.cpp) reads these.
extern "C" {
volatile int rex_nx_menu_open = 0;    // 1 while the menu is up: the game sees a neutral pad
volatile int rex_nx_menu_cursor = 0;  // main page: 0 resume, 1 fps, 2 sound, 3 overlay, 4 hatching, 5 start level, 6 go to level, 7 cheats, 8 mods; cheats page: 0-3 cheats, 4 back; mods page: one line per mod, then back
volatile int rex_nx_menu_page = 0;    // 0 main, 1 cheats, 2 mods
volatile int rex_nx_show_fps = 1;     // the fps number in the on-screen strip
volatile int rex_nx_sound = 1;       // the audio output (audout driver, src/nx_audout_driver.h) plays; 0 = silence
volatile int rex_nx_overlay = 1;     // the on-screen strip (version + fps) is drawn; 0 = hidden
volatile int rex_nx_hatching = 1;     // the pencil hatching on the sprites (deko3d backend); 0 = off
// The data mods (sdmc:/switch/gh/mods/<id>/, src/sprite_mod.h). The deko3d backend finds them at the start and
// registers them here, the highest priority first; "mod.<id>=0|1" in settings.txt holds each switch (a new mod is on).
volatile int rex_nx_mod_epoch = 0;  // rises at every change of a mod switch: the backend looks its replacements up again
struct NxMod {
  char id[32];
  char name[64];
  char description[200];
  char info[64];  // "v<version> by <author>"
  int on;
};
// ponytail: fixed tables (32 ids known from settings.txt, 16 mods in the menu); a mod beyond that is not loaded.
static NxMod g_mods[32];
static int g_mod_total = 0;
static int g_mod_reg[16];
static int g_mod_count = 0;
static NxMod* NxFindMod(const char* id, bool add) {
  for (int i = 0; i < g_mod_total; i++)
    if (std::strcmp(g_mods[i].id, id) == 0) return &g_mods[i];
  if (!add || g_mod_total >= 32) return nullptr;
  NxMod& m = g_mods[g_mod_total++];
  std::snprintf(m.id, sizeof(m.id), "%s", id);
  m.name[0] = m.description[0] = m.info[0] = 0;
  m.on = 1;
  return &m;
}
void rex_nx_load_settings(void);
int rex_nx_mod_count(void) { return g_mod_count; }
const char* rex_nx_mod_name(int i) { return i >= 0 && i < g_mod_count ? g_mods[g_mod_reg[i]].name : ""; }
const char* rex_nx_mod_description(int i) { return i >= 0 && i < g_mod_count ? g_mods[g_mod_reg[i]].description : ""; }
const char* rex_nx_mod_info(int i) { return i >= 0 && i < g_mod_count ? g_mods[g_mod_reg[i]].info : ""; }
int rex_nx_mod_on(int i) { return i >= 0 && i < g_mod_count ? g_mods[g_mod_reg[i]].on : 0; }
// Returns the mod's number (its place in the menu and its priority), or -1 when the table is full.
int rex_nx_register_mod(const char* id, const char* name, const char* description, const char* info) {
  rex_nx_load_settings();
  NxMod* m = g_mod_count < 16 ? NxFindMod(id, true) : nullptr;
  if (!m) return -1;
  std::snprintf(m->name, sizeof(m->name), "%s", name);
  std::snprintf(m->description, sizeof(m->description), "%s", description);
  std::snprintf(m->info, sizeof(m->info), "%s", info);
  g_mod_reg[g_mod_count] = int(m - g_mods);
  return g_mod_count++;
}
// The 4-player story mod (src/story4p.cpp) is the built-in mod "4players", the first line of the mods page.
// This is its switch as read at the start: the hooks take it once, a change in the menu acts after a restart.
volatile int rex_nx_four_players = 1;
// One Joy-Con held sideways: 1 = this driver turns its stick and buttons (the system reports them upright),
// 0 = the system does it (hold type "horizontal"). "single_joycon=0|1" in settings.txt, read at the start.
volatile int rex_nx_single_joycon = 1;
// The command processor's sleep between two polls of a value the guest writes (WAIT_REG_MEM, sdk-edits.py), in
// microseconds. 0.114 had 100 by default (key gpu_wait_us, no longer read): its perf lines show one wait a frame of
// 12-16 ms (memory ..1006), so a finer poll gains at most a millisecond. "gpu_poll_us=" in settings.txt (50..1000).
volatile int rex_nx_gpu_wait_us = 1000;
// Frame skip (the menu, "fps_cap=60|30"): 1 = the renderer draws every second guest frame (deko3d_backend.cpp).
volatile int rex_nx_frame_skip = 0;
// The control scheme (the menu, "controls=std|alt"): 1 = alt_controls.h, for full pads. Read at every poll.
volatile int rex_nx_controls_alt = 0;
// The cheats (src/cheats.cpp reads them live).
volatile int rex_nx_cheat_invincible = 0;
volatile int rex_nx_cheat_mp = 0;
volatile int rex_nx_cheat_continues = 0;
volatile int rex_nx_cheat_attributes = 0;
// 0 = the normal story flow; 1..40 = a new story game starts at stage name list entry n-1 (src/cheats.cpp).
volatile int rex_nx_start_level = 0;
extern volatile int rex_nx_auto_clock, rex_nx_clock_max_cpu, rex_nx_clock_max_gpu, rex_nx_clock_max_mem;  // switch_nx.cpp
volatile int rex_nx_apply_level = 0;  // Apply pressed: 1..40, the game thread takes it (src/cheats.cpp) and sets it back to 0
// The game's own stage names (string table at guest 0x82914668, indexed by the stage byte at game state +0x3AF).
const char* rex_nx_level_name(int n) {
  static const char* const names[] = {"1-1", "1-2", "1-3", "2-1", "3-1", "3-vs", "3-2", "4-1", "4-2", "4-3", "5-1", "5-vs",
                                      "5-2", "6-1", "6-2", "7-1", "7-2", "8-1", "8-2", "9-1", "9-2", "9-3", "10-1", "10-2",
                                      "10-3", "11-1", "11-2", "12-1", "12-vs", "12-2", "13-1", "14-1", "15-1", "15-2",
                                      "15-3", "16-1", "16-2", "17-1", "18-1", "18-2"};
  return n >= 1 && n <= 40 ? names[n - 1] : "off";
}

// The settings live in sdmc:/switch/gh/settings.txt ("fps=1", "sound=1", "overlay=1", and the cheat keys
// "invincible=", "mp=", "continues=", "attributes=", one per line). A hand-made cheats.txt in the same folder is
// read first (settings.txt wins). The
// file is read once, by whoever asks first (the pad driver or the deko3d backend), and written on every change
// in the menu. The marker files overlay_off and mute (src/nx_audout_driver.h) still force a start with that
// setting off.
static const char* const kSettingsPath = "sdmc:/switch/gh/settings.txt";
static bool g_settings_loaded = false;

void rex_nx_load_settings(void) {
  if (g_settings_loaded) return;
  g_settings_loaded = true;
  GhCheatFlags cheats;
  int four_legacy = -1;
  if (FILE* fp = std::fopen("sdmc:/switch/gh/cheats.txt", "r")) {
    char line[64];
    while (std::fgets(line, sizeof(line), fp)) GhCheatParseLine(line, cheats);
    std::fclose(fp);
  }
  if (FILE* fp = std::fopen(kSettingsPath, "r")) {
    char line[64];
    while (std::fgets(line, sizeof(line), fp)) {
      GhCheatParseLine(line, cheats);
      int v = 0;
      char mod_id[32];
      if (std::sscanf(line, "mod.%31[^=\r\n]=%d", mod_id, &v) == 2) {
        if (NxMod* m = NxFindMod(mod_id, true)) m->on = v != 0;
      }
      else if (std::sscanf(line, "fps=%d", &v) == 1) rex_nx_show_fps = v != 0;
      else if (std::sscanf(line, "sound=%d", &v) == 1) rex_nx_sound = v != 0;
      else if (std::sscanf(line, "overlay=%d", &v) == 1) rex_nx_overlay = v != 0;
      else if (std::sscanf(line, "hatching=%d", &v) == 1) rex_nx_hatching = v != 0;
      else if (std::sscanf(line, "single_joycon=%d", &v) == 1) rex_nx_single_joycon = v != 0;
      else if (std::sscanf(line, "gpu_poll_us=%d", &v) == 1) rex_nx_gpu_wait_us = v < 50 ? 50 : v > 1000 ? 1000 : v;
      else if (std::sscanf(line, "fps_cap=%d", &v) == 1) rex_nx_frame_skip = v == 30;
      else if (std::strncmp(line, "controls=", 9) == 0) rex_nx_controls_alt = std::strncmp(line + 9, "alt", 3) == 0;
      else if (std::sscanf(line, "four_players=%d", &v) == 1) four_legacy = v != 0;  // the key before the mods page
      else if (std::sscanf(line, "start_level=%d", &v) == 1) rex_nx_start_level = v >= 1 && v <= 40 ? v : 0;
    }
    std::fclose(fp);
  }
  // The built-in mod: mod 0, before the data mods the backend registers. "mod.4players" wins over the old key.
  NxMod* four = NxFindMod("4players", false);
  if (!four && (four = NxFindMod("4players", true))) four->on = four_legacy >= 0 ? four_legacy : 1;
  if (four) {
    std::snprintf(four->name, sizeof(four->name), "4 players (RedDude)");
    std::snprintf(four->description, sizeof(four->description),
                  "4-player story mode. players 3 and 4 join with +. use 3 pads.");  // two menu lines of 30
    std::snprintf(four->info, sizeof(four->info), "by RedDude. restart needed");
    g_mod_reg[g_mod_count++] = int(four - g_mods);
    rex_nx_four_players = four->on;
  }
  rex_nx_cheat_invincible = cheats.invincible;
  rex_nx_cheat_mp = cheats.mp;
  rex_nx_cheat_continues = cheats.continues;
  rex_nx_cheat_attributes = cheats.attributes;
  if (access("sdmc:/switch/gh/overlay_off", F_OK) == 0) rex_nx_overlay = 0;
  std::printf("[nx] settings: fps %d, sound %d, overlay %d, cheats %d%d%d%d\n", rex_nx_show_fps, rex_nx_sound,
              rex_nx_overlay, rex_nx_cheat_invincible, rex_nx_cheat_mp, rex_nx_cheat_continues,
              rex_nx_cheat_attributes);
  std::fflush(stdout);
}

void rex_nx_save_settings(void) {
  FILE* fp = std::fopen(kSettingsPath, "w");
  if (!fp) return;
  std::fprintf(fp, "fps=%d\nsound=%d\noverlay=%d\nstart_level=%d\n", rex_nx_show_fps, rex_nx_sound, rex_nx_overlay,
               rex_nx_start_level);
  std::fprintf(fp, "hatching=%d\nsingle_joycon=%d\ngpu_poll_us=%d\nfps_cap=%d\ncontrols=%s\n", rex_nx_hatching, rex_nx_single_joycon,
               rex_nx_gpu_wait_us, rex_nx_frame_skip ? 30 : 60, rex_nx_controls_alt ? "alt" : "std");
  for (int i = 0; i < g_mod_total; i++) std::fprintf(fp, "mod.%s=%d\n", g_mods[i].id, g_mods[i].on);
  // The clock keys (switch_nx.cpp reads them at start): kept, so a menu save does not drop them.
  std::fprintf(fp, "auto_clock=%d\nclock_max_cpu=%d\nclock_max_gpu=%d\nclock_max_mem=%d\n", rex_nx_auto_clock,
               rex_nx_clock_max_cpu, rex_nx_clock_max_gpu, rex_nx_clock_max_mem);
  GhCheatFlags cheats;
  cheats.invincible = rex_nx_cheat_invincible != 0;
  cheats.mp = rex_nx_cheat_mp != 0;
  cheats.continues = rex_nx_cheat_continues != 0;
  cheats.attributes = rex_nx_cheat_attributes != 0;
  char text[96];
  GhCheatFormat(cheats, text, sizeof(text));
  std::fputs(text, fp);
  std::fclose(fp);
}
}

namespace rex::input::nx {

namespace {

constexpr int kMainItems = 11, kCheatItems = 5;  // main page: resume, fps, sound, overlay, hatching, frame skip, controls, start level, go to level, cheats, mods; cheats page: 4 cheats, back
int PageItems() {
  return rex_nx_menu_page == 1 ? kCheatItems : rex_nx_menu_page == 2 ? g_mod_count + 1 : kMainItems;  // mods page: the mods, back
}
constexpr uint64_t kRepeatDelayNs = 400000000ull, kRepeatNs = 120000000ull;  // D-pad left/right repeat
bool g_combo_prev = false;
int g_menu_open_logged = 0;
bool g_neutral = false;  // neutral pad for the game until every button is released
int g_rep_dir = 0;
uint64_t g_rep_next = 0;
bool g_menu_dirty = false;  // a value changed and is not saved yet
uint16_t g_prev_all = 0;

// One menu action on the current item. dir = -1 (left, previous) or +1 (right, next); confirm = the A button.
// Switches flip on left, right and confirm; start level steps by dir and wraps 0..40 (0 = off); resume, cheats and
// back act on confirm only. Returns true when a value changed (the caller saves).
bool MenuChange(int dir, bool confirm) {
  volatile int& c = rex_nx_menu_cursor;
  auto flip = [](volatile int& v) { v = !v; return true; };
  if (rex_nx_menu_page == 0) {
    if (c == 0) { if (confirm) rex_nx_menu_open = 0; return false; }
    if (c == 1) return flip(rex_nx_show_fps);
    if (c == 2) return flip(rex_nx_sound);
    if (c == 3) return flip(rex_nx_overlay);
    if (c == 4) return flip(rex_nx_hatching);
    if (c == 5) return flip(rex_nx_frame_skip);
    if (c == 6) return flip(rex_nx_controls_alt);
    if (c == 7) {
      rex_nx_start_level = (rex_nx_start_level + dir + 41) % 41;
      return true;
    }
    if (c == 8) {  // go to level: the game thread writes the stage byte (src/cheats.cpp)
      if (confirm && rex_nx_start_level) { rex_nx_apply_level = rex_nx_start_level; rex_nx_menu_open = 0; }
      return false;
    }
    if (confirm) { rex_nx_menu_page = c == 9 ? 1 : 2; c = 0; }  // cheats, mods
    return false;
  }
  if (rex_nx_menu_page == 1) {
    if (c == 0) return flip(rex_nx_cheat_invincible);
    if (c == 1) return flip(rex_nx_cheat_mp);
    if (c == 2) return flip(rex_nx_cheat_continues);
    if (c == 3) return flip(rex_nx_cheat_attributes);
    if (confirm) { rex_nx_menu_page = 0; c = 9; }  // back
    return false;
  }
  // The mods page: one switch per mod. The switch first, then the epoch (the renderer looks up at a new epoch).
  if (c < g_mod_count) {
    NxMod& m = g_mods[g_mod_reg[c]];
    m.on = !m.on;
    rex_nx_mod_epoch = rex_nx_mod_epoch + 1;
    return true;
  }
  if (confirm) { rex_nx_menu_page = 0; c = 10; }  // back
  return false;
}

// Five pad sources: 0 = the console itself (handheld, Joy-Cons on the rails), 1..4 = controllers No1..No4. Each
// connected source is one device (kPad + source); the input system gives guest users 0..3 to the first four that
// connect. A controller is a Pro Controller, a Joy-Con pair or one Joy-Con held sideways.
constexpr DeviceId kPad = static_cast<DeviceId>(0x4E580000);  // "NX"
constexpr int kSources = 5;
constexpr HidNpadIdType kNpad[kSources] = {HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2,
                                           HidNpadIdType_No3, HidNpadIdType_No4};

int Source(DeviceId id) {
  const uint64_t i = static_cast<uint64_t>(id) - static_cast<uint64_t>(kPad);
  return i < kSources ? int(i) : -1;
}
bool Connected(int source) { return hidGetNpadStyleSet(kNpad[source]) != 0; }
// The pad that owns the menu and pad.txt: the first connected source (the handheld when nothing is connected).
int Primary() {
  for (int i = 0; i < kSources; i++)
    if (Connected(i)) return i;
  return 0;
}

// Switch A (east) = Xbox A (accept), Switch B (south) = Xbox B (cancel). Y/X stay positional (west = Xbox X, north = Xbox Y).
struct ButtonMap {
  uint64_t nx;
  uint16_t x360;
};
constexpr ButtonMap kButtons[] = {
    {HidNpadButton_A, X_INPUT_GAMEPAD_A},
    {HidNpadButton_B, X_INPUT_GAMEPAD_B},
    {HidNpadButton_Y, X_INPUT_GAMEPAD_X},
    {HidNpadButton_X, X_INPUT_GAMEPAD_Y},
    {HidNpadButton_Plus, X_INPUT_GAMEPAD_START},
    {HidNpadButton_Minus, X_INPUT_GAMEPAD_BACK},
    {HidNpadButton_L, X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {HidNpadButton_R, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {HidNpadButton_StickL, X_INPUT_GAMEPAD_LEFT_THUMB},
    {HidNpadButton_StickR, X_INPUT_GAMEPAD_RIGHT_THUMB},
    {HidNpadButton_Up, X_INPUT_GAMEPAD_DPAD_UP},
    {HidNpadButton_Down, X_INPUT_GAMEPAD_DPAD_DOWN},
    {HidNpadButton_Left, X_INPUT_GAMEPAD_DPAD_LEFT},
    {HidNpadButton_Right, X_INPUT_GAMEPAD_DPAD_RIGHT},
};

// 1 = a left Joy-Con alone, 2 = a right Joy-Con alone (held sideways), 0 = a full pad.
// A Joy-Con alone reports the style JoyLeft / JoyRight, or the style JoyDual with one half connected.
int Single(uint32_t style, uint32_t attributes) {
  if (style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld)) return 0;
  if (style & HidNpadStyleTag_NpadJoyDual) {
    const bool l = attributes & HidNpadAttribute_IsLeftConnected, r = attributes & HidNpadAttribute_IsRightConnected;
    return l == r ? 0 : l ? 1 : 2;
  }
  return (style & HidNpadStyleTag_NpadJoyLeft) ? 1 : (style & HidNpadStyleTag_NpadJoyRight) ? 2 : 0;
}

// One Joy-Con held sideways (the rail up) as a whole pad, in the bits of a full pad:
//   stick = the left stick (and the D-pad, for the menu); the four buttons by their place, as on a full pad
//   (right = A, bottom = B, left = Y, top = X); SL / SR = L / R; the two end buttons = ZL, ZR;
//   - (left Joy-Con) or + (right Joy-Con) = +; the stick click = -.
// `turn`: the system reports the Joy-Con upright, so the stick and the buttons are turned here.
uint64_t SingleToFull(int single, bool turn, uint64_t raw, HidAnalogStickState& stick) {
  struct M { uint64_t from, to; };
  static constexpr M kLeft[] = {{HidNpadButton_Left, HidNpadButton_B}, {HidNpadButton_Down, HidNpadButton_A},
                                {HidNpadButton_Up, HidNpadButton_Y},   {HidNpadButton_Right, HidNpadButton_X}};
  static constexpr M kRight[] = {{HidNpadButton_A, HidNpadButton_B}, {HidNpadButton_X, HidNpadButton_A},
                                 {HidNpadButton_B, HidNpadButton_Y}, {HidNpadButton_Y, HidNpadButton_X}};
  uint64_t out = 0;
  if (turn) {
    for (const M& m : single == 1 ? kLeft : kRight)
      if (raw & m.from) out |= m.to;
    const int32_t x = stick.x, y = stick.y;
    stick.x = single == 1 ? -y : y;
    stick.y = single == 1 ? x : -x;
  } else {
    out = raw & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y);
  }
  if (raw & (HidNpadButton_Plus | HidNpadButton_Minus)) out |= HidNpadButton_Plus;
  if (raw & (HidNpadButton_StickL | HidNpadButton_StickR)) out |= HidNpadButton_Minus;
  if (raw & HidNpadButton_AnySL) out |= HidNpadButton_L;
  if (raw & HidNpadButton_AnySR) out |= HidNpadButton_R;
  if (raw & (HidNpadButton_L | HidNpadButton_R)) out |= HidNpadButton_ZL;
  if (raw & (HidNpadButton_ZL | HidNpadButton_ZR)) out |= HidNpadButton_ZR;
  constexpr int32_t kDir = 16000;  // of 32767: the stick as a D-pad
  if (stick.x > kDir) out |= HidNpadButton_Right;
  if (stick.x < -kDir) out |= HidNpadButton_Left;
  if (stick.y > kDir) out |= HidNpadButton_Up;
  if (stick.y < -kDir) out |= HidNpadButton_Down;
  return out;
}

// "[nx] pads:" in gh_log.txt, at the start and at every change: what the console reports for each source.
void LogPads() {
  static uint32_t last[kSources] = {~0u};
  uint32_t now[kSources];
  bool same = true;
  for (int i = 0; i < kSources; i++) {
    now[i] = hidGetNpadStyleSet(kNpad[i]) & 0xFFFF;
    if (now[i] & HidNpadStyleTag_NpadJoyDual) {  // which halves: the attributes, in the high half
      HidNpadJoyDualState st{};
      if (hidGetNpadStatesJoyDual(kNpad[i], &st, 1)) now[i] |= (st.attributes & 0xFFFF) << 16;
    }
    same = same && now[i] == last[i];
  }
  if (same) return;
  int player = 0;
  for (int i = 0; i < kSources; i++) {
    last[i] = now[i];
    const uint32_t st = now[i] & 0xFFFF, attributes = now[i] >> 16;
    const int single = Single(st, attributes);
    const char* kind = !st ? "not connected"
                       : single == 1 ? "left Joy-Con, sideways"
                       : single == 2 ? "right Joy-Con, sideways"
                       : (st & HidNpadStyleTag_NpadHandheld) ? "handheld"
                       : (st & HidNpadStyleTag_NpadFullKey) ? "Pro Controller"
                       : (st & HidNpadStyleTag_NpadJoyDual) ? "Joy-Con pair" : "other";
    if (st) player++;
    std::printf("[nx] pads: source %d (%s): style 0x%X attributes 0x%X, %s%s\n", i, i ? "controller" : "console", st,
                attributes, kind, st ? (player == 1 ? ", first in order (owns the menu)" : "") : "");
  }
  std::printf("[nx] pads: %d connected; the game gives its pads 1-4 in the order of connection\n", player);
  std::fflush(stdout);
}

int16_t ClampStick(int32_t v) {
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return static_cast<int16_t>(v);
}

}  // namespace

NXInputDriver::NXInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

NXInputDriver::~NXInputDriver() = default;

X_STATUS NXInputDriver::Setup() {
  rex_nx_load_settings();
  padConfigureInput(4, HidNpadStyleSet_NpadStandard);  // Pro Controller, handheld, Joy-Con pair, one Joy-Con
  hidSetNpadJoyHoldType(rex_nx_single_joycon ? HidNpadJoyHoldType_Vertical : HidNpadJoyHoldType_Horizontal);
  for (int i = 0; i < kSources; i++) padInitialize(&pads_[i], kNpad[i]);
  REXLOG_INFO("[nx] input: handheld, then controllers 1-4 (Pro Controller / Joy-Con pair / one Joy-Con sideways); "
              "the first four are gamepads 0-3");
  return X_STATUS_SUCCESS;
}

void NXInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  static const char* const names[kSources] = {"Switch handheld", "Switch pad 1", "Switch pad 2", "Switch pad 3",
                                              "Switch pad 4"};
  LogPads();
  const int primary = Primary();
  for (int i = 0; i < kSources; i++) {
    if (i != primary && !Connected(i)) continue;
    DeviceInfo info;
    info.id = static_cast<DeviceId>(static_cast<uint64_t>(kPad) + i);
    info.name = names[i];
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT NXInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (Source(id) < 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

// Scheduled presses from sdmc:/switch/gh/pad.txt, one per line: "<seconds>:<BUTTON>[+BUTTON][:<hold ms>]"
// (START BACK A B X Y LB RB LT RT UP DOWN LEFT RIGHT LS RS; the hold defaults to 200 ms), seconds
// from the driver's start. Test automation in Eden and on the console: the keyboard in Eden needs
// focus, this does not. A "#" starts a comment.
void NXInputDriver::LoadSchedule() {
  schedule_loaded_ = true;
  t0_tick_ = armGetSystemTick();
  FILE* fp = std::fopen("sdmc:/switch/gh/pad.txt", "r");
  if (!fp) return;
  struct Name {
    const char* name;
    uint16_t bit;
  };
  static const Name names[] = {{"START", X_INPUT_GAMEPAD_START}, {"BACK", X_INPUT_GAMEPAD_BACK},
                               {"A", X_INPUT_GAMEPAD_A},         {"B", X_INPUT_GAMEPAD_B},
                               {"X", X_INPUT_GAMEPAD_X},         {"Y", X_INPUT_GAMEPAD_Y},
                               {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
                               {"UP", X_INPUT_GAMEPAD_DPAD_UP},  {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
                               {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
                               {"LS", X_INPUT_GAMEPAD_LEFT_THUMB}, {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB}};
  char line[128];
  while (std::fgets(line, sizeof(line), fp)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char* p = line;
    double seconds = std::strtod(p, &p);
    if (*p != ':') continue;
    p++;
    Scheduled s{};
    s.start_tick = t0_tick_ + armNsToTicks(uint64_t(seconds * 1e9));
    uint32_t hold_ms = 200;
    char* buttons = p;
    char* colon = std::strchr(p, ':');
    if (colon) {
      *colon = 0;
      hold_ms = uint32_t(std::strtoul(colon + 1, nullptr, 10));
    }
    for (char* tok = std::strtok(buttons, "+ \r\n"); tok; tok = std::strtok(nullptr, "+ \r\n")) {
      for (const Name& n : names) {
        if (std::strcmp(tok, n.name) == 0) s.buttons |= n.bit;
      }
      if (std::strcmp(tok, "LT") == 0) s.left_trigger = true;
      if (std::strcmp(tok, "RT") == 0) s.right_trigger = true;
    }
    s.end_tick = s.start_tick + armNsToTicks(uint64_t(hold_ms) * 1000000ull);
    schedule_.push_back(s);
  }
  std::fclose(fp);
  std::printf("[nx] pad: %zu scheduled presses from pad.txt\n", schedule_.size());
  std::fflush(stdout);
}

X_RESULT NXInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  const int source = Source(id);
  if (source < 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const bool primary = source == Primary();  // the menu and pad.txt belong to this pad only
  if (!primary && !Connected(source)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  PadState& pad_ = pads_[source];
  if (!schedule_loaded_) LoadSchedule();
  padUpdate(&pad_);
  uint64_t held = padGetButtons(&pad_);
  HidAnalogStickState left = padGetStickPos(&pad_, 0);
  HidAnalogStickState right = padGetStickPos(&pad_, 1);
  const int single = Single(padGetStyleSet(&pad_), padGetAttributes(&pad_));
  if (single) {
    static uint64_t raw_prev[kSources];
    static int raw_logs = 0;
    if (held != raw_prev[source] && held && raw_logs < 24) {  // the first presses: what the console reports
      raw_logs++;
      std::printf("[nx] pads: source %d (%s Joy-Con) raw buttons 0x%llX stick %d,%d / %d,%d\n", source,
                  single == 1 ? "left" : "right", (unsigned long long)held, left.x, left.y, right.x, right.y);
      std::fflush(stdout);
    }
    raw_prev[source] = held;
    left.x += right.x;  // one stick: the other reads 0
    left.y += right.y;
    right = HidAnalogStickState{};
    const bool dual_half = padGetStyleSet(&pad_) & HidNpadStyleTag_NpadJoyDual;  // always reported upright
    held = SingleToFull(single, dual_half || rex_nx_single_joycon != 0, held, left);
  }
  uint16_t scheduled = 0;
  bool scheduled_lt = false, scheduled_rt = false;
  if (primary && !schedule_.empty()) {
    const uint64_t now = armGetSystemTick();
    for (Scheduled& s : schedule_) {
      if (now >= s.start_tick && now < s.end_tick) {
        scheduled |= s.buttons;
        scheduled_lt |= s.left_trigger;
        scheduled_rt |= s.right_trigger;
        if (!s.logged) {
          s.logged = true;
          std::printf("[nx] pad: scheduled press buttons 0x%04X at %llu s\n", s.buttons,
                      (unsigned long long)(armTicksToNs(now - t0_tick_) / 1000000000ull));
          std::fflush(stdout);
        }
      }
    }
    if (scheduled || scheduled_lt || scheduled_rt) held ^= 0x8000000000000000ull;  // a change: a new packet
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    uint16_t buttons = 0;
    for (const ButtonMap& m : kButtons) {
      if (held & m.nx) buttons |= m.x360;
    }
    // gh: the alternative scheme, for a full pad (one Joy-Con has no right stick). The menu keys are not in it.
    bool lt = held & HidNpadButton_ZL;
    int32_t ry = right.y;
    if (rex_nx_controls_alt && !single) {
      static GhAltLatch latch[kSources];
      GhAltControls(buttons, lt, ry, latch[source]);
      held ^= (latch[source].up ? 1ull << 61 : 0) ^ (latch[source].down ? 1ull << 62 : 0);  // a change: a new packet
    }
    uint16_t all = buttons | scheduled;
    bool neutral_now = g_neutral || rex_nx_menu_open;  // the other pads are neutral under the menu too
    if (primary) {
      // gh: + and - together (Start and Back) open and close the settings menu; each button alone goes to the game.
      // L3 and R3 are plain game buttons (the game uses them). While the menu is up, and until all buttons are
      // released after it, the game gets a neutral pad.
      const uint64_t now_tick = armGetSystemTick();
      const bool combo = (all & X_INPUT_GAMEPAD_START) && (all & X_INPUT_GAMEPAD_BACK);
      if (combo && !g_combo_prev) {
        rex_nx_menu_open = !rex_nx_menu_open;
        rex_nx_menu_cursor = 0;
        rex_nx_menu_page = 0;
        g_neutral = true;
      }
      g_combo_prev = combo;
      if (rex_nx_menu_open) {
        const uint16_t pressed = all & ~g_prev_all;
        const int max_cursor = PageItems() - 1;
        if (pressed & X_INPUT_GAMEPAD_DPAD_UP) rex_nx_menu_cursor = (rex_nx_menu_cursor + max_cursor) % (max_cursor + 1);
        if (pressed & X_INPUT_GAMEPAD_DPAD_DOWN) rex_nx_menu_cursor = (rex_nx_menu_cursor + 1) % (max_cursor + 1);
        // D-pad left / right change the value (left = previous, right = next, wrap around); held, they repeat.
        const int dir = ((all & X_INPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) - ((all & X_INPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0);
        bool changed = false;
        if (dir != g_rep_dir) {
          g_rep_dir = dir;
          g_rep_next = now_tick + armNsToTicks(kRepeatDelayNs);
          if (dir) changed = MenuChange(dir, false);
        } else if (dir && now_tick >= g_rep_next) {
          g_rep_next = now_tick + armNsToTicks(kRepeatNs);
          changed = MenuChange(dir, false);
        }
        if (pressed & X_INPUT_GAMEPAD_A) changed |= MenuChange(1, true);  // the Switch A button confirms
        if (changed) {
          g_menu_dirty = true;
          std::printf("[nx] menu: page %d item %d: fps %d, sound %d, overlay %d, cheats %d%d%d%d, start level %s\n",
                      rex_nx_menu_page, rex_nx_menu_cursor, rex_nx_show_fps, rex_nx_sound, rex_nx_overlay,
                      rex_nx_cheat_invincible, rex_nx_cheat_mp, rex_nx_cheat_continues, rex_nx_cheat_attributes,
                      rex_nx_level_name(rex_nx_start_level));
          std::fflush(stdout);
        }
        if (pressed & X_INPUT_GAMEPAD_B) {  // the Switch B button goes back
          if (rex_nx_menu_page) {
            rex_nx_menu_cursor = rex_nx_menu_page == 1 ? 9 : 10;  // the cheats and mods lines
            rex_nx_menu_page = 0;
          } else {
            rex_nx_menu_open = 0;
          }
        }
        g_neutral = true;
      }
      if (!rex_nx_menu_open) g_rep_dir = 0;
      // Write settings.txt once the D-pad is released (not on every repeat step) or when the menu closes.
      if (g_menu_dirty && (!rex_nx_menu_open || g_rep_dir == 0)) {
        g_menu_dirty = false;
        rex_nx_save_settings();
      }
      if (rex_nx_menu_open != g_menu_open_logged) {
        g_menu_open_logged = rex_nx_menu_open;
        std::printf("[nx] menu: %s\n", rex_nx_menu_open ? "open" : "closed");
        std::fflush(stdout);
      }
      g_prev_all = all;
      if (g_neutral && !all && !rex_nx_menu_open) g_neutral = false;
      neutral_now = g_neutral || rex_nx_menu_open;
    }
    if (neutral_now) {
      all = 0;
      scheduled_lt = scheduled_rt = false;
      lt = false;
      held &= ~(uint64_t(HidNpadButton_ZL) | uint64_t(HidNpadButton_ZR));
    }
    out_state->gamepad.buttons = all;
    out_state->gamepad.left_trigger = lt || scheduled_lt ? 0xFF : 0;
    out_state->gamepad.right_trigger = (held & HidNpadButton_ZR) || scheduled_rt ? 0xFF : 0;
    out_state->gamepad.thumb_lx = neutral_now ? 0 : ClampStick(left.x);
    out_state->gamepad.thumb_ly = neutral_now ? 0 : ClampStick(left.y);
    out_state->gamepad.thumb_rx = neutral_now ? 0 : ClampStick(right.x);
    out_state->gamepad.thumb_ry = neutral_now ? 0 : ClampStick(ry);
    if (held != last_buttons_[source]) {
      packet_number_[source]++;
      last_buttons_[source] = held;
    }
    out_state->packet_number = packet_number_[source];
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NXInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  return Source(id) >= 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT NXInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  return Source(id) >= 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace rex::input::nx
