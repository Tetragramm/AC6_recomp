#pragma once

#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace ac6::graphics {

class NativeGraphicsStatusDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit NativeGraphicsStatusDialog(rex::ui::ImGuiDrawer* imgui_drawer);
  ~NativeGraphicsStatusDialog();

  void Show() { visible_ = true; }
  void ToggleVisible() { visible_ = !visible_; }
  // Effective visibility, not just the toggle: performance mode suppresses
  // the window entirely (see OnDraw), and a window that never draws must not
  // count as visible for the drawer's input-ownership aggregate - the app
  // calls Show() at startup, so the raw flag alone would report a window on
  // every plain user run.
  bool IsVisible() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
};

}  // namespace ac6::graphics
