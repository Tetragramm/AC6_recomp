// Cutscene A/V resync (audio-master catch-up).
//
// The mechanism (confirmed in code and on real hardware): guest audio time =
// XAudioGetRenderDriverTic = host samples consumed INCLUDING injected
// underrun silence, so it tracks wall clock through any render hitch; the
// audio worker is its own thread, so a render stall does not even pause real
// audio. The in-engine cutscene sequencers (CAce6DemoManager::Exec "DD"
// 0x82184460 / CX360DemoManagerEM::Exec "EM" 0x821856F8) tick their timeline
// once per rendered frame. One long frame puts the video permanently behind
// the audio; sustained sub-30fps render turns the whole cutscene into slow
// motion against its soundtrack.
//
// This file wraps both Exec functions (weak symbols in the generated code,
// same override pattern as ac6_fps_physics_fix.cpp). Audio is the master
// clock and is NEVER touched - the cutscene catches up instead. Per rendered
// frame, deficit = ticks the audio clock says should have run minus ticks
// issued; while the deficit is >= 2 ticks, extra Exec calls run (capped per
// frame by ac6_cutscene_resync_max_ticks) - a bounded timeline frame-skip,
// the same audio-master pattern film players use. The >= 2 engage threshold
// keeps normal 30fps playback provably untouched (steady-state phase jitter
// is +/-1 tick and can never trigger it). The cap is also the slowest
// sustained render rate that stays synced: N extras per frame holds sync
// down to 30/(1+N) fps. 0 = uncapped (one hard jump-cut after a stall).
//
// Both Exec wrappers run on the guest game thread (the sole caller), so
// plain statics are safe throughout.

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <native/audio/audio_client.h>
#include <native/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(ac6_cutscene_resync, true, "AC6",
                    "Keep in-engine cutscene video locked to its audio. Audio "
                    "is the master clock and is never altered; after a render "
                    "hitch (or under sustained slow rendering) the cutscene "
                    "timeline catches up by running extra sequencer ticks - a "
                    "bounded frame-skip, like every film player's audio-master "
                    "sync. No effect on normal full-speed playback (catch-up "
                    "engages only past 2 ticks of drift; verified extras=0 in "
                    "steady state, injected-hitch recovery in 3 frames, and "
                    "sustained 14fps @ 5x draw scale staying locked).");
REXCVAR_DEFINE_INT32(ac6_cutscene_resync_max_ticks, 3, "AC6",
                     "Max extra cutscene sequencer ticks per rendered frame "
                     "while catching up (ac6_cutscene_resync). Also sets the "
                     "slowest sustained render rate that stays in sync: N "
                     "extras holds sync down to 30/(1+N) fps (3 -> 7.5 fps). "
                     "Low = gentle brief fast-forward after a stall; 0 = "
                     "uncapped, one hard jump-cut.");

PPC_EXTERN_FUNC(__imp__rex_sub_82184460);  // CAce6DemoManager::Exec ("DD")
PPC_EXTERN_FUNC(__imp__rex_sub_821856F8);  // CX360DemoManagerEM::Exec ("EM")

