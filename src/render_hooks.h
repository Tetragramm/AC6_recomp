#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/types.h>

REXCVAR_DECLARE(bool, ac6_unlock_fps);
REXCVAR_DECLARE(bool, ac6_cutscene_clamp);

namespace ac6 {

struct FrameStats {
    double frame_time_ms{0.0};
    double fps{0.0};
    uint64_t frame_count{0};
};

FrameStats GetFrameStats();

// True while the FPS-unlock timing hooks are remapping the game's cadence
// (unlock cvars on and no cutscene clamp). The physics force-step rescale keys
// off this so it stays in lockstep with the frame-delta hooks.
bool TimingHooksActive();

// ac6_min_sim_fps clamped to its supported range [10, 30]. Single source for
// the low-fps floor shared by the frame-delta clamp (ac6DeltaPrecisionHook)
// and the physics step cap (StepRatio) - the two must saturate at the same
// framerate or the dynamics desync from the kinematics below 30fps.
double ClampedMinSimFps();

// True while an in-engine cutscene (NU::FW::IngameCinematics, driven by
// CAce6DemoManager::Exec) has ticked within the last decay window. Used by the
// timing hooks to suspend the 60fps unlock so cutscenes play at native cadence.
bool IsCinematicActive();

// Called by the GPU command processor whenever a draw uses the world/effects
// compositor pixel shader (guest ucode 17e5e4ac3e713245). The compositor runs
// once per frame whenever the 3D world is being rendered and never in the 2D
// front-end (menus, hangar), so its freshness is the "gameplay world active"
// signal for the dynamic FPS pacing.
void NotifyWorldCompositorDraw();

// ac6_timing_trace plumbing: the delta hook reports what it handed the sim, and
// the physics fix reports the step ratio it last used, so one log line can show
// whether the unlock's remapping is actually correct.
void NoteSimDelta(double exact, double delta, bool snapped);
double LastPhysicsStepRatio();

// True while a world-compositor draw happened within the last decay window
// (i.e. the 3D world is being rendered - gameplay or in-engine cutscene, not
// the 2D front-end). Same signal the dynamic FPS pacing uses.
bool WorldRenderActiveRecently();

}  // namespace ac6

bool ac6FlipIntervalHook();
bool ac6PresentIntervalHook(PPCRegister& r10);
void ac6DeltaDivisorHook(PPCRegister& r29);
void ac6DeltaPrecisionHook(PPCRegister& r8, PPCRegister& r10, PPCRegister& r29, PPCRegister& r30);
void ac6PresentTimingHook(PPCRegister& r31);

// Fires once per frame from the demo-manager Exec while a cutscene is playing.
// r3 = the demo-manager `this`; unused (presence-of-call is the signal).
void ac6CinematicTickHook(PPCRegister& r3);
