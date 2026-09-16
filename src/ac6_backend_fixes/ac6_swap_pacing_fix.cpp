// AC6 swap pacing fix: flip on fence instead of on vblank.
//
// The guest's D3D9 throttles the ring to vsync: after each swap it writes 1 to
// a "swap pending" flag (scratch-register write-back into the system writeback
// block at [device+10900], dword 1), and the next frame's ring section carries
// a WAIT_REG_MEM for that flag to return to 0. The flag is cleared by the
// swap-complete worker 0x821EFBE0, which the vblank ISR (0x821E63F0, source 0)
// runs once per 60Hz tick: if the head swap's fence has been reached it
// programs D1GRPH_PRIMARY_SURFACE_ADDRESS (the flip) and clears the flag.
//
// So a frame can only flip at the vblank AFTER the emulated GPU finishes it,
// and a frame that takes 17ms costs 33. Measured on 2026-09-15: ~13ms of
// command-processor work followed by ~7ms waiting for the tick, 45-55fps as a
// blend of 60 and 30.
//
// The fix runs that same worker from the command processor the moment a
// memory wait would otherwise sleep. The worker checks the fence itself, so
// running it early is safe (nothing happens if the frame is not done), and
// MarkVblank is untouched - the 60Hz ISR the game measures its frame delta
// against keeps ticking exactly as before. The host swapchain has its own
// vsync, so nothing tears.
//
// Raising the guest vblank rate would ALSO release this wait, and did in an
// earlier session - but it corrupts the game's frame delta (see
// ac6_freerun_vblank_hz). This releases the swap without touching the clock.

#include <atomic>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ac6_swap_flip_on_fence, false, "AC6/Enhancements",
                    "Complete the guest's swap as soon as the emulated GPU reaches its fence "
                    "instead of waiting for the next 60Hz vblank tick. Removes the vsync "
                    "quantisation of the command-processor thread (a 17ms frame no longer "
                    "costs 33) without changing the vblank rate the game times itself by.");

namespace ac6::backend {

namespace {

// D3D's swap-complete worker: (device) -> void. Walks the pending-swap queue,
// flips any swap whose fence the GPU has reached, clears the pending flag.
constexpr uint32_t kSwapCompleteWorker = 0x821EFBE0;

std::atomic<uint64_t> g_flips_forced{0};

}  // namespace

void InstallSwapPacingFix(rex::graphics::GraphicsSystem* graphics_system) {
  if (!graphics_system) {
    return;
  }
  graphics_system->SetWaitMemoryHook([graphics_system](uint32_t /*guest_physical_address*/) {
    if (!REXCVAR_GET(ac6_swap_flip_on_fence)) {
      return;
    }
    // Only while the present pacer is the limiter. Outside gameplay (menus,
    // hangar, cutscenes under ac6_cutscene_clamp) the FPS unlock leaves the
    // game frame-locked to the 60Hz vblank, and that vsync IS the pacing there:
    // releasing it with nothing in its place ran the title screen at 2,400fps.
    if (rex::graphics::GraphicsSystem::GetGuestPresentPacing() <= 0.0) {
      return;
    }
    const uint32_t device = graphics_system->interrupt_callback_data();
    if (!device) {
      return;
    }
    graphics_system->DispatchGuestCall(kSwapCompleteWorker, device);
    if (g_flips_forced.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_ERROR("[AC6 swap pacing] flip-on-fence active (worker 0x{:08X}, device 0x{:08X})",
                   kSwapCompleteWorker, device);
    }
  });
}

uint64_t SwapPacingFlipsForced() { return g_flips_forced.load(std::memory_order_relaxed); }

}  // namespace ac6::backend
