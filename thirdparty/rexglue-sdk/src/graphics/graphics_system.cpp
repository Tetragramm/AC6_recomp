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

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");

REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "GPU", "Enable GPU trace streaming");
REXCVAR_DEFINE_BOOL(guest_vblank_sync_to_refresh, false, "GPU",
                    "Keep guest VBlank cadence tied to the guest refresh rate even when host "
                    "vsync is disabled");

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
constexpr bool kStoreShaders = true;

bool HasMeaningfulTextureFetch(const rex::system::GraphicsSwapSubmission& submission) {
  for (uint32_t word : submission.texture_fetch) {
    if (word != 0) {
      return true;
    }
  }
  return false;
}

rex::system::GraphicsSwapSubmission MergeSwapSubmission(
    const rex::system::GraphicsSwapSubmission& base,
    const rex::system::GraphicsSwapSubmission& incoming) {
  rex::system::GraphicsSwapSubmission merged = base;
  if (incoming.frontbuffer_virtual_address) {
    merged.frontbuffer_virtual_address = incoming.frontbuffer_virtual_address;
  }
  if (incoming.frontbuffer_physical_address) {
    merged.frontbuffer_physical_address = incoming.frontbuffer_physical_address;
  }
  if (incoming.frontbuffer_width) {
    merged.frontbuffer_width = incoming.frontbuffer_width;
  }
  if (incoming.frontbuffer_height) {
    merged.frontbuffer_height = incoming.frontbuffer_height;
  }
  if (incoming.texture_format) {
    merged.texture_format = incoming.texture_format;
  }
  if (incoming.color_space) {
    merged.color_space = incoming.color_space;
  }
  if (HasMeaningfulTextureFetch(incoming)) {
    merged.texture_fetch = incoming.texture_fetch;
  }
  return merged;
}

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

