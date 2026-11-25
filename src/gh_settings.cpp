// In-game settings window, see gh_settings.h. The key capture follows the SDK's settings overlay
// (rex-sdk, BSD-3): ImGui key -> VirtualKey -> name, written to the keybind_* cvars.
#include "gh_settings.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>


// Back (Select) is used by the game (it skips cut scenes: "Press BACK to Skip", and appears in menu prompts), so a
// short press must reach the game: the window opens on a hold. F1 and Escape open it too.
REXCVAR_DEFINE_STRING(menu_open, "start_back", "Input", "How a gamepad opens the settings window: start_back (Start and Back together), hold_back (hold Back) or esc (keyboard only: F1 and Esc always work)")
    .allowed({"start_back", "hold_back", "esc"});
REXCVAR_DEFINE_DOUBLE(menu_open_hold, 1.0, "Input", "Seconds Back/Select must be held with menu_open = hold_back");

namespace {

using VK = rex::ui::VirtualKey;

VK ImGuiKeyToVirtualKey(ImGuiKey key) {
  auto off = [](VK base, int n) { return static_cast<VK>(static_cast<uint16_t>(base) + n); };
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return off(VK::kA, key - ImGuiKey_A);
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return off(VK::k0, key - ImGuiKey_0);
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return off(VK::kF1, key - ImGuiKey_F1);
  switch (key) {
    case ImGuiKey_Space: return VK::kSpace;
    case ImGuiKey_Enter: return VK::kReturn;
    case ImGuiKey_Tab: return VK::kTab;
    case ImGuiKey_Backspace: return VK::kBack;
    case ImGuiKey_Delete: return VK::kDelete;
    case ImGuiKey_Insert: return VK::kInsert;
    case ImGuiKey_Home: return VK::kHome;
    case ImGuiKey_End: return VK::kEnd;
    case ImGuiKey_PageUp: return VK::kPrior;
    case ImGuiKey_PageDown: return VK::kNext;
    case ImGuiKey_LeftArrow: return VK::kLeft;
    case ImGuiKey_RightArrow: return VK::kRight;
    case ImGuiKey_UpArrow: return VK::kUp;
    case ImGuiKey_DownArrow: return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return VK::kShift;
    case ImGuiKey_Minus: return VK::kOemMinus;
    case ImGuiKey_Equal: return VK::kOemPlus;
    case ImGuiKey_LeftBracket: return VK::kOem4;
    case ImGuiKey_RightBracket: return VK::kOem6;
    case ImGuiKey_Backslash: return VK::kOem5;
    case ImGuiKey_Semicolon: return VK::kOem1;
    case ImGuiKey_Apostrophe: return VK::kOem7;
    case ImGuiKey_Comma: return VK::kOemComma;
    case ImGuiKey_Period: return VK::kOemPeriod;
    case ImGuiKey_Slash: return VK::kOem2;
    default: return VK::kNone;
  }
}

// The game's pad actions with the SDK cvar behind each.
struct Bind {
  const char* label;
  const char* cvar;
};
constexpr Bind kBinds[] = {
    {"Move up", "keybind_lstick_up"},      {"Move down", "keybind_lstick_down"},
    {"Move left", "keybind_lstick_left"},  {"Move right", "keybind_lstick_right"},
    {"A (confirm / attack)", "keybind_a"}, {"B (cancel / special)", "keybind_b"},
    {"X", "keybind_x"},                    {"Y", "keybind_y"},
    {"Left shoulder", "keybind_left_shoulder"}, {"Right shoulder", "keybind_right_shoulder"},
    {"Left trigger", "keybind_left_trigger"},   {"Right trigger", "keybind_right_trigger"},
    {"D-pad up", "keybind_dpad_up"},       {"D-pad down", "keybind_dpad_down"},
    {"D-pad left", "keybind_dpad_left"},   {"D-pad right", "keybind_dpad_right"},
    {"Start", "keybind_start"},            {"Back / Select", "keybind_back"},
};

bool CvarBool(const char* name);
std::string CvarValue(const char* name) {
  for (auto& e : rex::cvar::GetRegistry())
    if (e.name == name) return e.getter();
  return {};
}

bool CvarBool(const char* name) { return CvarValue(name) == "true"; }
void SetBool(const char* name, bool v) { rex::cvar::SetFlagByName(name, v ? "true" : "false"); }

}  // namespace

