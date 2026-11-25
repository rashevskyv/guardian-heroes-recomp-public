#include <rex/input/nx/nx_input_driver.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <rex/input/flags.h>
#include <rex/logging.h>

#include "../../../src/cheats_config.h"  // gh: GhCheatFlags, GhCheatParseLine, GhCheatFormat

// gh: the settings menu drawn by the deko3d backend (src/gpu/deko3d_backend.cpp) reads these.
extern "C" {
volatile int rex_nx_menu_open = 0;    // 1 while the menu is up: the game sees a neutral pad
volatile int rex_nx_menu_cursor = 0;  // main page: 0 resume, 1 fps, 2 sound, 3 overlay, 4 hatching, 5 sprite mods, 6 cheats; cheats page: 0-3 cheats, 4 start level, 5 apply, 6 back
volatile int rex_nx_menu_page = 0;    // 0 main, 1 cheats
volatile int rex_nx_show_fps = 1;     // the fps number in the on-screen strip
volatile int rex_nx_sound = 1;       // the audio output (audout driver, src/nx_audout_driver.h) plays; 0 = silence
volatile int rex_nx_overlay = 1;     // the on-screen strip (version + fps) is drawn; 0 = hidden
volatile int rex_nx_hatching = 1;     // the pencil hatching on the sprites (deko3d backend); 0 = off
volatile int rex_nx_sprite_mods = 1;  // the PNG replacements of mods/sprites/ are drawn (deko3d backend); 0 = the game's sprites
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
      if (std::sscanf(line, "fps=%d", &v) == 1) rex_nx_show_fps = v != 0;
      else if (std::sscanf(line, "sound=%d", &v) == 1) rex_nx_sound = v != 0;
      else if (std::sscanf(line, "overlay=%d", &v) == 1) rex_nx_overlay = v != 0;
      else if (std::sscanf(line, "hatching=%d", &v) == 1) rex_nx_hatching = v != 0;
      else if (std::sscanf(line, "sprite_mods=%d", &v) == 1) rex_nx_sprite_mods = v != 0;
      else if (std::sscanf(line, "start_level=%d", &v) == 1) rex_nx_start_level = v >= 1 && v <= 40 ? v : 0;
    }
    std::fclose(fp);
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
  std::fprintf(fp, "hatching=%d\nsprite_mods=%d\n", rex_nx_hatching, rex_nx_sprite_mods);
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

constexpr int kMainItems = 7, kCheatItems = 7;  // main page: resume, fps, sound, overlay, hatching, sprite mods, cheats; cheats page: 4 cheats, start level, apply, back
constexpr uint64_t kRepeatDelayNs = 400000000ull, kRepeatNs = 120000000ull;  // D-pad left/right repeat
constexpr uint64_t kStickHoldNs = 150000000ull;   // L3 + R3 must be held together this long (any order)
constexpr uint64_t kPartialLogNs = 400000000ull;  // one thumb alone this long is logged once
bool g_combo_prev = false;
uint64_t g_sticks_since = 0, g_one_stick_since = 0;
bool g_partial_logged = false;
int g_menu_open_logged = 0;
bool g_menu_via_sticks = false;
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
    if (c == 5) return flip(rex_nx_sprite_mods);
    if (confirm) { rex_nx_menu_page = 1; c = 0; }  // cheats
    return false;
  }
  if (c == 0) return flip(rex_nx_cheat_invincible);
  if (c == 1) return flip(rex_nx_cheat_mp);
  if (c == 2) return flip(rex_nx_cheat_continues);
  if (c == 3) return flip(rex_nx_cheat_attributes);
  if (c == 4) {
    rex_nx_start_level = (rex_nx_start_level + dir + 41) % 41;
    return true;
  }
  if (c == 5) {  // apply: the game thread writes the stage byte (src/cheats.cpp)
    if (confirm && rex_nx_start_level) { rex_nx_apply_level = rex_nx_start_level; rex_nx_menu_open = 0; }
    return false;
  }
  if (confirm) { rex_nx_menu_page = 0; c = 6; }  // back
  return false;
}

constexpr DeviceId kPad = static_cast<DeviceId>(0x4E580000);  // "NX"

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
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad_);
  REXLOG_INFO("[nx] input: libnx pad (handheld / Joy-Con pair / Pro Controller) as gamepad 0");
  rex_nx_load_settings();
  return X_STATUS_SUCCESS;
}

void NXInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kPad;
  info.name = "Switch pad";
  info.synthetic = false;
  out.push_back(info);
}

X_RESULT NXInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (id != kPad) {
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
  if (id != kPad) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!schedule_loaded_) LoadSchedule();
  padUpdate(&pad_);
  uint64_t held = padGetButtons(&pad_);
  uint16_t scheduled = 0;
  bool scheduled_lt = false, scheduled_rt = false;
  if (!schedule_.empty()) {
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
  const HidAnalogStickState left = padGetStickPos(&pad_, 0);
  const HidAnalogStickState right = padGetStickPos(&pad_, 1);
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    uint16_t buttons = 0;
    for (const ButtonMap& m : kButtons) {
      if (held & m.nx) buttons |= m.x360;
    }
    uint16_t all = buttons | scheduled;
    bool neutral_now = false;
    {
      // gh: L3 + R3 held together for 150 ms (any order), or + and - together (Start and Back), open and close the
      // settings menu; each button alone goes to the game. While the menu is up, and until all buttons are released
      // after it, the game gets a neutral pad.
      const uint64_t now_tick = armGetSystemTick();
      const bool l3 = all & X_INPUT_GAMEPAD_LEFT_THUMB, r3 = all & X_INPUT_GAMEPAD_RIGHT_THUMB;
      if (l3 && r3) {
        if (!g_sticks_since) g_sticks_since = now_tick;
      } else {
        g_sticks_since = 0;
      }
      if (l3 != r3) {
        if (!g_one_stick_since) g_one_stick_since = now_tick;
        if (!g_partial_logged && armTicksToNs(now_tick - g_one_stick_since) >= kPartialLogNs) {
          g_partial_logged = true;
          std::printf("[nx] menu: only %s is held; the menu needs L3 and R3 together (150 ms) or + and - together\n",
                      l3 ? "L3" : "R3");
          std::fflush(stdout);
        }
      } else {
        g_one_stick_since = 0;
      }
      const bool start_back = (all & X_INPUT_GAMEPAD_START) && (all & X_INPUT_GAMEPAD_BACK);
      const bool sticks = g_sticks_since && armTicksToNs(now_tick - g_sticks_since) >= kStickHoldNs;
      const bool combo = start_back || sticks;
      if (combo && !g_combo_prev) {
        rex_nx_menu_open = !rex_nx_menu_open;
        rex_nx_menu_cursor = 0;
        rex_nx_menu_page = 0;
        g_neutral = true;
        g_menu_via_sticks = !start_back;
      }
      g_combo_prev = combo;
      if (rex_nx_menu_open) {
        const uint16_t pressed = all & ~g_prev_all;
        const int max_cursor = (rex_nx_menu_page ? kCheatItems : kMainItems) - 1;
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
            rex_nx_menu_page = 0;
            rex_nx_menu_cursor = 6;
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
        std::printf("[nx] menu: %s (%s)\n", rex_nx_menu_open ? "open" : "closed",
                    rex_nx_menu_open ? (g_menu_via_sticks ? "l3+r3" : "start+back") : "combo or menu button");
        std::fflush(stdout);
      }
      g_prev_all = all;
      if (g_neutral && !all && !rex_nx_menu_open) g_neutral = false;
      neutral_now = g_neutral || rex_nx_menu_open;
    }
    if (neutral_now) {
      all = 0;
      scheduled_lt = scheduled_rt = false;
      held &= ~(uint64_t(HidNpadButton_ZL) | uint64_t(HidNpadButton_ZR));
    }
    out_state->gamepad.buttons = all;
    out_state->gamepad.left_trigger = (held & HidNpadButton_ZL) || scheduled_lt ? 0xFF : 0;
    out_state->gamepad.right_trigger = (held & HidNpadButton_ZR) || scheduled_rt ? 0xFF : 0;
    out_state->gamepad.thumb_lx = neutral_now ? 0 : ClampStick(left.x);
    out_state->gamepad.thumb_ly = neutral_now ? 0 : ClampStick(left.y);
    out_state->gamepad.thumb_rx = neutral_now ? 0 : ClampStick(right.x);
    out_state->gamepad.thumb_ry = neutral_now ? 0 : ClampStick(right.y);
    if (held != last_buttons_) {
      packet_number_++;
      last_buttons_ = held;
    }
    out_state->packet_number = packet_number_;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NXInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  return id == kPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT NXInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  return id == kPad ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace rex::input::nx
