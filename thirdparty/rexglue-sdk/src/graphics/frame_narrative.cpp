#include <rex/graphics/frame_narrative.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include <rex/graphics/registers.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(narrate_frame_every_s, 0.0, "GPU",
                      "Write one whole frame to the log as [NARR] lines - every draw with its "
                      "targets, transfers, shaders and textures, every resolve with its source and "
                      "destination - this many seconds apart. 0 disables.");
REXCVAR_DEFINE_INT32(narrate_frame_max_draws, 4000, "GPU",
                     "Stop narrating a frame after this many draws.");

namespace rex::graphics::narrative {

namespace {

bool g_active = false;
uint32_t g_frame = 0;
uint32_t g_draw = 0;
uint32_t g_resolve = 0;
std::chrono::steady_clock::time_point g_last_narrated{};

// Where resolves put their pixels, so a later texture fetch from that address
// can be named for what it is. Keyed by the destination base; the range is
// kept for fetches that start inside a destination rather than at it.
struct ResolveDest {
  uint32_t frame;
  uint32_t resolve;
  uint32_t extent_start;
  uint32_t extent_length;
  std::string source;
};
std::map<uint32_t, ResolveDest> g_resolve_dests;

const char* SampleSelectName(xenos::CopySampleSelect select) {
  switch (select) {
    case xenos::CopySampleSelect::k0:
      return "s0";
    case xenos::CopySampleSelect::k1:
      return "s1";
    case xenos::CopySampleSelect::k2:
      return "s2";
    case xenos::CopySampleSelect::k3:
      return "s3";
    case xenos::CopySampleSelect::k01:
      return "avg01";
    case xenos::CopySampleSelect::k23:
      return "avg23";
    case xenos::CopySampleSelect::k0123:
      return "avg0123";
    default:
      return "?";
  }
}

const ResolveDest* FindResolveDest(uint32_t address) {
  auto it = g_resolve_dests.find(address);
  if (it != g_resolve_dests.end()) {
    return &it->second;
  }
  // Not at a destination's base - inside one?
  auto upper = g_resolve_dests.upper_bound(address);
  if (upper == g_resolve_dests.begin()) {
    return nullptr;
  }
  --upper;
  const ResolveDest& dest = upper->second;
  if (address >= dest.extent_start && address < dest.extent_start + dest.extent_length) {
    return &dest;
  }
  return nullptr;
}

}  // namespace

bool Active() { return g_active; }

void OnSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height) {
  if (g_active) {
    REXGPU_ERROR("[NARR] === frame {} end: swap fb {:08X} {}x{} | {} draws {} resolves ===",
                 g_frame, frontbuffer_ptr, width, height, g_draw, g_resolve);
    g_active = false;
  }
  ++g_frame;
  g_resolve = 0;
  const double every = REXCVAR_GET(narrate_frame_every_s);
  if (every <= 0.0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (g_last_narrated.time_since_epoch().count() != 0 &&
      now - g_last_narrated < std::chrono::duration<double>(every)) {
    return;
  }
  g_last_narrated = now;
  g_active = true;
  g_draw = 0;
  REXGPU_ERROR("[NARR] === frame {} begin ===", g_frame);
}

void OnDraw(const RegisterFile& regs, const Shader* vertex_shader, const Shader* pixel_shader,
            xenos::PrimitiveType prim_type, uint32_t index_count, const RenderTargetCache& rtc,
            uint64_t bin_mask, uint64_t bin_select, bool is_clear_quad) {
  if (!g_active) {
    return;
  }
  const uint32_t draw = g_draw++;
  if (int32_t(draw) >= REXCVAR_GET(narrate_frame_max_draws)) {
    return;
  }

  // Targets as the render target cache bound them for this draw, and what it
  // had to copy in first.
  std::string targets;
  std::string transfers;
  rtc.DescribeLastUpdate(targets, transfers);

  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  const auto scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const float vp_xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  const float vp_xo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  const float vp_ys = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  const float vp_yo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  const float vp_zs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  const float vp_zo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
  std::string viewport;
  if (vte.vport_x_scale_ena || vte.vport_x_offset_ena) {
    viewport = fmt::format("vp x{:g}..{:g} y{:g}..{:g} z{:g}..{:g}", vp_xo - std::fabs(vp_xs),
                           vp_xo + std::fabs(vp_xs), vp_yo - std::fabs(vp_ys),
                           vp_yo + std::fabs(vp_ys), vp_zo, vp_zo + vp_zs);
  } else {
    viewport = "vp none";
  }

  REXGPU_ERROR(
      "[NARR] d{} {}vs {:016X} ps {:016X} prim {} n {} |{} | pitch {} msaa {}x | {} | winoff "
      "{},{} sc {},{}-{},{} | vte {:03X} aa {:08X} vtx {:X} sumode {:08X} clip {:08X} | cmask "
      "{:04X} blend0 {:08X} dc {:08X} cc {:08X} mode {:X} sref {:08X} | bin m {:X} s {:X}",
      draw, is_clear_quad ? "CLEAR-QUAD " : "", vertex_shader ? vertex_shader->ucode_data_hash() : 0,
      pixel_shader ? pixel_shader->ucode_data_hash() : 0, uint32_t(prim_type), index_count, targets,
      uint32_t(surface_info.surface_pitch), 1u << uint32_t(surface_info.msaa_samples), viewport,
      int32_t(window_offset.window_x_offset), int32_t(window_offset.window_y_offset),
      uint32_t(scissor_tl.tl_x), uint32_t(scissor_tl.tl_y), uint32_t(scissor_br.br_x),
      uint32_t(scissor_br.br_y), vte.value, regs.values[XE_GPU_REG_PA_SC_AA_CONFIG],
      regs.values[XE_GPU_REG_PA_SU_VTX_CNTL], regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
      regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL], regs.values[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF,
      regs.values[XE_GPU_REG_RB_BLENDCONTROL0], regs.values[XE_GPU_REG_RB_DEPTHCONTROL],
      regs.values[XE_GPU_REG_RB_COLORCONTROL], regs.values[XE_GPU_REG_RB_MODECONTROL] & 0xF,
      regs.values[XE_GPU_REG_RB_STENCILREFMASK], bin_mask, bin_select);
  if (!transfers.empty()) {
    REXGPU_ERROR("[NARR]   xfer{}", transfers);
  }

  // The textures each shader reads, by fetch constant, with the resolve that
  // produced them when there is one.
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const Shader* shader = stage ? pixel_shader : vertex_shader;
    if (!shader) {
      continue;
    }
    for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
      const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
      if (fetch.type != xenos::FetchConstantType::kTexture &&
          fetch.type != xenos::FetchConstantType::kInvalidTexture) {
        continue;
      }
      const uint32_t address = fetch.base_address << 12;
      uint32_t width = 0, height = 0, depth = 0;
      switch (fetch.dimension) {
        case xenos::DataDimension::k1D:
          width = fetch.size_1d.width + 1;
          break;
        case xenos::DataDimension::k2DOrStacked:
          width = fetch.size_2d.width + 1;
          height = fetch.size_2d.height + 1;
          depth = fetch.size_2d.stack_depth + 1;
          break;
        case xenos::DataDimension::k3D:
          width = fetch.size_3d.width + 1;
          height = fetch.size_3d.height + 1;
          depth = fetch.size_3d.depth + 1;
          break;
        case xenos::DataDimension::kCube:
          width = fetch.size_2d.width + 1;
          height = fetch.size_2d.height + 1;
          depth = 6;
          break;
      }
      std::string origin;
      if (const ResolveDest* dest = FindResolveDest(address)) {
        origin = fmt::format(" <- r{}@f{} [{}]{}", dest->resolve, dest->frame, dest->source,
                             address == dest->extent_start ? "" : " (inside)");
      }
      REXGPU_ERROR("[NARR]   {} t{} @{:08X} fmt {} {}x{}x{} {} pitch {} mips {}-{} filt {}/{}/{} "
                   "swz {:03X} sign {}{}{}{}{}",
                   stage ? "ps" : "vs", binding.fetch_constant, address, uint32_t(fetch.format),
                   width, height, depth, fetch.tiled ? "tiled" : "linear", uint32_t(fetch.pitch),
                   uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
                   uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
                   uint32_t(fetch.mip_filter), uint32_t(fetch.swizzle), uint32_t(fetch.sign_x),
                   uint32_t(fetch.sign_y), uint32_t(fetch.sign_z), uint32_t(fetch.sign_w), origin);
    }
  }
}