GhSettings::GhSettings(rex::ui::ImGuiDrawer* drawer, std::function<rex::Runtime*()> runtime,
                       std::function<rex::ui::Window*()> window, std::filesystem::path config_path)
    : ImGuiDialog(drawer), runtime_(std::move(runtime)), window_(std::move(window)), config_path_(std::move(config_path)) {
  rex::ui::RegisterBind("bind_gh_settings", "F1", "Toggle the settings window", [this] { Toggle(); });
  rex::ui::RegisterBind("bind_gh_settings_esc", "Escape", "Toggle the settings window (Esc)", [this] { Toggle(); });
}

void GhSettings::Toggle() {
  open_ = !open_;
  capturing_.clear();
  status_.clear();
}

// Start+Back together (menu_open = start_back, default), a hold of Back (hold_back) or only the keyboard (esc) open and
// close the window; while it is open the pad drives ImGui.
void GhSettings::PollPad(ImGuiIO& io) {
  auto* input = runtime_() ? static_cast<rex::input::InputSystem*>(runtime_()->input_system()) : nullptr;
  if (!input) return;
  rex::input::X_INPUT_STATE st = {};
  bool connected = false;
  for (uint32_t user = 0; user < 4 && !connected; ++user)
    connected = input->GetStateRaw(user, &st) == 0;
  const uint16_t b = connected ? static_cast<uint16_t>(st.gamepad.buttons) : 0;
  const bool back = (b & rex::input::X_INPUT_GAMEPAD_BACK) != 0;
  const bool start = (b & rex::input::X_INPUT_GAMEPAD_START) != 0;
  const std::string mode = REXCVAR_GET(menu_open);
  if (mode == "start_back") {
    // Start and Back together (Switch + and -): the game keeps short presses of either alone; while the
    // combo is down and until both are released the game gets a neutral pad.
    const bool combo = start && back;
    if (combo && !combo_prev_) {
      Toggle();
      combo_wait_release_ = true;
    }
    combo_prev_ = combo;
    if (combo_wait_release_ && !start && !back) combo_wait_release_ = false;
  } else if (mode == "hold_back") {
    if (back) {
      back_held_ += io.DeltaTime;
      if (back_held_ >= float(REXCVAR_GET(menu_open_hold)) && !back_fired_) {
        back_fired_ = true;
        Toggle();
      }
    } else {
      back_held_ = 0.0f;
      back_fired_ = false;
    }
  }
  // The SDK shows the guest's message boxes as ImGui popups (the save-data question): while one is up
  // the pad drives it, not the game.
  const bool ui = open_ || combo_wait_release_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  if (ui != blocked_) {
    blocked_ = ui;
    rex::input::InputSystem::SetGameInputBlocked(ui);
  }
  if (!ui) return;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
  using namespace rex::input;
  key(ImGuiKey_GamepadDpadUp, b & X_INPUT_GAMEPAD_DPAD_UP);
  key(ImGuiKey_GamepadDpadDown, b & X_INPUT_GAMEPAD_DPAD_DOWN);
  key(ImGuiKey_GamepadDpadLeft, b & X_INPUT_GAMEPAD_DPAD_LEFT);
  key(ImGuiKey_GamepadDpadRight, b & X_INPUT_GAMEPAD_DPAD_RIGHT);
  key(ImGuiKey_GamepadFaceDown, b & X_INPUT_GAMEPAD_A);   // activate
  key(ImGuiKey_GamepadFaceRight, b & X_INPUT_GAMEPAD_B);  // cancel
  key(ImGuiKey_GamepadL1, b & X_INPUT_GAMEPAD_LEFT_SHOULDER);
  key(ImGuiKey_GamepadR1, b & X_INPUT_GAMEPAD_RIGHT_SHOULDER);
}

