#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace ac6 {

// AC6 enhancement: arbitrary aspect ratio (ultrawide), in-mission only.
// Called once from the D3D12 command processor's SetupContext; applies the
// one-switch config preset (this runs after the toml is loaded, unlike app
// create) and spawns a background thread that, while the ac6_widescreen cvar
// is enabled, polls the game's mode-task state machine and aims the cameras
// accordingly: inside a mission (mode task CModeTaskGame - gameplay,
// in-engine cutscenes and pause) the stored 16:9 aspect (the static default
// in the XEX image plus live camera objects found by signature scan) is
// patched to ac6_widescreen_aspect so the game builds its own projection
// wider; outside a mission everything is reverted to 16:9 so the front end
// (menus, hangar, briefing, attract) renders vanilla. In-engine cinematics
// outside the mission task can opt in via ac6_widescreen_cinematics. See
// ac6_widescreen.cpp for the discovered memory layout and the mode-task
// chain.
void WidescreenInit(rex::memory::Memory* memory);

// The 2D/UI half of the ultrawide feature. Called per draw from the D3D12
// command processor's IssueDraw with the vertex-shader float constant block
// (256 vec4s, host-endian, mutable) and the vertex shader's guest ucode hash
// (the game-placed target-marker shader is left full-width so markers stay
// on target). Detects screen-space 2D transforms among the constants and
// scales their X output row around screen center by 16:9 / target,
// pre-squeezing 2D rendering so the presenter's fill-window stretch cancels
// out. Applies only in wide scenes, where the fill presentation is active
// (world rendering, and in-mission non-world draws like the pause menu).
// Returns true if constants were modified - the caller must then invalidate
// the vertex float constant buffer binding. No-op (atomic load + compare)
// unless ac6_widescreen is on.
// sub_viewport: the draw uses a sub-screen guest viewport (radar window,
// PiP inset) - placed by the viewport, so its constants are never shrunk
// (the viewport rect is shrunk instead; see WidescreenViewportShrinkX).
bool WidescreenPatchUiOrtho(uint32_t* vs_float_constants, uint64_t vs_ucode_hash,
                            bool sub_viewport);

// Called by the D3D12 texture cache whenever the frontbuffer texture is
// requested for a swap. gpu_composed = the frontbuffer range holds GPU-written
// pages (shared-memory tracking); classification_valid = the caller actually
// computed it. Drives the mode-classified presentation: in-mission GPU frames
// fill the window, everything else - the whole front end, and CPU-written
// frames (FMV, loading images) even in-mission - presents letterboxed at
// 16:9, i.e. vanilla.
void WidescreenNotifySwapSource(bool gpu_composed, bool classification_valid);

// Whether the current draw is a target-marker draw whose quads should be
// narrowed by WidescreenShrinkMarkerQuads (marker shader, wide scene, shrink
// active). Cheap: an atomic load plus a hash compare.
bool WidescreenWantsMarkerQuadFix(uint64_t vs_ucode_hash);

// Marker-quad geometry fix. The game bakes marker box corners as screen-space
// positions computed through the WIDENED camera, so the positions are already
// right for the full-width display but the box art inherits the presenter's
// horizontal stretch (~1.34x wide at 21.5:9). Narrowing each quad's X extent
// about its OWN centre - not screen centre - cancels that stretch for the art
// while leaving the centre, i.e. the aim point, exactly where the game put it.
// A per-swap bitmap makes it idempotent when the same geometry is drawn more
// than once in a frame.
// vertices: host pointer to the guest vertex data; stride/pos_offset in bytes
// (position = two big-endian floats). indices: host pointer to the guest index
// buffer, or null for sequential vertices. Quads are 4 consecutive indices.
void WidescreenShrinkMarkerQuads(uint8_t* vertices, uint32_t vertex_stride,
                                 uint32_t pos_offset, const uint8_t* indices,
                                 bool indices_32bit, uint32_t count, uint32_t arena_base);

// The X shrink to apply to SUB-VIEWPORT rects (radar window, PiP inset) for
// the current scene, or 1.0. Sub-viewport elements are placed by their
// viewport, not their constants - the viewport rect must move with the
// uniformly shrunk full-screen UI (scaled around the render target center)
// while their constants stay untouched. World scenes with the in-world
// shrink only.
float WidescreenViewportShrinkX();

}  // namespace ac6
