#include "render_hooks.h"
#include "d3d_hooks.h"
#include "ac6_native_graphics.h"

#include <atomic>
#include <chrono>
#include <cmath>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <native/thread.h>
#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(ac6_unlock_fps, true, "AC6",
                    "Master switch for the smooth 60fps unlock (modern present pacing + "
                    "dt-snap + physics dt-correction). On by default; false = stock behaviour.");
REXCVAR_DEFINE_BOOL(ac6_cutscene_clamp, true, "AC6",
                    "Suspend the 60fps unlock during in-engine cutscenes so they "
                    "play at native ~30fps instead of double speed");
REXCVAR_DEFINE_BOOL(ac6_dynamic_vblank, true, "AC6/Enhancements",
                    "Pace menus and cutscenes at the native 60Hz vblank while "
                    "gameplay runs at the target rate.");
REXCVAR_DEFINE_DOUBLE(ac6_fps_target, 60.0, "AC6/Enhancements",
                      "Target framerate under the FPS unlock. 0 = auto: the highest "
                      "rate up to ac6_max_sim_fps that evenly divides your monitor's "
                      "refresh rate.");
REXCVAR_DEFINE_DOUBLE(ac6_max_sim_fps, 60.0, "AC6/Enhancements",
                      "Ceiling on the simulation rate. The game's physics is only "
                      "validated to 60fps; raise only for testing.");
REXCVAR_DEFINE_BOOL(ac6_dt_snap, true, "AC6/Enhancements",
                    "Snap the simulation step to the pacing target when within ~1ms "
                    "of it, removing residual micro-stutter and shake.");
REXCVAR_DEFINE_BOOL(ac6_timing_trace, false, "AC6/Enhancements",
                    "Log the FPS unlock's timing state once a second: measured frame "
                    "time, the frame delta handed to the sim, and the physics step "
                    "ratio. For diagnosing unlock misbehaviour.");
REXCVAR_DEFINE_BOOL(ac6_present_pacing, true, "AC6/Enhancements",
                    "Gate the guest's swap on real GPU delivery of the previous frame "
                    "plus a target-rate limiter. false = the FPS unlock's other parts "
                    "still apply but the guest swap is never blocked.");
REXCVAR_DEFINE_DOUBLE(ac6_freerun_vblank_hz, 0.0, "AC6/Enhancements",
                      "Guest VBlank rate while gameplay free-runs under the FPS unlock. "
                      "0 = leave the guest VBlank at its native rate (the only value "
                      "known to be correct - see the note in ac6PresentTimingHook). "
                      "Raising it above the frame rate CORRUPTS the game's own frame "
                      "delta and makes the simulation crawl.");
REXCVAR_DEFINE_DOUBLE(ac6_min_sim_fps, 20.0, "AC6/Enhancements",
                      "Lowest framerate the game runs at true speed before slow "
                      "motion kicks in. Stock behaviour is 30.");

using Clock = std::chrono::steady_clock;

// Current monitor refresh rate in Hz, 0 if unknown. Cached on first use (a
// mid-session refresh change is rare enough to need a restart).
static double HostRefreshHz() {
  static double cached = -1.0;
  if (cached >= 0.0) {
    return cached;
  }
  cached = 0.0;
#if defined(_WIN32)
  DEVMODEW dm;
  ZeroMemory(&dm, sizeof(dm));
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
    cached = double(dm.dmDisplayFrequency);
  }
#endif
  return cached;
}

