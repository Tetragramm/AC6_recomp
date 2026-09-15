/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include "rex/system/function_dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/graphics/register_file.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

// Forward declarations
namespace rex {
class Memory;
namespace stream {
class ByteStream;
}  // namespace stream
}  // namespace rex

namespace rex::ui {
class WindowedAppContext;
}  // namespace rex::ui

namespace rex::graphics {

class CommandProcessor;

class GraphicsSystem : public system::IGraphicsSystem {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  system::KernelState* kernel_state() const { return kernel_state_; }
  ::rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  ::rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  void Shutdown() override;

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }

  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  virtual void SetFrameBoundaryCallback(std::function<void(rex::memory::Memory*)> callback) override;
  virtual bool HandleVideoSwap(const system::GraphicsSwapSubmission& submission) override;
  bool GetLastSwapSubmission(system::GraphicsSwapSubmission* out_submission,
                             uint64_t* out_sequence = nullptr) const;
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  // Runs a guest function on the calling (command-processor) thread, the way
  // the interrupt callback is run. For game-specific hooks that need to poke
  // the guest's own GPU bookkeeping from the CP side.
  uint64_t DispatchGuestCall(uint32_t address, uint32_t arg0, uint32_t arg1 = 0);
  uint32_t interrupt_callback_data() const { return interrupt_callback_data_; }

  // Called by the command processor when a WAIT_REG_MEM on guest memory is
  // not satisfied and is about to sleep. Lets a game-specific fix advance
  // whatever guest-side state the wait depends on (AC6's swap flag is cleared
  // by the vblank ISR, which vsync-throttles the ring).
  using WaitMemoryHook = std::function<void(uint32_t guest_physical_address)>;
  void SetWaitMemoryHook(WaitMemoryHook hook) { wait_memory_hook_ = std::move(hook); }
  void OnWaitMemory(uint32_t guest_physical_address) {
    if (wait_memory_hook_) {
      wait_memory_hook_(guest_physical_address);
    }
  }

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);

  void RequestFrameTrace();
  void BeginTracing();
  void EndTracing();

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  uint64_t guest_vblank_interval_ticks() const {
    return guest_vblank_interval_ticks_.load(std::memory_order_acquire);
  }
  uint64_t last_vblank_interrupt_guest_tick() const {
    return last_vblank_interrupt_guest_tick_.load(std::memory_order_acquire);
  }

  // Per-game override of the guest vblank rate, in Hz. 0 = no override (use
  // the vsync/guest_vblank_sync_to_refresh cvar logic). Lets game-specific
  // timing hooks pace frame-locked content (menus, cinematics) at the native
  // rate while gameplay free-runs. Process-wide, not per-instance.
  static void SetGuestVblankHzOverride(double hz);
  static double GetGuestVblankHzOverride();

  // Modern present pacing: while target_hz > 0, PaceGuestPresent (called by
  // VdSwap on the guest thread that submits swaps) blocks that thread until
  // every previously issued swap has been delivered to the presenter (GPU
  // backpressure, frame latency 1), then rate-limits it to target_hz with an
  // absolute-deadline limiter. The game then runs at min(target, real GPU
  // throughput) with uniform frame times - the modern game loop - instead of
  // quantizing to vblank-grid sub-harmonics (60/30/20) when the GPU cannot
  // hold the target. Combine with a free-running guest vblank so the vblank
  // wait never gates. target_hz 0 = off. Process-wide, not per-instance.
  static void SetGuestPresentPacing(double target_hz);
  // Current present-pacing target, 0 when the pacer is not gating the swap.
  static double GetGuestPresentPacing();
  // Called by VdSwap on the swapping guest thread before it submits the next
  // swap; blocks per SetGuestPresentPacing. No-op while pacing is off.
  static void PaceGuestPresent();
  // Called by the command processor whenever a guest frame is actually
  // delivered to the presenter (guest output refreshed for a swap).
  static void NotifyGuestPresent();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  GraphicsSystem();

  // Backends build their provider here. Called lazily from either setup
  // entry point; with_presentation is false only on headless guest-GPU paths.
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                 uint32_t value);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  ::rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<::rex::ui::GraphicsProvider> provider_;
  bool provider_supports_presentation_ = false;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> vsync_worker_running_;
  system::object_ref<system::XHostThread> vsync_worker_thread_;
  std::atomic<uint64_t> guest_vblank_interval_ticks_{0};
  std::atomic<uint64_t> last_vblank_interrupt_guest_tick_{0};
  mutable std::mutex last_swap_submission_mutex_;
  system::GraphicsSwapSubmission last_swap_submission_{};
  uint64_t last_swap_submission_sequence_ = 0;

  std::function<void(rex::memory::Memory*)> frame_boundary_callback_;
  WaitMemoryHook wait_memory_hook_;

  RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

 private:
  std::unique_ptr<::rex::ui::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::graphics
