#pragma once

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

#include "ac6_native_graphics.h"
#include "ac6_native_graphics_overlay.h"
#include "render_hooks.h"
#include "generated/ac6recomp_config.h"

REXCVAR_DECLARE(std::string, ac6_graphics_backend);

// Defined in main.cpp: the config preset suite (session defaults + fix
// payload expansion) and its post-logging summary lines.
void ApplyAc6ConfigPresets();
void LogAc6ConfigPresetSummary();

class Ac6recompApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  Ac6recompApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo ppc_info)
      : rex::ReXApp(ctx, name, ppc_info) {
    REXLOG_INFO("Ac6recompApp constructor");
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    REXLOG_INFO("Ac6recompApp::Create");
    return std::unique_ptr<Ac6recompApp>(new Ac6recompApp(ctx, "ac6recomp", PPCImageConfig));
  }

 protected:
  // Ace Combat 6 title id: selects the right .iso when several are present
  // and rejects wrong/corrupt images with a clear message.
  uint32_t OnGetExpectedTitleId() const override { return 0x4E4D07D1; }

  // Runs right after the toml is parsed and before anything consumes cvars:
  // the one place presets can both see the user's real values and still act
  // before logging/window/graphics read the results.
  void OnConfigLoaded() override { ApplyAc6ConfigPresets(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    REXLOG_INFO("Ac6recompApp::OnPreSetup");
    rex::ReXApp::OnPreSetup(config);
    LogAc6ConfigPresetSummary();

    const std::string requested_backend = REXCVAR_GET(ac6_graphics_backend);
#if REX_HAS_VULKAN
    if (requested_backend == "vulkan" || requested_backend == "auto") {
      config.graphics =
          REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
      REXLOG_INFO("Ac6recompApp: selected Vulkan graphics backend");
      return;
    }
#endif
#if REX_HAS_D3D12
    if (requested_backend == "d3d12" || requested_backend == "auto") {
      config.graphics =
          REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
      REXLOG_INFO("Ac6recompApp: selected D3D12 graphics backend");
      return;
    }
#endif
    REXLOG_WARN("Ac6recompApp: requested graphics backend '{}' is not available in this build",
                requested_backend);
  }

  void OnPostSetup() override {
    REXLOG_INFO("Ac6recompApp::OnPostSetup");
    rex::ReXApp::OnPostSetup();

    auto* graphics_sys = runtime()->graphics_system();
    if (graphics_sys) {
        graphics_sys->SetFrameBoundaryCallback([](rex::memory::Memory* memory) {
            ::ac6::graphics::OnFrameBoundary(memory);
        });
        REXLOG_INFO("Ac6recompApp: Native frame boundary callback registered");
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    REXLOG_INFO("Ac6recompApp::OnCreateDialogs");
    native_graphics_status_dialog_ =
        std::make_unique<ac6::graphics::NativeGraphicsStatusDialog>(drawer);
    native_graphics_status_dialog_->Show();

    // Feed the F3 debug overlay the game's own frame stats so its guest
    // frametime graph and counter reflect the real sim cadence (SDK cannot
    // reach into the AC6 app layer, so it takes them through this callback).
    // Must be here, not OnPostSetup: the debug overlay is created just before
    // OnCreateDialogs, but OnPostSetup runs BEFORE the overlay exists, so a
    // provider set there is silently dropped.
    SetGuestFrameStats([]() -> rex::ui::FrameStats {
        const ::ac6::FrameStats s = ::ac6::GetFrameStats();
        return rex::ui::FrameStats{s.frame_time_ms, s.fps, s.frame_count};
    });
  }

 private:
  std::unique_ptr<ac6::graphics::NativeGraphicsStatusDialog> native_graphics_status_dialog_;
};

