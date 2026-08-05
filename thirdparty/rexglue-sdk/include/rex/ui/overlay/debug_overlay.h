/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <cstdint>
#include <functional>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~DebugOverlayDialog();

  void ToggleVisible() { visible_ = !visible_; }
  bool IsVisible() const override { return visible_; }
  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
  FrameStatsProvider stats_provider_;

  // Frametime graph + throttled counter state.
  static constexpr int kHistory = 240;
  static constexpr double kCounterRefreshSeconds = 0.5;

  // Ring buffers of frame times in milliseconds. Host = the presenter/paint
  // cadence (what an external tool like RTSS would measure); guest = the
  // game's own per-frame sim delta (where framerate-dependent judder lives).
  float host_ms_history_[kHistory] = {};
  float guest_ms_history_[kHistory] = {};
  int host_history_pos_ = 0;
  int guest_history_pos_ = 0;
  uint64_t last_guest_frame_count_ = 0;

  // Graph samples are pushed at a FIXED TIME interval (not per frame), so the
  // graphs scroll at a consistent rate regardless of fps. kHistory samples then
  // cover a fixed time window (240 * 1/60 s = ~4 s). Each sample is the max
  // frametime since the last sample, so spikes between samples are preserved.
  static constexpr double kGraphSampleSeconds = 1.0 / 60.0;
  double graph_sample_accum_s_ = 0.0;
  float graph_host_ms_max_ = 0.0f;
  float graph_guest_ms_max_ = 0.0f;

  // Counter averaging window (refreshed every kCounterRefreshSeconds so the
  // numbers are readable instead of flickering every frame).
  double counter_window_s_ = 0.0;
  int host_frames_in_window_ = 0;
  uint64_t guest_frames_at_window_start_ = 0;
  double window_guest_ms_min_ = 1.0e9;
  double window_guest_ms_max_ = 0.0;

  // Held (displayed) counter values.
  double disp_host_fps_ = 0.0;
  double disp_host_ms_ = 0.0;
  double disp_guest_fps_ = 0.0;
  double disp_guest_ms_ = 0.0;
  double disp_guest_ms_min_ = 0.0;
  double disp_guest_ms_max_ = 0.0;
};

}  // namespace rex::ui