void OnResolve(const draw_util::ResolveInfo& info) {
  // Destinations are remembered in every frame, narrated or not: a texture
  // read in the narrated frame may have been resolved in the one before it.
  // Only the destination label is built outside a narrated frame - this runs
  // ~70 times a frame on the command processor thread.
  if (REXCVAR_GET(narrate_frame_every_s) <= 0.0) {
    return;
  }
  const uint32_t resolve = g_resolve++;
  const draw_util::ResolveEdramInfo edram =
      info.IsCopyingDepth() ? info.depth_edram_info : info.color_edram_info;
  const uint32_t x = info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t y = info.coordinate_info.edram_offset_y_div_8 * 8;
  const uint32_t width = info.coordinate_info.width_div_8 * 8;
  const uint32_t height = info.height_div_8 * 8;
  const std::string source = fmt::format(
      "{} {}/{}/{}x f{}", edram.is_depth ? "depth" : "color", uint32_t(edram.base_tiles),
      uint32_t(edram.pitch_tiles), 1u << uint32_t(edram.msaa_samples), uint32_t(edram.format));
  if (info.copy_dest_extent_length) {
    ResolveDest& dest = g_resolve_dests[info.copy_dest_base];
    dest.frame = g_frame;
    dest.resolve = resolve;
    dest.extent_start = info.copy_dest_extent_start;
    dest.extent_length = info.copy_dest_extent_length;
    dest.source = fmt::format("{} {}x{}@{},{}", source, width, height, x, y);
  }
  if (!g_active) {
    return;
  }
  std::string copy;
  if (info.copy_dest_extent_length) {
    copy = fmt::format(" copy {} -> @{:08X} (+{:X}..+{:X}) fmt {} pitch {} h {} off {},{}",
                       SampleSelectName(info.rb_copy_control.copy_sample_select),
                       info.copy_dest_base, info.copy_dest_extent_start - info.copy_dest_base,
                       info.copy_dest_extent_start + info.copy_dest_extent_length -
                           info.copy_dest_base,
                       uint32_t(info.copy_dest_info.copy_dest_format),
                       uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
                       uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
                       uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * 8,
                       uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * 8);
  }
  std::string clear;
  if (info.IsClearingDepth()) {
    clear += fmt::format(" clear-depth {:08X}", info.rb_depth_clear);
  }
  if (info.IsClearingColor()) {
    clear += fmt::format(" clear-color {:08X}", info.rb_color_clear);
  }
  REXGPU_ERROR("[NARR] r{} after d{}: [{}] {}x{} at {},{}{}{}", resolve, g_draw, source, width,
               height, x, y, copy, clear);
}

}  // namespace rex::graphics::narrative