void GhSettings::DrawDisplay() {
  bool fullscreen = window_()->IsFullscreen();
  if (ImGui::Checkbox("Fullscreen", &fullscreen)) window_()->SetFullscreen(fullscreen);

  bool letterbox = CvarBool("present_letterbox");
  int aspect = letterbox ? 0 : 1;
  const char* aspects[] = {"16:9 (black bars when the window differs)", "Stretch to the window"};
  ImGui::SetNextItemWidth(320);
  if (ImGui::Combo("Aspect", &aspect, aspects, 2)) SetBool("present_letterbox", aspect == 0);

  bool vsync = CvarBool("vsync");
  if (ImGui::Checkbox("VSync (60 fps)", &vsync)) SetBool("vsync", vsync);

  bool fps = CvarBool("debug_overlay");
  if (ImGui::Checkbox("FPS counter", &fps)) SetBool("debug_overlay", fps);

  int scale = std::max(1, std::atoi(CvarValue("resolution_scale").c_str()));
  const char* scales[] = {"1x (1280x720)", "2x (2560x1440)", "3x (3840x2160)"};
  int idx = std::clamp(scale, 1, 3) - 1;
  ImGui::SetNextItemWidth(220);
  if (ImGui::Combo("Render scale (restart)", &idx, scales, 3)) {
    const std::string v = std::to_string(idx + 1);
    rex::cvar::SetFlagByName("resolution_scale", v);
    rex::cvar::SetFlagByName("draw_resolution_scale_x", v);
    rex::cvar::SetFlagByName("draw_resolution_scale_y", v);
  }

  const char* fx[] = {"bilinear", "cas", "fsr"};
  std::string cur = CvarValue("present_effect");
  int fxi = cur == "cas" ? 1 : cur == "fsr" ? 2 : 0;
  ImGui::SetNextItemWidth(220);
  if (ImGui::Combo("Upscaler (restart)", &fxi, fx, 3)) rex::cvar::SetFlagByName("present_effect", fx[fxi]);
}

void GhSettings::DrawAudio() {
  bool mute = CvarBool("audio_mute");
  if (ImGui::Checkbox("Mute", &mute)) SetBool("audio_mute", mute);
  ImGui::SetNextItemWidth(320);
  if (ImGui::SliderFloat("Volume", &volume_, 0.0f, 100.0f, "%.0f%%"))
    rex::audio::SetOutputGain(volume_ / 100.0f);
}

void GhSettings::DrawControls() {
  bool mnk = CvarBool("mnk_mode");
  if (ImGui::Checkbox("Keyboard plays as pad 1", &mnk)) SetBool("mnk_mode", mnk);
  ImGui::TextDisabled("Gamepad: press Start and Back together (menu_open) to open or close this window; each alone still goes to the game. F1 and Esc do the same.");
  ImGui::Separator();
  for (const Bind& bind : kBinds) {
    ImGui::PushID(bind.cvar);
    ImGui::Text("%-24s", bind.label);
    ImGui::SameLine(250.0f);
    if (capturing_ == bind.cvar) {
      ImGui::Button("Press a key... (Esc cancels)", ImVec2(230, 0));
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        capturing_.clear();
      } else {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
          const auto key = static_cast<ImGuiKey>(k);
          if (key == ImGuiKey_Escape || !ImGui::IsKeyPressed(key)) continue;
          const std::string name = rex::ui::VirtualKeyToString(ImGuiKeyToVirtualKey(key));
          if (!name.empty()) rex::cvar::SetFlagByName(bind.cvar, name);
          capturing_.clear();
          break;
        }
      }
    } else {
      ImGui::Text("%-16s", CvarValue(bind.cvar).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Rebind")) capturing_ = bind.cvar;
      ImGui::SameLine();
      if (ImGui::SmallButton("Reset"))
        for (auto& e : rex::cvar::GetRegistry())
          if (e.name == bind.cvar) rex::cvar::SetFlagByName(bind.cvar, e.default_value);
    }
    ImGui::PopID();
  }
}

void GhSettings::DrawCheats() {
  ImGui::TextDisabled("Take effect at once. Save to gh.toml to keep them.");
  const struct { const char* cvar; const char* label; } cheats[] = {
      {"cheat_invincible", "Health always full"}, {"cheat_mp", "Magic always full"},
      {"cheat_continues", "99 continues"}, {"cheat_attributes", "All attributes at maximum"}};
  for (const auto& c : cheats) {
    bool on = CvarBool(c.cvar);
    if (ImGui::Checkbox(c.label, &on)) SetBool(c.cvar, on);
  }
}

void GhSettings::OnDraw(ImGuiIO& io) {
  PollPad(io);
  if (!open_) return;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(0.92f);
  bool keep = true;
  if (ImGui::Begin("Guardian Heroes: settings", &keep, ImGuiWindowFlags_NoCollapse)) {
    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("Display")) { DrawDisplay(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Audio")) { DrawAudio(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Controls")) { DrawControls(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Cheats")) { DrawCheats(); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save to gh.toml")) {
      rex::cvar::SaveConfig(config_path_);
      status_ = "Saved " + config_path_.string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) keep = false;
    if (!status_.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", status_.c_str());
    }
  }
  ImGui::End();
  if (!keep) Toggle();
}