// Single source of truth for the sim/presentation rate under the unlock, so the
// pacing limiter and the dt-snap always agree. See ac6_fps_target / ac6_max_sim_fps.
static double ResolvePacingTargetFps() {
  // Absolute floor (10) lets sub-30 rates be targeted for testing; below
  // ac6_min_sim_fps the sim goes slow-mo (expected), but the rate is honored.
  constexpr double kMinTargetFps = 10.0;
  double ceil_fps = REXCVAR_GET(ac6_max_sim_fps);
  if (ceil_fps < kMinTargetFps) ceil_fps = kMinTargetFps;
  if (ceil_fps > 240.0) ceil_fps = 240.0;

  double target = REXCVAR_GET(ac6_fps_target);
  if (target > 0.0) {
    if (target < kMinTargetFps) target = kMinTargetFps;
    if (target > ceil_fps) target = ceil_fps;
    return target;
  }

  // Auto: largest integer in [kMinTargetFps, ceil] that evenly divides the
  // refresh rate, so frames align to refresh boundaries instead of beating.
  const double refresh = HostRefreshHz();
  if (refresh < kMinTargetFps) {
    return ceil_fps;  // unknown refresh: just run at the ceiling
  }
  const int hz = int(refresh + 0.5);
  for (int f = int(ceil_fps + 0.5); f >= int(kMinTargetFps); --f) {
    if (hz % f == 0) {
      return double(f);
    }
  }
  // No clean divisor >= 30 (e.g. 50/75Hz): fall back to min(refresh, ceiling).
  return refresh < ceil_fps ? refresh : ceil_fps;
}

namespace {

std::atomic<double> g_frame_time_ms{0.0};
std::atomic<double> g_fps{0.0};
std::atomic<uint64_t> g_frame_count{0};
Clock::time_point g_frame_start{};

// Wall-clock (steady) ms of the last in-engine cutscene tick. The cutscene
// hook (ac6CinematicTickHook) runs on the game thread; the timing hooks read
// this on the present/GPU path, hence the atomic. INT64_MIN = never ticked.
std::atomic<int64_t> g_last_cinematic_tick_ms{INT64_MIN};

// The demo-manager Exec runs every game frame while a cutscene plays (~16-33ms
// apart). A few-frame decay keeps the clamp asserted across the cutscene and
// auto-releases shortly after it ends.
constexpr int64_t kCinematicDecayMs = 100;

// Wall-clock ms of the last draw using the world/effects compositor pixel
// shader (stamped by the GPU command processor via NotifyWorldCompositorDraw).
// The compositor runs every frame the 3D world renders and never in the 2D
// front-end, so its freshness distinguishes free-runnable gameplay from
// frame-locked menus/hangar. (The delta-time hook was tried first as this
// signal, but it lives in the frame layer and ticks in menus too.)
// INT64_MIN = never drawn.
std::atomic<int64_t> g_last_world_draw_ms{INT64_MIN};
constexpr int64_t kWorldDrawDecayMs = 300;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

bool IsWorldRenderActive() {
  const int64_t last = g_last_world_draw_ms.load(std::memory_order_relaxed);
  if (last == INT64_MIN) {
    return false;
  }
  return (NowMs() - last) <= kWorldDrawDecayMs;
}

bool AreTimingHooksActive() {
    // The world-render gate only applies under dynamic vblank pacing: it exists
    // to keep menus/hangar at native cadence, and it fails closed (a game
    // build/render path whose compositor shader hashes differently would never
    // stamp it). ac6_dynamic_vblank=false restores the plain always-on unlock.
    return REXCVAR_GET(ac6_unlock_fps) &&
           !ac6::IsCinematicActive() &&
           (!REXCVAR_GET(ac6_dynamic_vblank) || IsWorldRenderActive());
}

}  // namespace

namespace {
// Last values the sim actually received, for ac6_timing_trace.
std::atomic<double> g_trace_exact{0.0};
std::atomic<double> g_trace_delta{0.0};
std::atomic<bool> g_trace_snapped{false};
std::atomic<bool> g_trace_delta_seen{false};
}  // namespace

