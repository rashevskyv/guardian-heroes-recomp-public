// In-game settings window (PC): opens on the pad's Back/Select (hold 0.5 s) or F1, edits the runtime's
// cvars, writes them to gh.toml. While it is open the game sees a neutral pad (patches/sdk-edits-gh.py).
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
namespace ui {
class Window;
}
}  // namespace rex

class GhSettings : public rex::ui::ImGuiDialog {
 public:
  // The runtime and the window do not exist yet when OnCreateDialogs runs: read them through the app.
  GhSettings(rex::ui::ImGuiDrawer* drawer, std::function<rex::Runtime*()> runtime,
             std::function<rex::ui::Window*()> window, std::filesystem::path config_path);

  void Toggle();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void PollPad(ImGuiIO& io);
  void DrawDisplay();
  void DrawAudio();
  void DrawControls();
  void DrawCheats();

  std::function<rex::Runtime*()> runtime_;
  std::function<rex::ui::Window*()> window_;
  std::filesystem::path config_path_;
  bool open_ = false;
  bool blocked_ = false;
  float back_held_ = 0.0f;
  bool back_fired_ = false;
  bool combo_prev_ = false;
  bool combo_wait_release_ = false;
  float volume_ = 100.0f;
  std::string capturing_;
  std::string status_;
};
