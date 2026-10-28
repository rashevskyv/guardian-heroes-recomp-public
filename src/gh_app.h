// gh - Guardian Heroes (Xbox 360 XBLA) recompiled with ReXGlue
#pragma once

#include <rex/rex_app.h>

#if !defined(__SWITCH__)
#include <rex/cvar.h>
#include "gh_settings.h"
REXCVAR_DECLARE(uint32_t, license_mask);
REXCVAR_DECLARE(std::string, gpu_plugin);
REXCVAR_DECLARE(bool, mnk_mode);
#endif

#if defined(__SWITCH__)
#include <rex/cvar.h>
REXCVAR_DECLARE(int32_t, audio_maxqframes);
#endif

class GhApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GhApp>(new GhApp(ctx, "gh", PPCImageConfig));
  }

#if defined(__SWITCH__)
  // Short audio queue for the audout driver (src/nx_audout_driver.h): 8 frames x 5.33 ms.
  void OnConfigurePaths(rex::PathConfig&) override { REXCVAR_SET(audio_maxqframes, 8); }
#else
  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_ = paths.config_path;
    // No --game_data_root: use the "game" folder next to the executable (default.xex + media/ inside).
    if (paths.game_data_root.empty() && std::filesystem::exists(config_path_.parent_path() / "game" / "default.xex"))
      paths.game_data_root = config_path_.parent_path() / "game";
    // Without a licence the game runs as a trial (locked menu entries); a config file or --license_mask still wins.
    if (REXCVAR_GET(license_mask) == 0) REXCVAR_SET(license_mask, 0xFFFFFFFFu);
    if (REXCVAR_GET(gpu_plugin).empty()) REXCVAR_SET(gpu_plugin, "xenos");
    // First start (no gh.toml yet): the keyboard plays as pad 1; the settings window can switch it off.
    if (!std::filesystem::exists(config_path_)) REXCVAR_SET(mnk_mode, true);
  }

  // The window deletes itself with the drawer; it opens on Back/Select (held) or F1.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    new GhSettings(drawer, [this] { return runtime(); }, [this] { return window(); }, config_path_);
  }

 private:
  std::filesystem::path config_path_;
#endif
};
