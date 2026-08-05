/**
 * @file        rex/ui/overlay/build_stamp_overlay.h
 * @brief       Always-on build-stamp watermark (short git commit hash) drawn in
 *              the lower-left corner, independent of the F3 debug overlay.
 *
 * @copyright   Copyright (c) 2026 Project Gracemeria
 * @license     BSD 3-Clause License
 */
#pragma once
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// A non-interactive dialog that draws the build's short git commit hash in the
// lower-left corner every frame. Registers itself with the ImGuiDrawer on
// construction (like any ImGuiDialog). Drawing is gated by the show_build_stamp
// cvar (on by default); set it false to hide the watermark.
class BuildStampOverlay : public ImGuiDialog {
 public:
  explicit BuildStampOverlay(ImGuiDrawer* imgui_drawer);

  // The watermark is drawn with NoInputs|NoNav: ImGui can neither hover nor
  // focus it, so it can never own the mouse or keyboard. It is also always
  // attached - reporting it visible would keep the drawer's dialog input
  // ownership permanently live and void the closed-overlay guarantee the
  // real overlay windows rely on. It is a stamp, not a window.
  bool IsVisible() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace rex::ui