namespace {
// Modern present pacing state (see SetGuestPresentPacing / PaceGuestPresent).
// The counters pair the guest's swap submissions (VdSwap) with the frames the
// command processor actually delivered to the presenter (NotifyGuestPresent).
// PaceGuestPresent blocks the swapping guest thread on that pairing plus an
// absolute-deadline frame limiter - GPU backpressure and a rate ceiling, the
// modern game loop - instead of pacing the guest off the vblank grid.
std::atomic<double> g_present_pacing_target_hz{0.0};
std::atomic<uint64_t> g_guest_present_count{0};
std::atomic<uint64_t> g_guest_swaps_issued{0};
std::mutex g_present_pacing_mutex;
std::condition_variable g_present_pacing_cv;
// Guarded by g_present_pacing_mutex.
std::chrono::steady_clock::time_point g_present_pacing_deadline{};
uint64_t g_present_delivery_at_last_timeout = UINT64_MAX;
}  // namespace

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
  uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
  uint64_t vsync_interval_ticks =
      std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
  uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
  guest_vblank_interval_ticks_.store(vsync_interval_ticks, std::memory_order_release);

  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this, vsync_interval_ticks,
                                                               no_vsync_interval_ticks,
                                                               guest_tick_frequency]() {
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
        while (vsync_worker_running_) {
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          uint64_t interval_ticks = REXCVAR_GET(guest_vblank_sync_to_refresh)
                                        ? vsync_interval_ticks
                                        : (REXCVAR_GET(vsync) ? vsync_interval_ticks
                                                              : no_vsync_interval_ticks);
          double vblank_hz_override = GetGuestVblankHzOverride();
          if (vblank_hz_override > 0.0) {
            interval_ticks = std::max(
                uint64_t(1), uint64_t(double(guest_tick_frequency) / vblank_hz_override));
          }
          // Re-anchor when far behind so a shrinking interval (an override
          // switching from a paced rate to a much faster one) or a long stall
          // does not burst a backlog of MarkVblank calls in one wake.
          if (current_time - last_frame_time >= interval_ticks * 4) {
            last_frame_time = current_time - interval_ticks;
          }
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  if (REXCVAR_GET(trace_gpu_stream)) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::SetFrameBoundaryCallback(std::function<void(rex::memory::Memory*)> callback) {
  frame_boundary_callback_ = std::move(callback);
}

bool GraphicsSystem::HandleVideoSwap(const system::GraphicsSwapSubmission& submission) {
  {
    std::lock_guard<std::mutex> lock(last_swap_submission_mutex_);
    last_swap_submission_ = MergeSwapSubmission(last_swap_submission_, submission);
    ++last_swap_submission_sequence_;
  }
  if (frame_boundary_callback_) {
    frame_boundary_callback_(memory_);
  }
  return false;
}

bool GraphicsSystem::GetLastSwapSubmission(system::GraphicsSwapSubmission* out_submission,
                                           uint64_t* out_sequence) const {
  std::lock_guard<std::mutex> lock(last_swap_submission_mutex_);
  if (out_submission) {
    *out_submission = last_swap_submission_;
  }
  if (out_sequence) {
    *out_sequence = last_swap_submission_sequence_;
  }
  return last_swap_submission_sequence_ != 0;
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);
  if (source == 0) {
    last_vblank_interrupt_guest_tick_.store(chrono::Clock::QueryGuestTickCount(),
                                            std::memory_order_release);
  }

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

namespace {
std::atomic<double> g_guest_vblank_hz_override{0.0};
}  // namespace

void GraphicsSystem::SetGuestVblankHzOverride(double hz) {
  g_guest_vblank_hz_override.store(hz, std::memory_order_relaxed);
}

double GraphicsSystem::GetGuestVblankHzOverride() {
  return g_guest_vblank_hz_override.load(std::memory_order_relaxed);
}

void GraphicsSystem::SetGuestPresentPacing(double target_hz) {
  g_present_pacing_target_hz.store(target_hz, std::memory_order_relaxed);
}

void GraphicsSystem::NotifyGuestPresent() {
  g_guest_present_count.fetch_add(1, std::memory_order_relaxed);
  // Empty critical section closes the race with PaceGuestPresent's predicate
  // check-then-wait, so the notify below cannot fall between them and be lost.
  { std::lock_guard<std::mutex> lock(g_present_pacing_mutex); }
  g_present_pacing_cv.notify_all();
}

void GraphicsSystem::PaceGuestPresent() {
  const double target_hz = g_present_pacing_target_hz.load(std::memory_order_relaxed);
  if (target_hz <= 0.0) {
    return;
  }

  // 1) Backpressure: before letting the game submit the next swap, wait until
  // every previously issued swap has been delivered to the presenter (frame
  // latency 1). This is what paces the game to the real GPU rate when it
  // cannot hold target_hz - uniformly, with no vblank grid to alias against.
  const uint64_t issued = g_guest_swaps_issued.load(std::memory_order_relaxed);
  {
    std::unique_lock<std::mutex> lock(g_present_pacing_mutex);
    const auto pred = [&] {
      return g_guest_present_count.load(std::memory_order_relaxed) >= issued;
    };
    const uint64_t delivered_now = g_guest_present_count.load(std::memory_order_relaxed);
    // While delivery is known-stalled (swaps are not reaching the presenter -
    // device loss, experimental swap paths), skip the wait entirely; the
    // limiter below still runs, degrading this to a plain frame-rate cap.
    const bool delivery_stalled = g_present_delivery_at_last_timeout != UINT64_MAX &&
                                  delivered_now == g_present_delivery_at_last_timeout;
    if (!delivery_stalled && !pred()) {
      if (g_present_pacing_cv.wait_for(lock, std::chrono::milliseconds(100), pred)) {
        g_present_delivery_at_last_timeout = UINT64_MAX;
      } else {
        // Timed out: forgive the whole backlog so one undelivered swap cannot
        // park every future frame on this timeout.
        g_present_delivery_at_last_timeout =
            g_guest_present_count.load(std::memory_order_relaxed);
        g_guest_swaps_issued.store(g_present_delivery_at_last_timeout,
                                   std::memory_order_relaxed);
        // Each of these is a 100ms stall of the guest's swap thread. A steady
        // stream of them means guest frames are not reaching the presenter and
        // the pacing gate is what is hitching the game.
        static std::atomic<uint32_t> s_timeouts{0};
        uint32_t timeout_count = s_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (timeout_count <= 8 || (timeout_count % 64) == 0) {
          REXGPU_WARN(
              "[AC6-PACING] guest swap stalled 100ms waiting for delivery "
              "(#{}; issued={} delivered={})",
              timeout_count, issued, g_present_delivery_at_last_timeout);
        }
      }
    } else if (!delivery_stalled) {
      g_present_delivery_at_last_timeout = UINT64_MAX;
    }
  }
  g_guest_swaps_issued.fetch_add(1, std::memory_order_relaxed);

  // 2) Ceiling: absolute-deadline limiter at target_hz. Deadlines advance by
  // exactly one interval while the game keeps up (drift-free target rate) and
  // re-anchor to now when it does not, so a slow stretch accrues no catch-up
  // debt that would burst frames afterwards.
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / target_hz));
  auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point wake;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (g_present_pacing_deadline < now) {
      g_present_pacing_deadline = now;
    }
    wake = g_present_pacing_deadline;
    g_present_pacing_deadline = wake + interval;
  }
  // Coarse sleep to ~2ms short of the deadline, then spin the remainder for
  // precision (OS sleep granularity is ~1ms and can overshoot).
  while (true) {
    now = std::chrono::steady_clock::now();
    if (now >= wake) {
      break;
    }
    const auto remaining = wake - now;
    if (remaining > std::chrono::milliseconds(2)) {
      rex::thread::Sleep(
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining) -
          std::chrono::milliseconds(2));
    } else {
      std::this_thread::yield();
    }
  }
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!kStoreShaders) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::EndTracing() {
  command_processor_->EndTracing();
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
