#pragma once

#include <cstdint>

struct PPCContext;

namespace ac6::backend {

// Render-target-0 latch. d3d_hooks records the last surface bound through
// D3DDevice_SetRenderTarget slot 0 (the device's own slot can be stale at
// SetViewport time because the game sets the viewport before binding); the
// full-res effects viewport lift reads it to identify the R32_FLOAT
// silhouette downscaler. (Formerly EffectsReconNoteRtBind/EffectsReconLastRt0
// in the removed recon module.)
void NoteRt0Bound(uint32_t index, uint32_t surface);
uint32_t LastRt0Bound();

// Full-res effects (ac6_fullres_effects): rewrite the effects module's
// per-frame 640x360 resolve source rect to 1280x720 in guest memory. Called
// from the D3D Resolve hook (d3d_hooks.cpp) before the original runs; no-op
// unless the caller is the effects render module and the rect matches.
void FullresFixResolveRect(PPCContext& ctx, uint8_t* base);

// Silhouette crop (the shipped mask fix): FxNoteResolveDest (from the guest
// Resolve hook) records the downscaler's resolve dest as the mask base;
// FxCropCompositorMask (at the compositor draw) crops that fetch to 640x360 so
// only the filled upper-left quarter is sampled.
void FxNoteResolveDest(uint32_t dest_obj, uint8_t* base);
void FxCropCompositorMask(uint64_t vs_hash, uint64_t ps_hash, uint32_t* fetch_constants);

// The silhouette downscaler fix: force-restore the 2:1 downscaler draw
// (PS 8EE463763E880F35) to its native 640-wide target so it downscales
// correctly again. Hash-gated (only this one draw). Returns true if fixed.
bool FxFixDownscalerDraw(uint64_t ps_ucode_hash, uint32_t* surface_info,
                         uint32_t* vp_xscale, uint32_t* vp_yscale,
                         uint32_t* vp_xoffset, uint32_t* vp_yoffset);

}  // namespace ac6::backend
