/**
 * ReXGlue native audio runtime
 * Part of the AC6 Recompilation project
 */

#include <native/audio/audio_system.h>

#include <native/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute host audio output");
REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio",
                    "Legacy placeholder while the audio stack is stubbed");
REXCVAR_DEFINE_BOOL(audio_trace_telemetry, false, "Audio",
                    "Trace audio runtime telemetry");
REXCVAR_DEFINE_BOOL(audio_trace_render_driver_verbose, false, "Audio",
                    "Trace render-driver activity");
REXCVAR_DEFINE_BOOL(audio_deep_trace, false, "Audio",
                    "Enable verbose runtime audio tracing");
// The audio_xma_* guards below are correctness fixes debugged on Ace
// Combat 6 (details in the commits introducing each). All default on; off
// restores the legacy behaviour for A/B comparison.
REXCVAR_DEFINE_BOOL(audio_xma_header_straddle_fix, true, "AC6/Fixes",
                    "Decode XMA frames whose header straddles a packet boundary. "
                    "Off = legacy walk, which drops a frame per occurrence and "
                    "combs multi-stream cutscene dialogue.");
REXCVAR_DEFINE_BOOL(audio_xma_loop_end_guard, true, "AC6/Fixes",
                    "Apply the degenerate-loop guard to the loop-end frame test "
                    "too, keeping multi-stream voices aligned from their first "
                    "frame. Off = legacy behaviour for A/B.");
REXCVAR_DEFINE_BOOL(audio_xma_preserve_timeline, true, "AC6/Fixes",
                    "Emit silence for XMA frames the decoder rejects instead of "
                    "dropping them, preserving multi-stream timing. Off = legacy "
                    "drop behaviour.");

namespace rex::audio {

namespace {

constexpr uint32_t kAudioSaveVersion = 2;
constexpr uint32_t kRenderDriverTagMask = 0xFFFF0000;
constexpr uint32_t kRenderDriverTagValue = 0x41550000;

}  // namespace

std::unique_ptr<AudioSystem> AudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::unique_ptr<AudioSystem>(new AudioSystem(function_dispatcher));
}

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : function_dispatcher_(function_dispatcher),
      xma_decoder_(std::make_unique<XmaDecoder>(function_dispatcher)) {}

AudioSystem::~AudioSystem() {
  Shutdown();
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  memory_ = kernel_state ? kernel_state->memory() : nullptr;
  if (xma_decoder_) {
    const X_STATUS xma_status = xma_decoder_->Setup(kernel_state);
    if (XFAILED(xma_status)) {
      return xma_status;
    }
  }
  runtime_ = std::make_unique<AudioRuntime>(memory_, function_dispatcher_);
  const X_STATUS runtime_status = runtime_->Setup(kernel_state);
  if (XFAILED(runtime_status) && xma_decoder_) {
    xma_decoder_->Shutdown();
    return runtime_status;
  }
  if (!XFAILED(runtime_status)) {
    REXAPU_INFO(
        "AudioSystem setup complete: runtime=AudioRuntime backend={} trace_telemetry={} "
        "trace_verbose={} mute={}",
        runtime_->backend_name(), REXCVAR_GET(audio_trace_telemetry),
        REXCVAR_GET(audio_trace_render_driver_verbose), REXCVAR_GET(audio_mute));
  }
  return runtime_status;
}