namespace ac6 {

void NoteSimDelta(double exact, double delta, bool snapped) {
  g_trace_exact.store(exact, std::memory_order_relaxed);
  g_trace_delta.store(delta, std::memory_order_relaxed);
  g_trace_snapped.store(snapped, std::memory_order_relaxed);
  g_trace_delta_seen.store(true, std::memory_order_relaxed);
}

// True while the FPS-unlock timing hooks are remapping the game's cadence
// (unlock cvars on and no cutscene clamp). The physics force-step rescale keys
// off this so it stays in lockstep with the frame-delta hooks: whenever the
// delta reverts to vanilla, the force step must too.
bool TimingHooksActive() {
  return AreTimingHooksActive();
}

double ClampedMinSimFps() {
  double min_fps = REXCVAR_GET(ac6_min_sim_fps);
  if (min_fps < 10.0) {
    min_fps = 10.0;
  } else if (min_fps > 30.0) {
    min_fps = 30.0;  // can't floor above 30fps - that would slow 30fps content
  }
  return min_fps;
}

bool WorldRenderActiveRecently() {
  return IsWorldRenderActive();
}

bool IsCinematicActive() {
    if (!REXCVAR_GET(ac6_cutscene_clamp)) {
        return false;
    }
    const int64_t last = g_last_cinematic_tick_ms.load(std::memory_order_relaxed);
    if (last == INT64_MIN) {
        return false;
    }
    return (NowMs() - last) <= kCinematicDecayMs;
}

}  // namespace ac6

bool ac6FlipIntervalHook() {
    return AreTimingHooksActive();
}

bool ac6PresentIntervalHook(PPCRegister& r10) {
    if (!AreTimingHooksActive()) {
        return false;
    }
    r10.u64 = 1;
    return true;
}

void ac6DeltaDivisorHook(PPCRegister& r29) {
    if (!AreTimingHooksActive()) {
        return;
    }
    r29.u64 = 30;
}

// Fires right after the game turns the measured frame time into its integer
// delta: r8 = min(floor(elapsed*100 / (freq/divisor)) + 1, 100). Under the
// unlock the floor plus the +1 guard bias the delta high by (1 - frac) every
// frame - a systematic speed-up at unlocked framerates (+2% @60fps, +10% @300).
// Carry the fractional remainder across frames so the SUM of integer deltas
// tracks real time exactly. Only active while the divisor remap is (r29 == 30);
// otherwise the vanilla value passes through and the remainder resets so no
// stale correction leaks across mode changes.
void ac6DeltaPrecisionHook(PPCRegister& r8, PPCRegister& r10, PPCRegister& r29, PPCRegister& r30) {
  static double s_remainder = 0.0;

  // Cheapest gates first: the register compares are free, the cvar/clock work
  // only runs on the frames that can actually take the rewrite path.
  if (r29.u32 != 30 || r10.u32 == 0 || !AreTimingHooksActive()) {
    s_remainder = 0.0;
    return;
  }
  const bool precision = true;  // always on now (frame-delta correctness)
  const double min_fps = ac6::ClampedMinSimFps();
  const bool raise_floor = min_fps < 30.0;
  if (!precision && !raise_floor) {
    s_remainder = 0.0;
    return;
  }
  // The delta clamp becomes 3000/min_fps instead of the stock 100 (30fps->100,
  // 20fps->150): the sim keeps true speed down to min_fps before slow motion.
  const double max_delta = 3000.0 / min_fps;

  // Exact delta this frame in the game's own scale (elapsed * 3000), using the
  // same ticks-per-frame divisor (r10 = freq / 30) the game divided by. Clamp
  // to the floor: time slower than min_fps is dropped, not banked.
  double exact = double(r30.u32) * 100.0 / double(r10.u32);
  if (exact > max_delta) {
    exact = max_delta;
  }

  // dt-snap: at a steady framerate near the pacing target, force the delta to the
  // EXACT target value so the fixed-step integrator sees a constant step. This is
  // what removes the residual per-frame jitter (the aircraft shake) that survives
  // smooth pacing - a fixed-step sim amplifies even sub-millisecond dt variance.
  // Only engages within a tight window of the target; genuine slowdowns/speedups
  // fall through to the precision path below. Mirrors PA's native PC engine, which
  // feeds a constant dt at a steady rate. The target matches the pacing limiter
  // exactly (shared ResolvePacingTargetFps), so sim and presentation stay locked.
  if (REXCVAR_GET(ac6_dt_snap)) {
    const double target_fps = ResolvePacingTargetFps();
    const double target_delta = 3000.0 / target_fps;                  // game units, 100 = 30fps
    const double tol_units = 3.0;  // ~1ms window (100 delta-units = 33.3ms, so 1ms = 3)
    if (tol_units > 0.0 && target_delta <= max_delta &&
        std::fabs(exact - target_delta) < tol_units) {
      double snapped = std::floor(target_delta + 0.5);
      if (snapped < 1.0) {
        snapped = 1.0;
      }
      s_remainder = 0.0;  // locked to target; no fractional carry
      r8.u64 = uint64_t(snapped);
      ac6::NoteSimDelta(exact, snapped, true);
      return;
    }
  }

  // precision on: carry the remainder (no +1). precision off: keep the game's
  // floor(exact)+1 integer. floor(exact + 1) == floor(exact) + 1.
  const double base = precision ? (s_remainder + exact) : (exact + 1.0);
  double delta = std::floor(base);
  if (delta < 1.0) {
    delta = 1.0;  // the game guarantees progress every frame; on the precision
                  // path the overshoot is repaid through a negative remainder
  } else if (delta > max_delta) {
    delta = max_delta;
  }
  s_remainder = precision ? (base - delta) : 0.0;
  r8.u64 = uint64_t(delta);
  ac6::NoteSimDelta(exact, delta, false);
}