namespace {

using Clock = std::chrono::steady_clock;

// Demo sequencers advance one timeline frame per Exec at the game's native
// 30fps cadence (cutscenes stay clamped to 30 under the FPS unlock).
constexpr uint64_t kSamplesPerDemoTick = rex::audio::kAudioFrameSampleRate / 30;  // 1600

// An Exec gap this long means the demo session ended (menus/gameplay between
// cutscenes). Generous enough that a real mid-cutscene stall keeps its
// session - that stall is exactly what the resync must recover from.
constexpr int64_t kSessionResetMs = 1500;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// Reads the single audio client's consumed-samples clock - the exact value
// the guest sees through XAudioGetRenderDriverTic (48kHz sample units,
// advancing in real time through hitches because underrun silence counts as
// consumed). Returns false if the audio system/client is unavailable.
bool ReadAudioClockSamples(PPCContext& ctx, uint64_t* out_samples) {
  if (!ctx.kernel_state) {
    return false;
  }
  auto* native_audio = ctx.kernel_state->native_audio_system();
  if (!native_audio) {
    return false;
  }
  const rex::audio::AudioClientTimingSnapshot timing =
      native_audio->GetClientTimingSnapshot(0);
  if (timing.consumed_samples == 0) {
    // No audio consumed yet (startup) - no usable master clock this frame.
    return false;
  }
  *out_samples = timing.consumed_samples;
  return true;
}

struct DemoSession {
  const char* site = "";
  int64_t last_exec_ms = INT64_MIN;
  bool clock_valid = false;
  uint64_t baseline_samples = 0;
  uint64_t ticks_issued = 0;
};

DemoSession g_session;

void ExecWithResync(PPCContext& ctx, uint8_t* base,
                    void (*original)(PPCContext&, uint8_t*), const char* site) {
  const int64_t now_ms = NowMs();

  const bool new_session = g_session.site != site ||
                           g_session.last_exec_ms == INT64_MIN ||
                           (now_ms - g_session.last_exec_ms) > kSessionResetMs;
  if (new_session) {
    g_session = DemoSession{};
    g_session.site = site;
  }
  g_session.last_exec_ms = now_ms;

  uint64_t audio_samples = 0;
  const bool clock_ok = ReadAudioClockSamples(ctx, &audio_samples);
  if (clock_ok && !g_session.clock_valid) {
    // First usable clock reading of this session: the tick issued this very
    // frame corresponds to "now" on the audio timeline.
    g_session.clock_valid = true;
    g_session.baseline_samples = audio_samples;
    g_session.ticks_issued = 0;
  }

  const bool resync = REXCVAR_GET(ac6_cutscene_resync) &&
                      g_session.clock_valid && clock_ok;
  // The original's input registers, for re-issuing the call. The extra ticks
  // must not see the first call's clobbered volatile registers.
  PPCContext saved_ctx;
  if (resync) {
    saved_ctx = ctx;
  }

  // The frame's own tick.
  original(ctx, base);
  ++g_session.ticks_issued;

  int64_t deficit_ticks = 0;
  if (g_session.clock_valid && clock_ok) {
    const uint64_t expected =
        (audio_samples - g_session.baseline_samples) / kSamplesPerDemoTick;
    deficit_ticks = static_cast<int64_t>(expected) -
                    static_cast<int64_t>(g_session.ticks_issued);
  }

  // Catch-up: engage past 2 ticks of drift (steady-state jitter is +/-1 and
  // must never trigger), then tick the timeline down to zero deficit, capped
  // per frame. The deficit is recomputed from absolute clocks every frame,
  // so any remainder past the cap carries automatically.
  if (resync && deficit_ticks >= 2) {
    const int32_t cap = REXCVAR_GET(ac6_cutscene_resync_max_ticks);
    int64_t extras = deficit_ticks;
    if (cap > 0) {
      extras = std::min<int64_t>(extras, cap);
    }
    for (int64_t i = 0; i < extras; ++i) {
      ctx = saved_ctx;
      original(ctx, base);
      ++g_session.ticks_issued;
    }
    REXLOG_DEBUG("[AC6-CUTSYNC] catch-up: site={} ran {} extra ticks (deficit was {}, cap {})",
                 site, extras, deficit_ticks, cap);
  }
}

}  // namespace

// CAce6DemoManager::Exec ("DD") - in-engine cutscene sequencer tick.
PPC_FUNC_IMPL(rex_sub_82184460) {
  PPC_FUNC_PROLOGUE();
  ExecWithResync(ctx, base, __imp__rex_sub_82184460, "DD");
}

// CX360DemoManagerEM::Exec ("EM") - in-engine cutscene sequencer tick.
PPC_FUNC_IMPL(rex_sub_821856F8) {
  PPC_FUNC_PROLOGUE();
  ExecWithResync(ctx, base, __imp__rex_sub_821856F8, "EM");
}