void AudioSystem::Shutdown() {
  if (runtime_) {
    runtime_->Shutdown();
    runtime_.reset();
  }
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::RegisterClient(const uint32_t callback, const uint32_t callback_arg,
                                     size_t* out_index) {
  return runtime_ ? runtime_->RegisterClient(callback, callback_arg, out_index) : X_E_FAIL;
}

void AudioSystem::UnregisterClient(const size_t index) {
  if (runtime_) {
    runtime_->UnregisterClient(index);
  }
}

void AudioSystem::SubmitFrame(const size_t index, const uint32_t samples_ptr) {
  if (runtime_) {
    runtime_->SubmitFrame(index, samples_ptr);
  }
}

void AudioSystem::SubmitSilenceFrame(const size_t index) {
  if (runtime_) {
    runtime_->SubmitSilenceFrame(index);
  }
}

AudioDriverTelemetry AudioSystem::GetClientTelemetry(const size_t index) {
  return runtime_ ? runtime_->GetClientTelemetry(index) : AudioDriverTelemetry{};
}

uint32_t AudioSystem::GetClientRenderDriverTic(const size_t index) {
  return runtime_ ? runtime_->GetClientRenderDriverTic(index) : 0;
}

AudioClientTimingSnapshot AudioSystem::GetClientTimingSnapshot(const size_t index) {
  return runtime_ ? runtime_->GetClientTimingSnapshot(index) : AudioClientTimingSnapshot{};
}

X_STATUS AudioSystem::RegisterRenderDriverClient(const uint32_t callback,
                                                 const uint32_t callback_arg,
                                                 uint32_t* out_driver_handle) {
  if (!out_driver_handle) {
    return X_E_INVALIDARG;
  }

  size_t index = 0;
  const X_STATUS result = RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  *out_driver_handle = kRenderDriverTagValue | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_STATUS_SUCCESS;
}

X_STATUS AudioSystem::UnregisterRenderDriverClient(const uint32_t driver_handle,
                                                   AudioDriverTelemetry* out_telemetry) {
  size_t index = 0;
  if (!ResolveRenderDriverHandle(driver_handle, &index)) {
    return X_E_INVALIDARG;
  }

  if (out_telemetry) {
    *out_telemetry = GetClientTelemetry(index);
  }
  UnregisterClient(index);
  return X_STATUS_SUCCESS;
}

X_STATUS AudioSystem::SubmitRenderDriverFrame(const uint32_t driver_handle,
                                              const uint32_t samples_ptr,
                                              AudioDriverTelemetry* out_telemetry) {
  size_t index = 0;
  if (!ResolveRenderDriverHandle(driver_handle, &index)) {
    return X_E_INVALIDARG;
  }

  if (samples_ptr == 0) {
    SubmitSilenceFrame(index);
  } else {
    SubmitFrame(index, samples_ptr);
  }
  if (out_telemetry) {
    *out_telemetry = GetClientTelemetry(index);
  }
  return X_STATUS_SUCCESS;
}

uint32_t AudioSystem::GetRenderDriverTic(const uint32_t driver_handle,
                                         AudioDriverTelemetry* out_telemetry,
                                         AudioClientTimingSnapshot* out_timing) {
  size_t index = 0;
  if (!ResolveRenderDriverHandle(driver_handle, &index)) {
    if (out_telemetry) {
      *out_telemetry = AudioDriverTelemetry{};
    }
    if (out_timing) {
      *out_timing = AudioClientTimingSnapshot{};
    }
    return 0;
  }

  if (out_telemetry) {
    *out_telemetry = GetClientTelemetry(index);
  }
  if (out_timing) {
    *out_timing = GetClientTimingSnapshot(index);
  }
  return GetClientRenderDriverTic(index);
}

uint32_t AudioSystem::GetUnderrunCount(const uint32_t driver_handle,
                                       AudioDriverTelemetry* out_telemetry) {
  size_t index = 0;
  if (!ResolveRenderDriverHandle(driver_handle, &index)) {
    if (out_telemetry) {
      *out_telemetry = AudioDriverTelemetry{};
    }
    return 0;
  }

  const auto telemetry = GetClientTelemetry(index);
  if (out_telemetry) {
    *out_telemetry = telemetry;
  }
  return telemetry.underrun_count;
}

AudioTelemetrySnapshot AudioSystem::GetTelemetrySnapshot() const {
  return runtime_ ? runtime_->GetTelemetrySnapshot() : AudioTelemetrySnapshot{};
}

const AudioTraceBuffer& AudioSystem::trace_buffer() const {
  static const AudioTraceBuffer empty_trace;
  return runtime_ ? runtime_->trace_buffer() : empty_trace;
}

std::string AudioSystem::GetBackendName() const {
  return runtime_ ? runtime_->backend_name() : "none";
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  if (!stream || !runtime_ || !xma_decoder_) {
    return false;
  }

  stream->Write(kAudioSaveSignature);
  stream->Write(kAudioSaveVersion);
  return runtime_->Save(stream) && xma_decoder_->Save(stream);
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (!stream || !runtime_ || !xma_decoder_) {
    return false;
  }

  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value");
    return false;
  }
  if (stream->Read<uint32_t>() != kAudioSaveVersion) {
    REXAPU_ERROR("AudioSystem::Restore - Unsupported save version");
    return false;
  }

  if (!runtime_->Restore(stream) || !xma_decoder_->Restore(stream)) {
    return false;
  }

  const bool runtime_paused = runtime_->is_paused();
  if (xma_decoder_->is_paused() != runtime_paused) {
    REXAPU_WARN("AudioSystem::Restore - runtime/XMA pause state mismatch, normalizing to {}",
                runtime_paused ? "paused" : "running");
    if (runtime_paused) {
      xma_decoder_->Pause();
    } else {
      xma_decoder_->Resume();
    }
  }
  return true;
}

void AudioSystem::Pause() {
  if (runtime_) {
    runtime_->Pause();
  }
  if (xma_decoder_) {
    xma_decoder_->Pause();
  }
}

void AudioSystem::Resume() {
  if (runtime_) {
    runtime_->Resume();
  }
  if (xma_decoder_) {
    xma_decoder_->Resume();
  }
}

bool AudioSystem::is_paused() const {
  return runtime_ ? runtime_->is_paused() : false;
}

bool AudioSystem::ResolveRenderDriverHandle(const uint32_t driver_handle, size_t* out_index) const {
  if ((driver_handle & kRenderDriverTagMask) != kRenderDriverTagValue || !runtime_) {
    return false;
  }

  const size_t index = driver_handle & 0x0000FFFF;
  if (index >= kMaximumAudioClientCount) {
    return false;
  }

  if (out_index) {
    *out_index = index;
  }
  return true;
}

}  // namespace rex::audio
