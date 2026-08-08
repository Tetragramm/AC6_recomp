/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, false, "GPU",
                    "Allow invalid fetch constants");
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
REXCVAR_DEFINE_BOOL(depth_transfer_not_equal_test, true, "GPU",
                    "Use not-equal test for depth transfer");
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
REXCVAR_DEFINE_BOOL(use_fuzzy_alpha_epsilon, true, "GPU",
                    "Use approximate compare for alpha test values to prevent "
                    "flickering on NVIDIA graphics cards");
REXCVAR_DEFINE_BOOL(vfetch_index_rounding_bias, false, "GPU/Shader",
                    "Apply small epsilon bias to vertex fetch indices before "
                    "flooring to fix black triangles caused by RCP precision");
REXCVAR_DEFINE_BOOL(ac6_terrain_hd, true, "AC6/Enhancements",
                    "Draw the terrain at the full shipped resolution (2x what "
                    "the game draws), also removes the terrain cracks.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(draw_resolution_scaled_texture_offsets, true, "GPU/Shader",
                    "Scale texture offsets with draw resolution");
// The param_gen pair and the ac6_* hash lists below are consumed at shader
// translation. Translated shaders persist in the shader storage, but storage
// stores ucode and re-translates at load, so a change applies at the next
// launch: kRequiresRestart.
REXCVAR_DEFINE_BOOL(param_gen_integer_guest_position, false, "AC6/Fixes",
                    "At above-1x draw resolution scale, floor the PsParamGen pixel "
                    "position to the integer guest-pixel index. Fixes mosaic in "
                    "passes doing their own integer pixel-address math.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_BOOL(param_gen_host_subpixel_restore, false, "AC6/Fixes",
                    "With param_gen_integer_guest_position: re-add the host "
                    "sub-pixel offset so restore passes keep full scaled detail.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_neutralize_deswizzle_hashes, "", "AC6/Fixes",
                    "Pixel-shader token list \"<hash>[:<slot>[+<slot>...]]\": replace "
                    "matching fetches' manual EDRAM de-swizzle with the identity "
                    "host-texel UV. Normally driven by ac6_fix_deswizzle.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_snap_guest_texel_hashes, "", "AC6/Fixes",
                    "Pixel-shader token list \"<hash>[:<slot>[+<slot>...]]\": snap "
                    "matching fetches to guest texel centers at above-1x scale. "
                    "Normally driven by ac6_fix_dof.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_densify_x_fetch_hashes, "", "AC6/Fixes",
                    "Token list: densify matching horizontal separable-filter "
                    "fetches at above-1x scale so ghost copies merge. Normally "
                    "driven by ac6_fix_dof.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_densify_y_fetch_hashes, "", "AC6/Fixes",
                    "As ac6_densify_x_fetch_hashes, for vertical-axis passes. "
                    "Normally driven by ac6_fix_dof.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_fix_hoisted_fetch_gradients_hashes, "", "AC6/Fixes",
                    "Token list \"<hash>:<interpolator index>\": compute that "
                    "interpolator's screen-space gradients once in the pixel "
                    "shader prologue, where the quad is uniform, and use them for "
                    "any computed-LOD texture fetch in that shader whose "
                    "coordinates come from the matching guest register.\n"
                    "The guest predicates with \"(p0) exec\" and branches with "
                    "\"jmp\", which on Xenos still let every pixel of the quad "
                    "run - results are masked - so derivatives stay well defined. "
                    "This translator turns both into real control flow, and DXBC "
                    "derivatives inside non-uniform control flow are UNDEFINED: a "
                    "quad pixel that did not enter the block holds a stale "
                    "coordinate, so the gradient collapses, the LOD goes to "
                    "negative infinity and the fetch clamps to the finest mip. "
                    "Measured directly in AC6's ocean - the gradient the fetch "
                    "receives reads zero along a mesh seam while the same "
                    "quantity measured in the prologue is smooth.\n"
                    "This restores hardware behaviour, so it is a fix rather than "
                    "an enhancement. Which shaders it applies to stays a per-shader "
                    "list rather than a blanket default, because the hazard is "
                    "general rather than AC6-specific and widening it wants testing "
                    "far beyond one ocean. Normally driven by ac6_fix_water_line.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_BOOL(ac6_flare_drop_quad2, true, "AC6/Fixes",
                    "Fix the faint rectangle around the sun by culling the lens "
                    "flare's spurious second billboard.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(ac6_flare_drop_index_min, 4, "AC6/Fixes",
                     "First vertex index of the flare draw to cull (the spurious "
                     "billboard is vertices 4-7 of 8).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");
// Consumed once at graphics SetupContext (a one-shot write-watch callback
// registration), so a change needs a restart to take effect.
REXCVAR_DEFINE_BOOL(ac6_fix_trails, true, "AC6/Fixes",
                    "Fix invisible missile and jet trails by refreshing the GPU copy "
                    "of the trail history when the CPU rewrites it.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}
