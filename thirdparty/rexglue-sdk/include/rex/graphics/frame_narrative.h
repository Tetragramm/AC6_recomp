#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

REXCVAR_DECLARE(double, narrate_frame_every_s);

namespace rex::graphics::narrative {

// The frame narrative: one whole frame, written out in order as the command
// processor executes it - every draw with its targets, the ownership transfers
// it paid for, its shaders and the textures it reads; every resolve with where
// it came from and where it went. Textures that are the destination of an
// earlier resolve are labelled with that resolve, which is how an EDRAM alias
// is followed to the shader that consumes it.
//
// It exists to answer "what does this alias mean" for the native renderer.
// Everything here runs on the command processor thread.

// True while the frame being executed is the one being narrated. Callers use
// it to skip the (cheap) argument gathering when it is not.
bool Active();

// Frame boundary. Decides whether the next frame is narrated.
void OnSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);

// After the render target cache has been updated for the draw, so the bound
// targets and the transfers they needed are known, and before the draw is
// recorded. `bin_mask`/`bin_select` are the predicated-tiling state.
void OnDraw(const RegisterFile& regs, const Shader* vertex_shader, const Shader* pixel_shader,
            xenos::PrimitiveType prim_type, uint32_t index_count, const RenderTargetCache& rtc,
            uint64_t bin_mask, uint64_t bin_select, bool is_clear_quad);

// A resolve (copy and/or clear) as the render target cache sees it. Called
// for every resolve, not only in narrated frames: it remembers where the
// pixels went so a later fetch can be labelled.
void OnResolve(const draw_util::ResolveInfo& info);

}  // namespace rex::graphics::narrative
