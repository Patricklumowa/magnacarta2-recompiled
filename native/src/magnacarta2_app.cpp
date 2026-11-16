#include "magnacarta2_app.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/perf/counter.h>
#include <rex/ui/flags.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/input/input_system.h>
#include <imgui.h>
#include <rex/filesystem.h>

#include <algorithm>
#include <stdexcept>
#ifdef MC2_ENABLE_NATIVE_BOOT
#include "native_boot.h"
#include <rex/ui/window.h>
#endif

namespace {
class PCSettingsDialog final : public rex::ui::ImGuiDialog {
 public:
  PCSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                   std::filesystem::path config, std::function<void()> on_close)
      : ImGuiDialog(drawer), input_(input), config_(std::move(config)), on_close_(std::move(on_close)) {
    if (input_) input_->AddUIInputBlocker();
  }
  ~PCSettingsDialog() override { if (input_) input_->RemoveUIInputBlocker(); }

 private:
  void OnClose() override { on_close_(); }
  void OnDraw(ImGuiIO&) override {
    bool open = true;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Magna Carta 2 - PC Settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
      int preset = std::clamp(REXCVAR_QUERY(int32_t, mc2_render_scale) - 1, 0, 2);
      if (ImGui::Combo("Rendering resolution", &preset,
                       "1280 x 720 (720p)\0" "2560 x 1440 (1440p)\0" "3840 x 2160 (4K)\0")) {
        rex::cvar::SetFlagByName("mc2_render_scale", std::to_string(preset + 1));
      }
      const auto rate = REXCVAR_QUERY(std::string, mc2_framerate);
      int frame_rate = rate == "unlocked" ? 2 : rate == "30" ? 0 : 1;
      if (ImGui::Combo("Frame rate", &frame_rate, "30 FPS\0" "60 FPS\0" "Unlocked (experimental)\0")) {
        rex::cvar::SetFlagByName("mc2_framerate", frame_rate == 2 ? "unlocked" : frame_rate == 0 ? "30" : "60");
      }
      ImGui::TextDisabled("Resolution and frame rate apply after restarting the game.");
      ImGui::Separator();
      bool sound = !REXCVAR_QUERY(bool, audio_mute);
      if (ImGui::Checkbox("Sound", &sound)) {
        rex::cvar::SetFlagByName("audio_mute", sound ? "false" : "true");
      }
      bool fullscreen = REXCVAR_GET(fullscreen);
      if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
      }
      if (ImGui::Button("Save settings")) {
        rex::cvar::SaveConfig(config_);
        saved_ = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) open = false;
      if (saved_) ImGui::TextDisabled("Settings saved.");
    }
    ImGui::End();
    if (!open) Close();
  }
  rex::input::InputSystem* input_;
  std::filesystem::path config_;
  std::function<void()> on_close_;
  bool saved_ = false;
};
}  // namespace

REXCVAR_DEFINE_STRING(mc2_framerate, "60", "Game", "Game frame-rate mode (requires restart)")
    .allowed({"30", "60", "unlocked"});
REXCVAR_DEFINE_BOOL(mc2_profile, false, "Development", "Enable a local Tracy profiling capture");
REXCVAR_DEFINE_INT32(mc2_render_scale, 2, "Game",
                     "Internal render scale: 1 = 720p, 2 = 1440p, 3 = 4K (requires restart)")
    .range(1, 3);

void MagnaCarta2App::OnPreSetup(rex::RuntimeConfig& config) {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootRequested()) {
    config.graphics = mc2::native::CreateNativeBootGraphics();
    config.gpu_plugin.clear();
  }
#endif
  // Guest vblank timing is independent of the PC monitor's refresh rate.
  REXCVAR_SET(video_mode_refresh_rate, 60.0);
  // Keep the game's display/layout coordinates consistent, and scale actual
  // GPU rasterization rather than stretching a console-sized image.
  REXCVAR_SET(video_mode_width, 1280);
  REXCVAR_SET(video_mode_height, 720);
  REXCVAR_SET(resolution, "720p");
}

std::optional<rex::PathConfig> MagnaCarta2App::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)>) {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootActive()) return defaults;
#endif
  // The GPU plugin and its settings now exist; draw resources are allocated
  // later during Runtime::Setup(), so apply the scale before that step.
  const auto scale = std::to_string(REXCVAR_GET(mc2_render_scale));
  for (const auto* flag : {"resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y"}) {
    if (!rex::cvar::SetFlagByName(flag, scale)) {
      throw std::runtime_error("GPU plugin does not support the requested internal resolution scale");
    }
  }
  rex::cvar::SetFlagByName("vsync", REXCVAR_GET(mc2_framerate) == "unlocked" ? "false" : "true");
  REXLOG_INFO("MC2: {}x internal rendering ({}x{}), 60 Hz guest timing",
               scale, 1280 * REXCVAR_GET(mc2_render_scale), 720 * REXCVAR_GET(mc2_render_scale));
  return defaults;
}

void MagnaCarta2App::OnPostSetup() {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootActive())
    mc2::native::SetNativeBootWindow(window()->GetNativeWindowHandle());
#endif
  REXLOG_INFO("MC2: requested game frame-rate mode {}", REXCVAR_GET(mc2_framerate));
  if (REXCVAR_GET(mc2_profile)) rex::perf::Profiler::Startup();
}

void MagnaCarta2App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::RegisterBind("bind_settings", "F4", "PC settings", [this, drawer] {
    if (pc_settings_) {
      delete pc_settings_;
      pc_settings_ = nullptr;
    } else {
      auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system()) : nullptr;
      const auto config_path = pc_config_path_.empty()
          ? rex::filesystem::GetExecutableFolder() / "magnacarta2.toml" : pc_config_path_;
      pc_settings_ = new PCSettingsDialog(drawer, input, config_path, [this] { pc_settings_ = nullptr; });
    }
  });
}

void MagnaCarta2App::OnShutdown() {
  rex::ui::UnregisterBind("bind_settings");
  delete pc_settings_;
  pc_settings_ = nullptr;
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> MagnaCarta2App::CreateAchievementNotificationDialog() {
  // ponytail: keep host achievement toasts off until the SDK can detach an
  // idle toast. Its permanently registered, empty dialog forces redundant
  // presents and prevents guest-driven presentation. Achievement data remains active.
  return nullptr;
}