void ac6PresentTimingHook(PPCRegister& /*r31*/) {
    // ac6::d3d::OnFrameBoundary(); // MOVED TO GPU THREAD

  // Dynamic vblank pacing: free-run only while the 3D world is rendering; pace
  // frame-locked content (menus, hangar, cutscenes) at the native 60Hz. Only
  // engages when the FPS unlock is on, so default configurations keep the plain
  // cvar-driven vblank behavior.
  const bool unlock = REXCVAR_GET(ac6_unlock_fps);
  const bool dynamic_pacing = REXCVAR_GET(ac6_dynamic_vblank) && unlock;
  // Single source of truth for "the unlock is remapping the cadence right now" -
  // the same signal that gates the interval/delta hooks and the physics rescale.
  const bool free_running = dynamic_pacing && AreTimingHooksActive();

  // Frame pacing for this frame. Precedence:
  //   1. frame-locked content (menus, cutscenes) -> fixed 60Hz vblank override;
  //   2. gameplay with ac6_vblank_auto -> modern present pacing: the guest
  //      vblank free-runs (gates nothing) and the game's swap thread instead
  //      blocks on real GPU delivery plus an absolute-deadline limiter at
  //      ac6_fps_target. The game runs at min(target, real GPU throughput)
  //      with uniform frame times - no vblank grid, so a sub-target GPU can
  //      never alias into the 60/30 sub-harmonic staircase;
  //   3. otherwise the plain cvar-driven vblank (previous behavior).
  double override_hz = 0.0;
  double pacing_target_hz = 0.0;
  if (dynamic_pacing && !free_running) {
    override_hz = 60.0;
  } else if (free_running) {
    // DO NOT raise the guest VBlank rate here. The original code forced 1000Hz
    // to make the VBlank non-gating so PaceGuestPresent would be the sole gate.
    // That is incompatible with this game: AC6 measures its frame delta BETWEEN
    // VBLANK INTERRUPTS, not between frames, so a VBlank faster than the frame
    // rate makes every measured delta a fraction of the real one.
    //
    // Measured 2026-08-31 with ac6_timing_trace at a steady 60fps (correct
    // delta = 50, where 100 = 30fps): at the 240Hz override the delta the sim
    // received peaked at 12.55 = 50 * 60/240 and jittered down to 1.85 as the
    // frame/VBlank phase drifted. The sim therefore advanced at roughly a sixth
    // of real time - the aircraft barely responds and the ramped input command
    // reads as "stuck on". At the original 1000Hz the cap is 3 instead of 12.5,
    // i.e. the same bug, worse.
    //
    // So the VBlank stays at its native rate and caps the game at the refresh
    // rate. The rest of the unlock (flip interval, delta precision, dt-snap,
    // physics rescale) still applies. ac6_freerun_vblank_hz exists to
    // re-introduce an override for experiments; anything above the frame rate
    // will corrupt the delta.
    pacing_target_hz = REXCVAR_GET(ac6_present_pacing) ? ResolvePacingTargetFps() : 0.0;
    override_hz = REXCVAR_GET(ac6_freerun_vblank_hz);
  }
  rex::graphics::GraphicsSystem::SetGuestVblankHzOverride(override_hz);
  rex::graphics::GraphicsSystem::SetGuestPresentPacing(pacing_target_hz);

    const auto now = Clock::now();
    if (g_frame_start.time_since_epoch().count() != 0) {
        const double frame_time_ms =
            std::chrono::duration<double, std::milli>(now - g_frame_start).count();
        g_frame_time_ms.store(frame_time_ms, std::memory_order_relaxed);
        g_fps.store(frame_time_ms > 0.0001 ? (1000.0 / frame_time_ms) : 0.0,
                    std::memory_order_relaxed);
        g_frame_count.fetch_add(1, std::memory_order_relaxed);
    }
    g_frame_start = now;

  if (REXCVAR_GET(ac6_timing_trace)) {
    static int64_t s_last_trace_ms = 0;
    const int64_t now_ms = NowMs();
    if (now_ms - s_last_trace_ms >= 1000) {
      s_last_trace_ms = now_ms;
      const double ft = g_frame_time_ms.load(std::memory_order_relaxed);
      REXLOG_INFO(
          "[AC6-TIMING] frame {:.2f}ms ({:.1f}fps) hooks={} world={} cine={} "
          "vblank={:.0f}Hz pacing={:.0f} | sim delta: exact={:.2f} used={:.0f} "
          "snapped={} seen={} | physics step ratio={:.4f}",
          ft, ft > 0.0001 ? 1000.0 / ft : 0.0, AreTimingHooksActive() ? 1 : 0,
          IsWorldRenderActive() ? 1 : 0, ac6::IsCinematicActive() ? 1 : 0, override_hz,
          pacing_target_hz, g_trace_exact.load(std::memory_order_relaxed),
          g_trace_delta.load(std::memory_order_relaxed),
          g_trace_snapped.load(std::memory_order_relaxed) ? 1 : 0,
          g_trace_delta_seen.exchange(false, std::memory_order_relaxed) ? 1 : 0,
          ac6::LastPhysicsStepRatio());
    }
  }

  // Log the first handful of pacing transitions (smooth pacing keyed negative
  // so mode switches log distinctly from plain overrides).
  const double log_key = pacing_target_hz > 0.0 ? -pacing_target_hz : override_hz;
  static double last_log_key = -0.5;
  static uint32_t transition_logs = 0;
  if (unlock && log_key != last_log_key && transition_logs < 32) {
    ++transition_logs;
    if (pacing_target_hz > 0.0) {
      REXLOG_INFO("[AC6-VBLANK] pacing -> modern (present-paced, target {:.0f}fps)",
                  pacing_target_hz);
    } else if (override_hz == 0.0) {
      REXLOG_INFO("[AC6-VBLANK] pacing -> free-run (uncapped)");
    } else {
      REXLOG_INFO("[AC6-VBLANK] pacing -> {:.0f}Hz guest vblank", override_hz);
    }
  }
  last_log_key = log_key;
}

void ac6CinematicTickHook(PPCRegister& /*r3*/) {
    // A demo-manager Exec (DD: sub_82184460 / EM: sub_821856F8) ran this frame ->
    // an in-engine cutscene is playing. Stamp the time so AreTimingHooksActive()
    // suspends the 60fps unlock until the cutscene ends (the stamp goes stale a
    // few frames after the last tick), letting the frame-locked cinematic
    // Sequencer play at native ~30fps instead of double speed.
    g_last_cinematic_tick_ms.store(NowMs(), std::memory_order_relaxed);
}

namespace ac6 {

FrameStats GetFrameStats() {
    return FrameStats{g_frame_time_ms.load(std::memory_order_relaxed),
                      g_fps.load(std::memory_order_relaxed),
                      g_frame_count.load(std::memory_order_relaxed)};
}

void NotifyWorldCompositorDraw() {
  g_last_world_draw_ms.store(NowMs(), std::memory_order_relaxed);
}

}  // namespace ac6
