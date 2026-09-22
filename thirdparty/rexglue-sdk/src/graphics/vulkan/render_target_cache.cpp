/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/frame_narrative.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_STRING(render_target_path_vulkan, "", "GPU/Vulkan",
                      "Vulkan render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// DEFINE_string(
//     render_target_path_vulkan, "",
//     "Render target emulation path to use on Vulkan.\n"
//     "Use: [any, fbo, fsi]\n"
//     " fbo:\n"
//     "  Host framebuffers and fixed-function blending and depth / stencil "
//     "testing, copying between render targets when needed.\n"
//     "  Lower accuracy (limited pixel format support).\n"
//     "  Performance limited primarily by render target layout changes requiring "
//     "copying, but generally higher.\n"
//     " fsi:\n"
//     "  Manual pixel packing, blending and depth / stencil testing, with free "
//     "render target layout changes.\n"
//     "  Requires a GPU supporting fragment shader interlock.\n"
//     "  Highest accuracy (all pixel formats handled in software).\n"
//     "  Performance limited primarily by overdraw.\n"
//     " Any other value:\n"
//     "  Choose what is considered the most optimal for the system (currently "
//     "always FB because the FSI path is much slower now).",
//     "GPU");

REXCVAR_DECLARE(bool, gpu_timestamps);
REXCVAR_DEFINE_BOOL(ac6_wide_world_target, true, "AC6/Enhancements",
                    "Render AC6's tiled 1280x720 2x MSAA world in one pass: a 640-wide 2x "
                    "render target gets a host image twice as wide, the second predicated "
                    "tile's draws are dropped and the first tile's scissor spans the whole "
                    "width. Removes ~25% of a frame's draws - the biggest cost on a machine "
                    "bound by the command processor thread. The EDRAM model is untouched; "
                    "the two halves are exchanged by image copies around the guest's own "
                    "clears and resolves (see IsWideKey).");
REXCVAR_DEFINE_INT32(ac6_wide_world_target_log, 0, "AC6/Enhancements",
                     "Log this many wide-world-target events ([WIDE] lines: pass starts, "
                     "rebinds, first/second tile resolves) and then go quiet.");
REXCVAR_DEFINE_BOOL(ac6_edram_no_transfers, false, "AC6/Enhancements",
                    "EXPERIMENT: never copy EDRAM contents between host render targets on "
                    "ownership change. Logs every transfer it skips ([EDRAM-SKIP]) so the "
                    "aliases the game actually relies on can be identified.");

REXCVAR_DEFINE_BOOL(ac6_edram_skip_stencil_transfers, true, "AC6/Enhancements",
                    "Don't carry stencil across EDRAM ownership transfers. Worth about 1 ms of a "
                    "4K / scale 3 frame, and two missions looked identical with it on - but the "
                    "game does test stencil (about 114 draws a frame read it with a real compare "
                    "function), so a scene that depends on stencil surviving a reinterpretation "
                    "would break. Turn it off if masked effects misbehave")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_edram_stencil_transfer_compute, true, "GPU",
                    "Without VK_EXT_shader_stencil_export, transfer stencil into single-sampled "
                    "depth render targets with one compute dispatch and a buffer-to-image copy "
                    "per rectangle instead of eight masked draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_edram_stencil_copy_general, false, "GPU",
                    "A/B: copy the stencil bytes with the destination in the GENERAL layout "
                    "instead of TRANSFER_DST_OPTIMAL")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_resolve_to_texture_prefer_compute, false, "GPU",
                    "Let the compute shader take every resolve it can rather than only those the "
                    "image copy cannot express (measured slower: the copy moves the same pixels "
                    "in less time, so this is off)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_resolve_to_texture_compute, true, "GPU",
                    "Resolve single-sampled resolve views into the destination texture images with "
                    "a compute shader (sampling the render target and unpacking as the texture "
                    "load would) instead of a copy or the tiled-memory path; covers owners of any "
                    "sample count and depth destinations")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(vulkan_resolve_to_texture_compute_debug, 0, "GPU",
                     "Debug the compute resolve: 1 - write solid magenta, 2 - write the render "
                     "target sample without packing, 3 - write the packed dword's low byte as "
                     "grey (0 - off, write the resolved texel)");

REXCVAR_DEFINE_BOOL(vulkan_resolve_to_texture_msaa, true, "GPU",
                    "Also resolve multisampled render targets into texture images with "
                    "vkCmdResolveImage when the resolve averages all samples")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_resolve_to_texture_image, true, "GPU",
                    "Resolve straight into the destination texture's host image with a copy when "
                    "the texture already exists and the render target holds its exact bits, "
                    "skipping the tiled-memory write and the untiling load")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {
std::atomic<int32_t> g_wide_log_budget{-1};
bool WideLogTake() {
  int32_t budget = g_wide_log_budget.load(std::memory_order_relaxed);
  if (budget < 0) {
    budget = REXCVAR_GET(ac6_wide_world_target_log);
    g_wide_log_budget.store(budget, std::memory_order_relaxed);
  }
  if (budget <= 0) {
    return false;
  }
  g_wide_log_budget.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/vulkan_spirv/host_depth_store_1xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/passthrough_position_xy_vs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs, sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs, sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs, sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs, sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs, sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[size_t(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit, kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit | kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const VulkanRenderTargetCache::TransferModeInfo
    VulkanRenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

VulkanRenderTargetCache::VulkanRenderTargetCache(const RegisterFile& register_file,
                                                 const memory::Memory& memory,
                                                 TraceWriter& trace_writer,
                                                 uint32_t draw_resolution_scale_x,
                                                 uint32_t draw_resolution_scale_y,
                                                 VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, &trace_writer, draw_resolution_scale_x,
                        draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

VulkanRenderTargetCache::~VulkanRenderTargetCache() {
  Shutdown(true);
}

bool VulkanRenderTargetCache::Initialize(uint32_t shared_memory_binding_count) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  bool fsi_path_supported =
      (device_properties.fragmentShaderSampleInterlock ||
       device_properties.fragmentShaderPixelInterlock) &&
      device_properties.fragmentStoresAndAtomics && device_properties.sampleRateShading &&
      device_properties.standardSampleLocations &&
      shared_memory_binding_count < device_properties.maxPerStageDescriptorStorageBuffers;
  if (REXCVAR_GET(render_target_path_vulkan) == "fsi") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
  }
  // Fragment shader interlock is a feature implemented by pretty advanced GPUs,
  // closer to Direct3D 11 / OpenGL ES 3.2 level mainly, not Direct3D 10 /
  // OpenGL ES 3.1. Thus, it's fine to demand a wide range of other optional
  // features for the fragment shader interlock backend to work.
  if (path_ == Path::kPixelShaderInterlock) {
    // Interlocking between fragments with common sample coverage is enough, but
    // interlocking more is acceptable too (fragmentShaderShadingRateInterlock
    // would be okay too, but it's unlikely that an implementation would
    // advertise only it and not any other ones, as it's a very specific feature
    // interacting with another optional feature that is variable shading rate,
    // so there's no need to overcomplicate the checks and the shader execution
    // mode setting).
    // Sample-rate shading is required by certain SPIR-V revisions to access the
    // sample mask fragment shader input.
    // Stanard sample locations are needed for calculating the depth at the
    // samples.
    // It's unlikely that a device exposing fragment shader interlock won't have
    // a large enough storage buffer range and a sufficient SSBO slot count for
    // all the shared memory buffers and the EDRAM buffer - an in a conflict
    // between, for instance, the ability to vfetch and memexport in fragment
    // shaders, and the usage of fragment shader interlock, prefer the former
    // for simplicity.
    if (!fsi_path_supported) {
      path_ = Path::kHostRenderTargets;
    }
  }

  // Format support.
  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  constexpr VkFormatFeatureFlags kUsedColorFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  bool gamma_render_target_as_unorm16_requested =
      path_ == Path::kHostRenderTargets && REXCVAR_GET(gamma_render_target_as_unorm16);
  VkFormatProperties depth_unorm24_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D24_UNORM_S8_UINT,
                                          &depth_unorm24_properties);
  depth_unorm24_vulkan_format_supported_ = (depth_unorm24_properties.optimalTilingFeatures &
                                            kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
  VkFormatProperties color_rg16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SNORM,
                                          &color_rg16_snorm_properties);
  VkFormatProperties color_rgba16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SNORM,
                                          &color_rgba16_snorm_properties);
  VkFormatProperties color_rg16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SFLOAT,
                                          &color_rg16_sfloat_properties);
  VkFormatProperties color_rgba16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                          &color_rgba16_sfloat_properties);
  VkFormatProperties color_rgba16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UNORM,
                                          &color_rgba16_unorm_properties);
  bool color_rg16_snorm_supported = (color_rg16_snorm_properties.optimalTilingFeatures &
                                     kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_snorm_supported = (color_rgba16_snorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_sfloat_supported = (color_rg16_sfloat_properties.optimalTilingFeatures &
                                      kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_sfloat_supported = (color_rgba16_sfloat_properties.optimalTilingFeatures &
                                        kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_unorm_supported = (color_rgba16_unorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_draw_format_supported = true;
  if (!color_rg16_snorm_supported) {
    if (color_rg16_sfloat_supported) {
      color_rg16_draw_format_fallback_to_float_ = true;
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16_SNORM render target support is unavailable; "
          "falling back to R16G16_SFLOAT for k_16_16");
    } else {
      color_rg16_draw_format_supported = false;
    }
  }
  bool color_rgba16_draw_format_supported = true;
  if (!color_rgba16_snorm_supported) {
    if (color_rgba16_sfloat_supported) {
      color_rgba16_draw_format_fallback_to_float_ = true;
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_SNORM render target support is unavailable; "
          "falling back to R16G16B16A16_SFLOAT for k_16_16_16_16");
    } else {
      color_rgba16_draw_format_supported = false;
    }
  }
  if (path_ == Path::kHostRenderTargets &&
      (!color_rg16_draw_format_supported || !color_rgba16_draw_format_supported)) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}); switching to fragment shader interlock "
          "path for D3D12 parity",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}), and fragment shader interlock fallback "
          "is unavailable",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      return false;
    }
  }
  if (path_ == Path::kHostRenderTargets && gamma_render_target_as_unorm16_requested &&
      !color_rgba16_unorm_supported) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage; switching to "
          "fragment shader interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage, and fragment "
          "shader interlock fallback is unavailable");
      return false;
    }
  }
  VkFormatProperties color_rg16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_UINT,
                                          &color_rg16_uint_properties);
  VkFormatProperties color_rgba16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UINT,
                                          &color_rgba16_uint_properties);
  VkFormatProperties color_r32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_UINT,
                                          &color_r32_uint_properties);
  VkFormatProperties color_rg32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32G32_UINT,
                                          &color_rg32_uint_properties);
  color_16bit_transfer_uint_formats_supported_ =
      (color_rg16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rgba16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_16bit_transfer_uint_formats_supported_) {
    REXGPU_WARN(
        "VulkanRenderTargetCache: R16G16/R16G16B16A16 UINT ownership transfer formats are not "
        "supported");
  }
  color_32bit_transfer_uint_formats_supported_ =
      (color_r32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rg32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_32bit_transfer_uint_formats_supported_) {
    REXGPU_WARN(
        "VulkanRenderTargetCache: R32/R32G32 UINT ownership transfer formats are not supported");
  }
  if ((device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
      !(device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
    REXGPU_WARN("VulkanRenderTargetCache: 4x integer sampled-image support is unavailable");
  }

  // 2x MSAA support.
  if (REXCVAR_GET(native_2x_msaa)) {
    msaa_2x_attachments_supported_ =
        (device_properties.framebufferColorSampleCounts &
         device_properties.framebufferDepthSampleCounts &
         device_properties.framebufferStencilSampleCounts &
         device_properties.sampledImageColorSampleCounts &
         device_properties.sampledImageDepthSampleCounts &
         device_properties.sampledImageStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT);
    msaa_2x_no_attachments_supported_ =
        (device_properties.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  } else {
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;
  }
  bool integer_transfer_sample_1x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
  bool integer_transfer_sample_2x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  bool integer_transfer_sample_4x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0 ||
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0;
  bool bit_exact_host_color_transfer_supported =
      color_16bit_transfer_uint_formats_supported_ &&
      color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_1x_supported &&
      integer_transfer_sample_4x_supported &&
      (!msaa_2x_attachments_supported_ || integer_transfer_sample_2x_supported);
  if (path_ == Path::kHostRenderTargets && !bit_exact_host_color_transfer_supported) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target ownership transfers "
          "can't be bit-exact on this device; switching to fragment shader "
          "interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Bit-exact host render target ownership "
          "transfers require UINT transfer formats and integer sampled-image "
          "MSAA support, and fragment shader interlock fallback is unavailable");
      return false;
    }
  }

  // Descriptor set layouts.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_storage_buffer_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one sampled image");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags = descriptor_set_layout_bindings[0].stageFlags;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 2;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_x2_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with two sampled images");
    Shutdown();
    return false;
  }

  // Descriptor set pools.
  // The pool sizes were chosen without a specific reason.
  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ = std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
      vulkan_device, 256, 1, &descriptor_set_layout_size, descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // EDRAM contents reinterpretation buffer.
  // 90 MB with 9x resolution scaling - within the minimum
  // maxStorageBufferRange.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize(xenos::kEdramSizeBytes *
                       (draw_resolution_scale_x() * draw_resolution_scale_y())),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, edram_buffer_, edram_buffer_memory_)) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    // The first operation will likely be drawing.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentReadWrite;
  } else {
    // The first operation will likely be depth self-comparison.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPoolSize edram_storage_buffer_descriptor_pool_size;
  edram_storage_buffer_descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo edram_storage_buffer_descriptor_pool_create_info;
  edram_storage_buffer_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  edram_storage_buffer_descriptor_pool_create_info.pNext = nullptr;
  edram_storage_buffer_descriptor_pool_create_info.flags = 0;
  edram_storage_buffer_descriptor_pool_create_info.maxSets = 1;
  edram_storage_buffer_descriptor_pool_create_info.poolSizeCount = 1;
  edram_storage_buffer_descriptor_pool_create_info.pPoolSizes =
      &edram_storage_buffer_descriptor_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &edram_storage_buffer_descriptor_pool_create_info, nullptr,
                                 &edram_storage_buffer_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the EDRAM buffer storage "
        "buffer descriptor pool");
    Shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo edram_storage_buffer_descriptor_set_allocate_info;
  edram_storage_buffer_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  edram_storage_buffer_descriptor_set_allocate_info.pNext = nullptr;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorPool =
      edram_storage_buffer_descriptor_pool_;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorSetCount = 1;
  edram_storage_buffer_descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_storage_buffer_;
  if (dfn.vkAllocateDescriptorSets(device, &edram_storage_buffer_descriptor_set_allocate_info,
                                   &edram_storage_buffer_descriptor_set_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to allocate the EDRAM buffer storage "
        "buffer descriptor set");
    Shutdown();
    return false;
  }
  VkDescriptorBufferInfo edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_buffer_info.buffer = edram_buffer_;
  edram_storage_buffer_descriptor_buffer_info.offset = 0;
  edram_storage_buffer_descriptor_buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet edram_storage_buffer_descriptor_write;
  edram_storage_buffer_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  edram_storage_buffer_descriptor_write.pNext = nullptr;
  edram_storage_buffer_descriptor_write.dstSet = edram_storage_buffer_descriptor_set_;
  edram_storage_buffer_descriptor_write.dstBinding = 0;
  edram_storage_buffer_descriptor_write.dstArrayElement = 0;
  edram_storage_buffer_descriptor_write.descriptorCount = 1;
  edram_storage_buffer_descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_write.pImageInfo = nullptr;
  edram_storage_buffer_descriptor_write.pBufferInfo = &edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &edram_storage_buffer_descriptor_write, 0, nullptr);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Resolve copy pipeline layout.
  VkDescriptorSetLayout resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEdram] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Potentially binding all of the shared memory at 1x resolution, but only
  // portions with scaled resolution.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
                             : sizeof(draw_util::ResolveCopyShaderConstants);
  VkPipelineLayoutCreateInfo resolve_copy_pipeline_layout_create_info;
  resolve_copy_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  resolve_copy_pipeline_layout_create_info.pNext = nullptr;
  resolve_copy_pipeline_layout_create_info.flags = 0;
  resolve_copy_pipeline_layout_create_info.setLayoutCount = kResolveCopyDescriptorSetCount;
  resolve_copy_pipeline_layout_create_info.pSetLayouts = resolve_copy_descriptor_set_layouts;
  resolve_copy_pipeline_layout_create_info.pushConstantRangeCount = 1;
  resolve_copy_pipeline_layout_create_info.pPushConstantRanges = &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_copy_pipeline_layout_create_info, nullptr,
                                 &resolve_copy_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
        "layout");
    Shutdown();
    return false;
  }

  // Direct resolve pipeline layouts (destination storage buffer + source image).
  auto create_direct_resolve_pipeline_layout = [&](VkDescriptorSetLayout source_layout,
                                                   VkPipelineLayout* pipeline_layout_out) {
    VkDescriptorSetLayout descriptor_set_layouts[] = {
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute),
        source_layout,
    };
    VkPushConstantRange push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(DirectResolvePushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_create_info;
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
    pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                   pipeline_layout_out) != VK_SUCCESS) {
      *pipeline_layout_out = VK_NULL_HANDLE;
    }
  };
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_,
                                        &direct_resolve_pipeline_layout_color_);
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_x2_,
                                        &direct_resolve_pipeline_layout_depth_);

  // Resolve copy pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size_bytes &&
                resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size_bytes);
    VkPipeline resolve_copy_pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the resolve copy "
          "pipeline {}",
          resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, resolve_copy_pipeline,
                                 resolve_copy_shader_info.debug_name);
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    gamma_render_target_as_unorm16_ = gamma_render_target_as_unorm16_requested;

    depth_float24_round_ = REXCVAR_GET(depth_float24_round);
    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

    // Host depth storing pipeline layout.
    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        // Destination EDRAM storage buffer.
        descriptor_set_layout_storage_buffer_,
        // Source depth / stencil texture (only depth is used).
        descriptor_set_layout_sampled_image_x2_,
    };
    VkPushConstantRange host_depth_store_push_constant_range;
    host_depth_store_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    host_depth_store_push_constant_range.offset = 0;
    host_depth_store_push_constant_range.size = sizeof(HostDepthStoreConstants);
    VkPipelineLayoutCreateInfo host_depth_store_pipeline_layout_create_info;
    host_depth_store_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    host_depth_store_pipeline_layout_create_info.pNext = nullptr;
    host_depth_store_pipeline_layout_create_info.flags = 0;
    host_depth_store_pipeline_layout_create_info.setLayoutCount =
        uint32_t(rex::countof(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &host_depth_store_pipeline_layout_create_info, nullptr,
                                   &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the host depth storing "
          "pipeline layout");
      Shutdown();
      return false;
    }
    const std::pair<const uint32_t*, size_t> host_depth_store_shaders[] = {
        {shaders::host_depth_store_1xmsaa_cs, sizeof(shaders::host_depth_store_1xmsaa_cs)},
        {shaders::host_depth_store_2xmsaa_cs, sizeof(shaders::host_depth_store_2xmsaa_cs)},
        {shaders::host_depth_store_4xmsaa_cs, sizeof(shaders::host_depth_store_4xmsaa_cs)},
    };
    for (size_t i = 0; i < rex::countof(host_depth_store_shaders); ++i) {
      const std::pair<const uint32_t*, size_t> host_depth_store_shader =
          host_depth_store_shaders[i];
      VkPipeline host_depth_store_pipeline = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, host_depth_store_pipeline_layout_, host_depth_store_shader.first,
          host_depth_store_shader.second);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the {}-sample host "
            "depth storing pipeline",
            uint32_t(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        std::max(
            ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
            sizeof(float) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles * xenos::kEdramTileCount));

    // Transfer vertex shader.
    transfer_passthrough_vertex_shader_ =
        ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::passthrough_position_xy_vs,
                                             sizeof(shaders::passthrough_position_xy_vs));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the render target "
          "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    // Transfer pipeline layouts.
    VkDescriptorSetLayout
        transfer_pipeline_layout_descriptor_set_layouts[kTransferUsedDescriptorSetCount];
    VkPushConstantRange transfer_pipeline_layout_push_constant_range;
    transfer_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    transfer_pipeline_layout_push_constant_range.offset = 0;
    VkPipelineLayoutCreateInfo transfer_pipeline_layout_create_info;
    transfer_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    transfer_pipeline_layout_create_info.pNext = nullptr;
    transfer_pipeline_layout_create_info.flags = 0;
    transfer_pipeline_layout_create_info.pSetLayouts =
        transfer_pipeline_layout_descriptor_set_layouts;
    transfer_pipeline_layout_create_info.pPushConstantRanges =
        &transfer_pipeline_layout_push_constant_range;
    for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      uint32_t transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      uint32_t transfer_pipeline_layout_descriptor_set_index;
      while (rex::bit_scan_forward(transfer_pipeline_layout_descriptor_sets_remaining,
                                   &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(uint32_t(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout = VK_NULL_HANDLE;
        switch (TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index)) {
          case kTransferUsedDescriptorSetHostDepthBuffer:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_storage_buffer_;
            break;
          case kTransferUsedDescriptorSetHostDepthStencilTextures:
          case kTransferUsedDescriptorSetDepthStencilTextures:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_x2_;
            break;
          case kTransferUsedDescriptorSetColorTexture:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_sampled_image_;
            break;
          default:
            assert_unhandled_case(
                TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts[transfer_pipeline_layout_create_info
                                                            .setLayoutCount++] =
            transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size =
          uint32_t(sizeof(uint32_t) *
                   rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(device, &transfer_pipeline_layout_create_info, nullptr,
                                     &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline layout {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dump pipeline layouts.
    VkDescriptorSetLayout dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size = sizeof(uint32_t) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        uint32_t(rex::countof(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts = dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_color_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the color render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_depth_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the depth render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }

    // Stencil transfer compute pipeline layouts: same sets as dumping (output
    // buffer, then the source), wider push constants.
    VkPushConstantRange stencil_compute_push_constant_range;
    stencil_compute_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    stencil_compute_push_constant_range.offset = 0;
    stencil_compute_push_constant_range.size = sizeof(StencilComputePushConstants);
    // The output set is the command processor's transient storage buffer
    // descriptor, so its layout must be the one those are allocated with.
    VkDescriptorSetLayout stencil_compute_descriptor_set_layouts[kDumpDescriptorSetCount];
    stencil_compute_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
    stencil_compute_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    VkPipelineLayoutCreateInfo stencil_compute_pipeline_layout_create_info =
        dump_pipeline_layout_create_info;
    stencil_compute_pipeline_layout_create_info.pSetLayouts =
        stencil_compute_descriptor_set_layouts;
    stencil_compute_pipeline_layout_create_info.pPushConstantRanges =
        &stencil_compute_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &stencil_compute_pipeline_layout_create_info, nullptr,
                                   &stencil_compute_pipeline_layout_depth_) != VK_SUCCESS) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the depth stencil transfer pipeline "
                   "layout");
      Shutdown();
      return false;
    }
    stencil_compute_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    if (dfn.vkCreatePipelineLayout(device, &stencil_compute_pipeline_layout_create_info, nullptr,
                                   &stencil_compute_pipeline_layout_color_) != VK_SUCCESS) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the color stencil transfer pipeline "
                   "layout");
      Shutdown();
      return false;
    }

    // Resolve-to-image pipeline layouts: storage image, then the source.
    VkDescriptorSetLayout resolve_to_image_descriptor_set_layouts[kDumpDescriptorSetCount];
    resolve_to_image_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageImageCompute);
    resolve_to_image_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange resolve_to_image_push_constant_range;
    resolve_to_image_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_to_image_push_constant_range.offset = 0;
    resolve_to_image_push_constant_range.size = sizeof(ResolveToImagePushConstants);
    VkPipelineLayoutCreateInfo resolve_to_image_pipeline_layout_create_info =
        dump_pipeline_layout_create_info;
    resolve_to_image_pipeline_layout_create_info.pSetLayouts =
        resolve_to_image_descriptor_set_layouts;
    resolve_to_image_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_to_image_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &resolve_to_image_pipeline_layout_create_info, nullptr,
                                   &resolve_to_image_pipeline_layout_color_) != VK_SUCCESS) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the color resolve-to-image pipeline "
                   "layout");
      Shutdown();
      return false;
    }
    resolve_to_image_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &resolve_to_image_pipeline_layout_create_info, nullptr,
                                   &resolve_to_image_pipeline_layout_depth_) != VK_SUCCESS) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the depth resolve-to-image pipeline "
                   "layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel (fragment) shader interlock.

    // Keep parity with D3D12 ROV, which uses 2x-as-4x in this path.
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;

    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // The pipeline layout and the pipelines for clearing the EDRAM buffer in
    // resolves.
    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size = sizeof(draw_util::ResolveClearShaderConstants);
    VkPipelineLayoutCreateInfo resolve_fsi_clear_pipeline_layout_create_info;
    resolve_fsi_clear_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_fsi_clear_pipeline_layout_create_info.pNext = nullptr;
    resolve_fsi_clear_pipeline_layout_create_info.flags = 0;
    resolve_fsi_clear_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pSetLayouts =
        &descriptor_set_layout_storage_buffer_;
    resolve_fsi_clear_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_fsi_clear_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &resolve_fsi_clear_pipeline_layout_create_info, nullptr,
                                   &resolve_fsi_clear_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear pipeline layout");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_32bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs));
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs));
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }

    // Common render pass.
    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Fragment shader interlock provides synchronization and ordering within a
    // subpass, create an external by-region dependency to maintain interlocking
    // between passes. Framebuffer-global dependencies will be made with
    // explicit barriers when the addressing of the EDRAM buffer relatively to
    // the fragment coordinates is changed.
    VkSubpassDependency fsi_subpass_dependencies[2];
    fsi_subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    fsi_subpass_dependencies[0].dstSubpass = 0;
    fsi_subpass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    fsi_subpass_dependencies[1] = fsi_subpass_dependencies[0];
    std::swap(fsi_subpass_dependencies[1].srcSubpass, fsi_subpass_dependencies[1].dstSubpass);
    VkRenderPassCreateInfo fsi_render_pass_create_info;
    fsi_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fsi_render_pass_create_info.pNext = nullptr;
    fsi_render_pass_create_info.flags = 0;
    fsi_render_pass_create_info.attachmentCount = 0;
    fsi_render_pass_create_info.pAttachments = nullptr;
    fsi_render_pass_create_info.subpassCount = 1;
    fsi_render_pass_create_info.pSubpasses = &fsi_subpass;
    fsi_render_pass_create_info.dependencyCount = uint32_t(rex::countof(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr, &fsi_render_pass_) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend render pass");
      Shutdown();
      return false;
    }

    // Common framebuffer.
    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_x(),
                 device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
                 device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  // Reset the last update structures, to keep the defaults consistent between
  // paths regardless of whether the update for the path actually modifies them.
  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::memset(last_update_framebuffer_attachments_, 0,
              sizeof(last_update_framebuffer_attachments_));
  last_update_framebuffer_ = VK_NULL_HANDLE;

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ResetTraceDownload();

  // Destroy all render targets before the descriptor set pool is destroyed -
  // may happen if shutting down the VulkanRenderTargetCache by destroying it,
  // so ShutdownCommon is called by the RenderTargetCache destructor, when it's
  // already too late.
  DestroyAllRenderTargets(true);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_64bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_32bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_fsi_clear_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         fsi_framebuffer_.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    // May be null to prevent recreation attempts.
    if (dump_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, dump_pipeline_pair.second, nullptr);
    }
  }
  dump_pipelines_.clear();
  for (const auto& direct_resolve_pipeline_pair : direct_resolve_pipelines_) {
    bool aliased_resolve_copy_pipeline = false;
    for (VkPipeline resolve_copy_pipeline : resolve_copy_pipelines_) {
      if (direct_resolve_pipeline_pair.second == resolve_copy_pipeline) {
        aliased_resolve_copy_pipeline = true;
        break;
      }
    }
    if (direct_resolve_pipeline_pair.second != VK_NULL_HANDLE && !aliased_resolve_copy_pipeline) {
      dfn.vkDestroyPipeline(device, direct_resolve_pipeline_pair.second, nullptr);
    }
  }
  direct_resolve_pipelines_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_color_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_color_);
  for (const auto& resolve_to_image_pipeline_pair : resolve_to_image_pipelines_) {
    if (resolve_to_image_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, resolve_to_image_pipeline_pair.second, nullptr);
    }
  }
  resolve_to_image_pipelines_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_to_image_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_to_image_pipeline_layout_color_);
  for (const auto& stencil_compute_pipeline_pair : stencil_compute_pipelines_) {
    if (stencil_compute_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, stencil_compute_pipeline_pair.second, nullptr);
    }
  }
  stencil_compute_pipelines_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         stencil_compute_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         stencil_compute_pipeline_layout_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      // May be null to prevent recreation attempts.
      if (transfer_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, transfer_pipeline, nullptr);
      }
    }
  }
  transfer_pipelines_.clear();
  for (const auto& transfer_shader_pair : transfer_shaders_) {
    if (transfer_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, transfer_shader_pair.second, nullptr);
    }
  }
  transfer_shaders_.clear();
  for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           transfer_pipeline_layouts_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();
  edram_snapshot_restore_pool_.reset();

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           host_depth_store_pipelines_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         host_depth_store_pipeline_layout_);

  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    if (render_pass_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
    }
  }
  render_passes_.clear();

  for (VkPipeline& resolve_copy_pipeline : resolve_copy_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, resolve_copy_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         edram_storage_buffer_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, edram_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_sampled_image_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_x2_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanRenderTargetCache::ClearCache() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Framebuffer objects must be destroyed because they reference views of
  // attachment images, which may be removed by the common ClearCache.
  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
  }
  render_passes_.clear();

  RenderTargetCache::ClearCache();
}

void VulkanRenderTargetCache::CompletedSubmissionUpdated() {
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void VulkanRenderTargetCache::EndSubmission() {
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->FlushWrites();
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->FlushWrites();
  }
}

void VulkanRenderTargetCache::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         edram_snapshot_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         edram_snapshot_download_buffer_memory_);
  edram_snapshot_download_buffer_memory_type_ = UINT32_MAX;
  edram_snapshot_download_buffer_memory_size_ = 0;
}

bool VulkanRenderTargetCache::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();

  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), xenos::kEdramSizeBytes,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
          edram_snapshot_download_buffer_, edram_snapshot_download_buffer_memory_,
          &edram_snapshot_download_buffer_memory_type_,
          &edram_snapshot_download_buffer_memory_size_)) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create an EDRAM snapshot download "
        "buffer");
    ResetTraceDownload();
    return false;
  }

  if (GetPath() == Path::kHostRenderTargets) {
    // Dump all host render targets to edram_buffer_.
    if (!DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount)) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to dump host render targets for trace");
      ResetTraceDownload();
      return false;
    }
  }

  UseEdramBuffer(EdramBufferUsage::kTransferRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  VkBufferCopy edram_download_copy;
  edram_download_copy.srcOffset = 0;
  edram_download_copy.dstOffset = 0;
  edram_download_copy.size = xenos::kEdramSizeBytes;
  command_buffer.CmdVkCopyBuffer(edram_buffer_, edram_snapshot_download_buffer_, 1,
                                 &edram_download_copy);
  command_processor_.PushBufferMemoryBarrier(
      edram_snapshot_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  return true;
}

void VulkanRenderTargetCache::InitializeTraceCompleteDownloads() {
  if (edram_snapshot_download_buffer_memory_ == VK_NULL_HANDLE) {
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* edram_snapshot_download_mapping = nullptr;
  if (dfn.vkMapMemory(device, edram_snapshot_download_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                      &edram_snapshot_download_mapping) == VK_SUCCESS) {
    if (!(vulkan_device->memory_types().host_coherent &
          (uint32_t(1) << edram_snapshot_download_buffer_memory_type_))) {
      VkMappedMemoryRange edram_snapshot_download_memory_range = {};
      edram_snapshot_download_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      edram_snapshot_download_memory_range.memory = edram_snapshot_download_buffer_memory_;
      edram_snapshot_download_memory_range.offset = 0;
      edram_snapshot_download_memory_range.size =
          std::min(rex::round_up(VkDeviceSize(xenos::kEdramSizeBytes),
                                 vulkan_device->properties().nonCoherentAtomSize),
                   edram_snapshot_download_buffer_memory_size_);
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &edram_snapshot_download_memory_range);
    }

    trace_writer_.WriteEdramSnapshot(edram_snapshot_download_mapping);
    dfn.vkUnmapMemory(device, edram_snapshot_download_buffer_memory_);
  } else {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to map the EDRAM snapshot download "
        "buffer");
  }

  ResetTraceDownload();
}

void VulkanRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  if (!edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, xenos::kEdramSizeBytes);
  }
  VkBuffer upload_buffer;
  VkDeviceSize upload_buffer_offset;
  uint8_t* upload_buffer_mapping = edram_snapshot_restore_pool_->Request(
      command_processor_.GetCurrentSubmission(), xenos::kEdramSizeBytes, 1, upload_buffer,
      upload_buffer_offset);
  if (!upload_buffer_mapping) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to get a buffer for restoring an "
        "EDRAM snapshot");
    return;
  }

  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      // k_32_FLOAT because it's unambiguous.
      VulkanRenderTarget* full_edram_render_target =
          static_cast<VulkanRenderTarget*>(PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
              xenos::ColorRenderTargetFormat::k_32_FLOAT));
      if (!full_edram_render_target) {
        return;
      }
      assert_false(full_edram_render_target->key().is_depth);
      assert_false(full_edram_render_target->key().Is64bpp());
      uint32_t pitch_tiles = full_edram_render_target->key().pitch_tiles_at_32bpp;
      uint32_t tile_rows = xenos::kEdramTileCount / pitch_tiles;
      assert_true(pitch_tiles * tile_rows == xenos::kEdramTileCount);
      uint32_t row_pitch_samples = pitch_tiles * xenos::kEdramTileWidthSamples;
      VkDeviceSize row_pitch_bytes = VkDeviceSize(row_pitch_samples) * sizeof(uint32_t);
      const uint8_t* snapshot_sample_row = reinterpret_cast<const uint8_t*>(snapshot);
      for (uint32_t y_tile = 0; y_tile < tile_rows; ++y_tile) {
        uint8_t* upload_buffer_tile_row_origin =
            upload_buffer_mapping + row_pitch_bytes * xenos::kEdramTileHeightSamples * y_tile;
        for (uint32_t x_tile = 0; x_tile < pitch_tiles; ++x_tile) {
          uint8_t* upload_buffer_sample_row =
              upload_buffer_tile_row_origin +
              sizeof(uint32_t) * xenos::kEdramTileWidthSamples * x_tile;
          for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples; ++sample_row) {
            std::memcpy(upload_buffer_sample_row, snapshot_sample_row,
                        sizeof(uint32_t) * xenos::kEdramTileWidthSamples);
            snapshot_sample_row += sizeof(uint32_t) * xenos::kEdramTileWidthSamples;
            upload_buffer_sample_row += row_pitch_bytes;
          }
        }
      }
      command_processor_.PushImageMemoryBarrier(
          full_edram_render_target->image(),
          ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
          full_edram_render_target->current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
          full_edram_render_target->current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
          full_edram_render_target->current_layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      full_edram_render_target->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      command_processor_.SubmitBarriers(true);
      VkBufferImageCopy copy_region = {};
      copy_region.bufferOffset = upload_buffer_offset;
      copy_region.bufferRowLength = row_pitch_samples;
      copy_region.bufferImageHeight = xenos::kEdramTileHeightSamples * tile_rows;
      copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy_region.imageSubresource.mipLevel = 0;
      copy_region.imageSubresource.baseArrayLayer = 0;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageOffset.x = 0;
      copy_region.imageOffset.y = 0;
      copy_region.imageOffset.z = 0;
      copy_region.imageExtent.width = row_pitch_samples;
      copy_region.imageExtent.height = xenos::kEdramTileHeightSamples * tile_rows;
      copy_region.imageExtent.depth = 1;
      command_buffer.CmdVkCopyBufferToImage(upload_buffer, full_edram_render_target->image(),
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    } break;

    case Path::kPixelShaderInterlock: {
      std::memcpy(upload_buffer_mapping, snapshot, xenos::kEdramSizeBytes);
      UseEdramBuffer(EdramBufferUsage::kTransferWrite);
      command_processor_.SubmitBarriers(true);
      VkBufferCopy copy_region = {};
      copy_region.srcOffset = upload_buffer_offset;
      copy_region.dstOffset = 0;
      copy_region.size = xenos::kEdramSizeBytes;
      command_buffer.CmdVkCopyBuffer(upload_buffer, edram_buffer_, 1, &copy_region);
    } break;

    default:
      assert_unhandled_case(GetPath());
  }
}

bool VulkanRenderTargetCache::Resolve(const memory::Memory& memory,
                                      VulkanSharedMemory& shared_memory,
                                      VulkanTextureCache& texture_cache,
                                      uint32_t& written_address_out, uint32_t& written_length_out) {
  SCOPE_profile_cpu_f("gpu");
  written_address_out = 0;
  written_length_out = 0;
  // Per resolve; the second-tile case below sets it.
  wide_resolve_source_x_offset_tiles_ = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
                                 IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }
  narrative::OnResolve(resolve_info);

  if (REXCVAR_GET(ac6_wide_world_target) && GetPath() == Path::kHostRenderTargets &&
      resolve_info.copy_dest_extent_length) {
    // The guest resolves each tile of a wide target in turn. The first resolve
    // of a span reads what the wide pass put in the left half. The second -
    // no draws into the wide target since - is of a buffer the guest expects
    // tile 1 to have filled: the right half is moved onto the left first,
    // and, if the guest's resolve-clear in between handed the span to an
    // alias-keyed target (the hangar resolves through a 640x1440 1x view),
    // ownership is handed back to the wide target without a transfer, so the
    // resolve reads it exactly as it read the first tile.
    uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
    resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
    GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                   dump_rectangles_);
    RenderTarget* owner =
        dump_rectangles_.size() == 1 ? dump_rectangles_[0].render_target : nullptr;
    RenderTarget* wide = owner && IsWideKey(owner->key()) ? owner : nullptr;
    auto awaited = wide_awaiting_second_resolve_.find(dump_base);
    if (!wide && awaited != wide_awaiting_second_resolve_.end()) {
      wide = awaited->second;
    }
    if (wide) {
      uint32_t& draws = wide_draws_since_resolve_[wide];
      if (WideLogTake()) {
        REXGPU_ERROR("[WIDE] resolve span {} {} owner {} awaited {} draws {} second_renders {} "
                     "dest {:08X}",
                     dump_base, resolve_info.IsCopyingDepth() ? "depth" : "color",
                     owner == wide ? "wide" : (owner ? "other" : "none"),
                     awaited != wide_awaiting_second_resolve_.end() ? 1 : 0, draws,
                     wide_second_tile_renders_ ? 1 : 0, resolve_info.copy_dest_base);
      }
      if (wide_second_tile_renders_) {
        // Both tiles were drawn into the left half; nothing to redirect.
        if (awaited != wide_awaiting_second_resolve_.end()) {
          wide_awaiting_second_resolve_.erase(awaited);
        }
        COUNT_profile_add("gpu/ac6_wide_rendered_tile_resolves", 1);
      } else if (draws == 0 && awaited != wide_awaiting_second_resolve_.end()) {
        const RenderTargetKey wide_key = wide->key();
        if (owner != wide) {
          ChangeOwnershipWithoutTransfer(wide_key, dump_base - wide_key.base_tiles,
                                         dump_rows * dump_pitch);
        }
        // Every path below reads the right half through this.
        wide_resolve_source_x_offset_tiles_ = wide_key.GetPitchTiles();
        wide_awaiting_second_resolve_.erase(awaited);
        COUNT_profile_add("gpu/ac6_wide_second_tile_resolves", 1);
      } else if (awaited != wide_awaiting_second_resolve_.end()) {
        // The span was resolved again after more draws, before any tile-1
        // draw: the guest resolves tile 0 more than once per pass (the intro
        // cutscene renders the world, resolves, draws effects over it,
        // resolves again, then does the same for tile 1). The right half now
        // holds both sub-passes, so it cannot stand in for tile 1's first
        // resolve; tile 1 renders. This resolve reads the left half as the
        // first did, and the entries stay for tile 1's resolves to consume.
        wide_second_tile_renders_ = true;
        COUNT_profile_add("gpu/ac6_wide_second_tile_rendered_passes", 1);
        COUNT_profile_add("gpu/ac6_wide_repeated_first_tile_resolves", 1);
      } else {
        wide_awaiting_second_resolve_[dump_base] = wide;
        COUNT_profile_add("gpu/ac6_wide_first_tile_resolves", 1);
      }
      draws = 0;
    }
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(draw_resolution_scale_x(), draw_resolution_scale_y(),
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];
      bool direct_resolved = false;
      bool copy_to_texture = false;
      bool compute_to_texture = false;
      if (GetPath() == Path::kHostRenderTargets) {
        // Two ways into the destination texture's image: an image copy, which
        // needs bitwise-equivalent formats and a single-sampled owner, and the
        // compute shader, which samples and repacks and so also covers depth
        // resolves and multisampled views. Whichever doesn't claim the resolve
        // leaves it to the other; what neither claims takes the tiled round
        // trip (dump, copy shader, then untiling at the consuming draw).
        if (REXCVAR_GET(vulkan_resolve_to_texture_image)) {
          bool compute_enabled = REXCVAR_GET(vulkan_resolve_to_texture_compute);
          bool prefer_compute =
              compute_enabled && REXCVAR_GET(vulkan_resolve_to_texture_prefer_compute);
          auto try_compute = [&]() {
            compute_to_texture = TryPrepareResolveComputeToTexture(
                resolve_info, copy_shader, copy_shader_constants, texture_cache,
                draw_resolution_scaled);
            if (!(resolve_compute_attempt_count_ & UINT64_C(2047))) {
              std::string rejects;
              for (const auto& entry : resolve_compute_rejects_) {
                rejects += fmt::format(" [{}: {}]", entry.first, entry.second);
              }
              REXGPU_ERROR("VulkanRenderTargetCache: resolve compute to texture images: {} of {} "
                           "attempts;{}",
                           resolve_compute_count_, resolve_compute_attempt_count_,
                           rejects.empty() ? " no rejections" : rejects.c_str());
            }
            return compute_to_texture;
          };
          auto try_copy = [&]() {
            copy_to_texture =
                TryPrepareResolveCopyToTexture(resolve_info, texture_cache, draw_resolution_scaled);
            if (!(resolve_copy_to_texture_attempt_count_ & UINT64_C(2047))) {
              LogResolveCopyToTextureStats();
            }
            return copy_to_texture;
          };
          if (prefer_compute) {
            if (!try_compute()) {
              try_copy();
            }
          } else {
            if (!try_copy() && compute_enabled) {
              try_compute();
            }
          }
          copy_to_texture = copy_to_texture || compute_to_texture;
        }
        if (!copy_to_texture && REXCVAR_GET(direct_host_resolve)) {
          direct_resolved =
              TryResolveCopyDirectly(resolve_info, copy_shader, draw_resolution_scaled);
          if (direct_resolved) {
            ++direct_resolve_success_count_;
          } else {
            ++direct_resolve_fallback_count_;
          }
          // One greppable line the first time the fused path runs, then a
          // periodic tally - the fallback share is what says whether a title
          // actually benefits.
          if (direct_resolved && direct_resolve_success_count_ == 1) {
            // Error level so the line survives a performance-mode log level,
            // like the other support lines - this is the one that says which
            // resolve path a session actually ran.
            REXGPU_ERROR("VulkanRenderTargetCache: resolving directly from host render targets");
          }
          if (!(direct_resolve_attempt_count_ & UINT64_C(2047))) {
            REXGPU_INFO(
                "VulkanRenderTargetCache: direct resolves: {} of {} attempts, {} fell back to "
                "the EDRAM dump",
                direct_resolve_success_count_, direct_resolve_attempt_count_,
                direct_resolve_fallback_count_);
          }
        }
        if (!copy_to_texture && !direct_resolved) {
          // Dump the current contents of the render targets owning the affected
          // range to edram_buffer_.
          uint32_t dump_base;
          uint32_t dump_row_length_used;
          uint32_t dump_rows;
          uint32_t dump_pitch;
          resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
          if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch)) {
            REXGPU_ERROR("VulkanRenderTargetCache: Failed to dump host render targets for resolve");
            return false;
          }
        }
      }

      uint32_t copy_dest_range_unscaled = resolve_info.copy_dest_extent_start -
                                          resolve_info.copy_dest_base +
                                          resolve_info.copy_dest_extent_length;
      uint64_t copy_dest_base = resolve_info.copy_dest_base;
      uint64_t copy_dest_range_length = copy_dest_range_unscaled;
      uint64_t copy_dest_use_start = resolve_info.copy_dest_extent_start;
      uint64_t copy_dest_use_length = resolve_info.copy_dest_extent_length;
      if (draw_resolution_scaled) {
        if (!texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_base, copy_dest_range_unscaled,
                copy_shader_info.dest_bpe_log2, copy_dest_base, copy_dest_range_length) ||
            !texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                copy_shader_info.dest_bpe_log2, copy_dest_use_start, copy_dest_use_length)) {
          REXGPU_ERROR(
              "VulkanRenderTargetCache: Failed to map scaled resolve "
              "destination range (base={:08X}, length={:08X})",
              resolve_info.copy_dest_base, copy_dest_range_unscaled);
          return false;
        }
      }

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        copy_dest_committed = texture_cache.CommitScaledResolveRange(
            resolve_info.copy_dest_base, copy_dest_range_unscaled, copy_shader_info.dest_bpe_log2);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (!copy_dest_committed) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      } else if (copy_to_texture) {
        // Fires the destination texture's watch (and marks the pages as
        // scaled-resolved); IssueResolveCopy declares it current again after
        // the copy, so the order matters.
        texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                          resolve_info.copy_dest_extent_length);
        if (compute_to_texture) {
          IssueResolveComputeToTexture(texture_cache);
        } else {
          IssueResolveCopyToTexture(texture_cache);
        }
        written_address_out = resolve_info.copy_dest_extent_start;
        written_length_out = resolve_info.copy_dest_extent_length;
        copied = true;
      } else {
        // TODO(Triang3l): Switching between descriptors if exceeding
        // maxStorageBufferRange.
        // TODO(Triang3l): Use a single 512 MB shared memory binding if
        // possible.
        VkDescriptorSet descriptor_set_dest = command_processor_.AllocateSingleTransientDescriptor(
            VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          // Write the destination descriptor.
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest_buffer_info.buffer = draw_resolution_scaled
                                                             ? texture_cache.scaled_resolve_buffer()
                                                             : shared_memory.buffer();
          write_descriptor_set_dest_buffer_info.offset = copy_dest_base;
          write_descriptor_set_dest_buffer_info.range = copy_dest_range_length;
          VkWriteDescriptorSet write_descriptor_set_dest;
          write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write_descriptor_set_dest.pNext = nullptr;
          write_descriptor_set_dest.dstSet = descriptor_set_dest;
          write_descriptor_set_dest.dstBinding = 0;
          write_descriptor_set_dest.dstArrayElement = 0;
          write_descriptor_set_dest.descriptorCount = 1;
          write_descriptor_set_dest.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          write_descriptor_set_dest.pImageInfo = nullptr;
          write_descriptor_set_dest.pBufferInfo = &write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0, nullptr);

          // Submit the resolve.
          if (draw_resolution_scaled) {
            texture_cache.UseScaledResolveBufferForWrite(copy_dest_use_start, copy_dest_use_length);
          } else {
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<uint32_t, uint32_t>(uint32_t(copy_dest_use_start),
                                                            uint32_t(copy_dest_use_length)));
          }
          if (direct_resolved) {
            // The render targets go straight to the destination - no dump to
            // the EDRAM buffer, and no copy pass reading it back.
            IssueDirectResolveCopy(resolve_info, copy_shader, copy_shader_constants,
                                   descriptor_set_dest, draw_resolution_scaled,
                                   uint32_t(write_descriptor_set_dest_buffer_info.offset));
          } else {
            UseEdramBuffer(EdramBufferUsage::kComputeRead);
            command_processor_.BindExternalComputePipeline(
                resolve_copy_pipelines_[size_t(copy_shader)]);
            VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
            descriptor_sets[kResolveCopyDescriptorSetEdram] = edram_storage_buffer_descriptor_set_;
            descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
            command_buffer.CmdVkBindDescriptorSets(
                VK_PIPELINE_BIND_POINT_COMPUTE, resolve_copy_pipeline_layout_, 0,
                uint32_t(rex::countof(descriptor_sets)), descriptor_sets, 0, nullptr);
            if (draw_resolution_scaled) {
              command_buffer.CmdVkPushConstants(resolve_copy_pipeline_layout_,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                sizeof(copy_shader_constants.dest_relative),
                                                &copy_shader_constants.dest_relative);
            } else {
              // TODO(Triang3l): Proper dest_base in case of one 512 MB shared
              // memory binding, or multiple shared memory bindings in case of
              // splitting due to maxStorageBufferRange overflow.
              copy_shader_constants.dest_base -=
                  uint32_t(write_descriptor_set_dest_buffer_info.offset);
              command_buffer.CmdVkPushConstants(
                  resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                  sizeof(copy_shader_constants), &copy_shader_constants);
            }
            command_processor_.SubmitBarriers(true);
            command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y, 1);
          }

          // Invalidate textures and mark the range as scaled if needed.
          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
        }
      }
    }
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                                 clear_render_targets[0], clear_transfers_[0],
                                                 clear_render_targets[1], clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] =
              resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Should be safe to only commit once (if was accessed as unordered or
        // with fragment shader interlock previously - if there was nothing to
        // copy, only to clear, for some reason, for instance), overlap of the
        // depth and the color ranges is highly unlikely.
        CommitEdramBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                               resolve_fsi_clear_pipeline_layout_, 0, 1,
                                               &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<uint32_t, uint32_t> clear_group_count = resolve_info.GetClearShaderGroupCount(
            draw_resolution_scale_x(), draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_buffer.CmdVkPushConstants(resolve_fsi_clear_pipeline_layout_,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                            sizeof(depth_clear_constants), &depth_clear_constants);
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        if (clear_color) {
          command_processor_.BindExternalComputePipeline(
              resolve_info.color_edram_info.format_is_64bpp ? resolve_fsi_clear_64bpp_pipeline_
                                                            : resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                uint32_t(offsetof(draw_util::ResolveClearShaderConstants, rt_specific)),
                sizeof(color_clear_constants.rt_specific), &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

bool VulkanRenderTargetCache::Update(bool is_rasterization_done,
                                     reg::RB_DEPTHCONTROL normalized_depth_control,
                                     uint32_t normalized_color_mask, const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }

  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed even with the fragment shader interlock render backend for passing
  // the sample count to the pipeline cache.
  render_pass_key.msaa_samples = rb_surface_info.msaa_samples;

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();

      if (REXCVAR_GET(ac6_edram_no_transfers)) {
        // Experiment: ownership changes, contents do not follow. Each host
        // render target keeps its own pixels, as on a PC; a target bound over
        // another's EDRAM range starts with whatever it held last time it was
        // bound. Aliases the game relies on WITHOUT resolving (reading depth,
        // stencil or colour written under a different key) break here, and
        // that is the point: the skipped-transfer log below names them.
        static std::mutex hist_mutex;
        static std::unordered_map<std::string, uint32_t> hist;
        static auto last = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(hist_mutex);
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          const RenderTarget* dest = depth_and_color_render_targets[i];
          const std::vector<Transfer>& transfers = last_update_transfers()[i];
          if (!dest || transfers.empty()) {
            continue;
          }
          COUNT_profile_add("gpu/edram_transfers_skipped", int64_t(transfers.size()));
          const RenderTargetKey dk = dest->key();
          for (const Transfer& t : transfers) {
            const RenderTargetKey sk = t.source->key();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s %u/%u/%ux -> %s %u/%u/%ux",
                          sk.is_depth ? "depth" : "color", uint32_t(sk.base_tiles),
                          uint32_t(sk.GetPitchTiles()), 1u << uint32_t(sk.msaa_samples),
                          dk.is_depth ? "depth" : "color", uint32_t(dk.base_tiles),
                          uint32_t(dk.GetPitchTiles()), 1u << uint32_t(dk.msaa_samples));
            ++hist[buf];
          }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(3) && !hist.empty()) {
          last = now;
          std::vector<std::pair<uint32_t, std::string>> rows;
          for (auto& kv : hist) rows.emplace_back(kv.second, kv.first);
          std::sort(rows.rbegin(), rows.rend());
          REXGPU_ERROR("[EDRAM-SKIP] transfers skipped in the last 3s, by pattern:");
          for (size_t i = 0; i < rows.size() && i < 12; ++i) {
            REXGPU_ERROR("[EDRAM-SKIP]   {:6}  {}", rows[i].first, rows[i].second);
          }
          hist.clear();
        }
        // Resolve clears are real clears and must still happen.
        std::vector<Transfer> none[1 + xenos::kMaxColorRenderTargets];
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets, none);
      } else {
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets, last_update_transfers());
      }

      // Between a wide target's two tile resolves, a draw into another target
      // that has taken over its span changes what the second tile starts
      // from - the hangar paints last frame's right half into the range
      // through a 1x alias right after resolving the first tile. Only one
      // render for both tiles is then wrong; the second tile is rendered.
      // Cheap: the map is non-empty only inside that window.
      if (!wide_awaiting_second_resolve_.empty() && !wide_second_tile_renders_) {
        for (auto it = wide_awaiting_second_resolve_.begin();
             it != wide_awaiting_second_resolve_.end();) {
          RenderTarget* wide = it->second;
          const RenderTargetKey wide_key = wide->key();
          const uint32_t span_tiles = wide_key.GetPitchTiles() * 1;
          GetResolveCopyRectanglesToDump(it->first, span_tiles, 1, span_tiles, dump_rectangles_);
          RenderTarget* span_owner =
              dump_rectangles_.size() == 1 ? dump_rectangles_[0].render_target : nullptr;
          bool drawn_into_by_other = false;
          if (span_owner && span_owner != wide) {
            for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
              drawn_into_by_other |= depth_and_color_render_targets[j] == span_owner;
            }
          }
          if (drawn_into_by_other) {
            // The entries stay: the second tile's own draws rebind the wide
            // target, and an awaiting entry is what tells that rebind apart
            // from a clean pass start (which would clear this flag). The
            // tile's resolves consume them.
            wide_second_tile_renders_ = true;
            COUNT_profile_add("gpu/ac6_wide_second_tile_rendered_passes", 1);
            if (WideLogTake()) {
              REXGPU_ERROR("[WIDE] span {} drawn into by another target between tile resolves - "
                           "second tile renders", it->first);
            }
          }
          ++it;
        }
      }

      // A wide target that was not bound by the previous update starts a pass:
      // its left half now holds what the guest's clear and transfers put in
      // EDRAM, and the right half must start from the same state.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        RenderTarget* wide = rt && IsWideKey(rt->key()) ? rt : nullptr;
        if (wide && wide != wide_bound_last_update_[i]) {
          if (WideLogTake()) {
            size_t awaiting_count = 0;
            for (const auto& kv : wide_awaiting_second_resolve_) {
              awaiting_count += kv.second == wide ? 1 : 0;
            }
            REXGPU_ERROR("[WIDE] rebind slot {} {} awaiting {} transfers {} second_renders {}", i,
                         wide->key().is_depth ? "depth" : "color", awaiting_count,
                         last_update_transfers()[i].size(), wide_second_tile_renders_ ? 1 : 0);
          }
          bool awaiting = false;
          for (const auto& kv : wide_awaiting_second_resolve_) {
            awaiting |= kv.second == wide;
          }
          if (awaiting) {
            // Rebound between its two tile resolves: either the second tile
            // is being rendered (the span check above), or, if contents were
            // transferred in, the second tile starts from something the first
            // did not and must be. Either way not a pass start.
            if (!last_update_transfers()[i].empty() && !wide_second_tile_renders_) {
              wide_second_tile_renders_ = true;
              COUNT_profile_add("gpu/ac6_wide_second_tile_rendered_passes", 1);
            }
          } else {
            // A clean pass start; the transfers that just ran covered both
            // halves.
            wide_draws_since_resolve_[wide] = 0;
            wide_second_tile_renders_ = false;
            COUNT_profile_add("gpu/ac6_wide_pass_starts", 1);
          }
        }
        wide_bound_last_update_[i] = wide;
      }

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format = depth_and_color_render_targets[0]->key().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->key().GetColorFormat();
      }

      const Framebuffer* framebuffer = last_update_framebuffer_;
      VkRenderPass render_pass = last_update_render_pass_key_ == render_pass_key
                                     ? last_update_render_pass_
                                     : VK_NULL_HANDLE;
      if (render_pass == VK_NULL_HANDLE) {
        render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
        if (render_pass == VK_NULL_HANDLE) {
          return false;
        }
        // Framebuffer for a different render pass needed now.
        framebuffer = nullptr;
      }

      uint32_t pitch_tiles_at_32bpp =
          ((rb_surface_info.surface_pitch
            << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
           (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ != pitch_tiles_at_32bpp ||
            std::memcmp(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                        sizeof(last_update_framebuffer_attachments_))) {
          framebuffer = nullptr;
        }
      }
      if (!framebuffer) {
        framebuffer = GetHostRenderTargetsFramebuffer(render_pass_key, pitch_tiles_at_32bpp,
                                                      depth_and_color_render_targets);
        if (!framebuffer) {
          return false;
        }
      }

      // Successful update - write the new configuration.
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::memcpy(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                  sizeof(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      // Transition the used render targets.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        if (!rt) {
          continue;
        }
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rt);
        VkPipelineStageFlags rt_dst_stage_mask;
        VkAccessFlags rt_dst_access_mask;
        VkImageLayout rt_new_layout;
        VulkanRenderTarget::GetDrawUsage(i == 0, &rt_dst_stage_mask, &rt_dst_access_mask,
                                         &rt_new_layout);
        command_processor_.PushImageMemoryBarrier(
            vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.current_stage_mask(), rt_dst_stage_mask, vulkan_rt.current_access_mask(),
            rt_dst_access_mask, vulkan_rt.current_layout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask, rt_new_layout);
      }
    } break;

    case Path::kPixelShaderInterlock: {
      // For FSI, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for FSI drawing now.
      UseEdramBuffer(EdramBufferUsage::kFragmentReadWrite);
      // Commit preceding unordered (but not FSI) writes like clears as they
      // aren't synchronized with FSI accesses.
      CommitEdramBufferShaderWrites(EdramBufferModificationStatus::kViaUnordered);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      assert_unhandled_case(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments, uint32_t* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  RenderTarget* const* rts = last_update_accumulated_render_targets();

  std::memset(depth_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  std::memset(stencil_attachment, 0, sizeof(VkRenderingAttachmentInfo));

  if ((key.depth_and_color_used & 0b1) && rts[0]) {
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment->pNext = nullptr;
    depth_attachment->imageView = vulkan_rt->view_depth_stencil();
    depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
    depth_attachment->resolveMode = VK_RESOLVE_MODE_NONE;
    depth_attachment->resolveImageView = VK_NULL_HANDLE;
    depth_attachment->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment->clearValue = {};
    *stencil_attachment = *depth_attachment;
  }

  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    std::memset(&color_attachment, 0, sizeof(VkRenderingAttachmentInfo));
    if ((key.depth_and_color_used & (1 << (1 + i))) && rts[1 + i]) {
      const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[1 + i]);
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = vulkan_rt->view_depth_color();
      color_attachment.imageLayout = VulkanRenderTarget::kColorDrawLayout;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
      color_attachment_count = i + 1;
    }
  }
  *color_attachment_count_out = color_attachment_count;
}

VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(RenderPassKey key) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MsaaSamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MsaaSamples::k2X:
      samples = IsMsaa2xSupported(key.depth_and_color_used != 0) ? VK_SAMPLE_COUNT_2_BIT
                                                                 : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MsaaSamples::k4X:
      samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      return VK_NULL_HANDLE;
  }

  VkAttachmentDescription attachments[1 + xenos::kMaxColorRenderTargets];
  if (key.depth_and_color_used & 0b1) {
    VkAttachmentDescription& attachment = attachments[0];
    attachment.flags = 0;
    attachment.format = GetDepthVulkanFormat(key.depth_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VulkanRenderTarget::kDepthDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kDepthDrawLayout;
  }
  VkAttachmentReference color_attachments[xenos::kMaxColorRenderTargets];
  xenos::ColorRenderTargetFormat color_formats[] = {
      key.color_0_view_format,
      key.color_1_view_format,
      key.color_2_view_format,
      key.color_3_view_format,
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    uint32_t attachment_bit = uint32_t(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    uint32_t attachment_index = rex::bit_count(key.depth_and_color_used & (attachment_bit - 1));
    color_attachment.attachment = attachment_index;
    VkAttachmentDescription& attachment = attachments[attachment_index];
    attachment.flags = 0;
    xenos::ColorRenderTargetFormat color_format = color_formats[i];
    attachment.format = key.color_rts_use_transfer_formats
                            ? GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples)
                            : GetColorVulkanFormat(color_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VulkanRenderTarget::kColorDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kColorDrawLayout;
  }

  VkAttachmentReference depth_stencil_attachment;
  depth_stencil_attachment.attachment = (key.depth_and_color_used & 0b1) ? 0 : VK_ATTACHMENT_UNUSED;
  depth_stencil_attachment.layout = VulkanRenderTarget::kDepthDrawLayout;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount = 32 - rex::lzcnt(uint32_t(key.depth_and_color_used >> 1));
  subpass.pColorAttachments = color_attachments;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment =
      (key.depth_and_color_used & 0b1) ? &depth_stencil_attachment : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkPipelineStageFlags dependency_stage_mask = 0;
  VkAccessFlags dependency_access_mask = 0;
  if (key.depth_and_color_used & 0b1) {
    dependency_stage_mask |= VulkanRenderTarget::kDepthDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kDepthDrawAccessMask;
  }
  if (key.depth_and_color_used >> 1) {
    dependency_stage_mask |= VulkanRenderTarget::kColorDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kColorDrawAccessMask;
  }
  VkSubpassDependency subpass_dependencies[2];
  subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[0].dstSubpass = 0;
  subpass_dependencies[0].srcStageMask = dependency_stage_mask;
  subpass_dependencies[0].dstStageMask = dependency_stage_mask;
  subpass_dependencies[0].srcAccessMask = dependency_access_mask;
  subpass_dependencies[0].dstAccessMask = dependency_access_mask;
  subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  subpass_dependencies[1].srcSubpass = 0;
  subpass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[1].srcStageMask = dependency_stage_mask;
  subpass_dependencies[1].dstStageMask = dependency_stage_mask;
  subpass_dependencies[1].srcAccessMask = dependency_access_mask;
  subpass_dependencies[1].dstAccessMask = dependency_access_mask;
  subpass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  VkRenderPassCreateInfo render_pass_create_info;
  render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;
  render_pass_create_info.attachmentCount = rex::bit_count(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? uint32_t(rex::countof(subpass_dependencies)) : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 && depth_unorm24_vulkan_format_supported()) {
    return VK_FORMAT_D24_UNORM_S8_UINT;
  }
  return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool VulkanRenderTargetCache::IsColor16FormatFloatLike(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return true;
    default:
      return false;
  }
}

VkFormat VulkanRenderTargetCache::GetColorVulkanFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16_SFLOAT
                                                       : VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                         : VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      assert_unhandled_case(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, xenos::MsaaSamples msaa_samples,
    bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  VkSampleCountFlagBits transfer_sample_count =
      VkSampleCountFlagBits(uint32_t(1) << uint32_t(msaa_samples));
  if (transfer_sample_count == VK_SAMPLE_COUNT_2_BIT && !msaa_2x_attachments_supported_) {
    // 2x guest color render targets are allocated as 4x host render targets in
    // this mode.
    transfer_sample_count = VK_SAMPLE_COUNT_4_BIT;
  }
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  bool integer_transfer_sample_count_supported =
      (device_properties.framebufferColorSampleCounts & transfer_sample_count) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & transfer_sample_count) != 0;
  // Floating-point numbers have NaNs that need to be propagated without
  // modifications to the bit representation, and SNORM has two representations
  // of -1.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16B16A16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32G32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
  }
}

VulkanRenderTargetCache::VulkanRenderTarget::~VulkanRenderTarget() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                     : *render_target_cache_.descriptor_set_pool_sampled_image_;
  descriptor_set_pool.Free(descriptor_set_index_transfer_source_);
  if (view_color_transfer_separate_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_color_transfer_separate_, nullptr);
  }
  if (view_srgb_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_srgb_, nullptr);
  }
  if (view_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_stencil_, nullptr);
  }
  if (view_depth_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_depth_stencil_, nullptr);
  }
  dfn.vkDestroyImageView(device, view_depth_color_, nullptr);
  dfn.vkDestroyImage(device, image_, nullptr);
  dfn.vkFreeMemory(device, memory_, nullptr);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image.

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width =
      key.GetWidth() * draw_resolution_scale_x() * (IsWideKey(key) ? 2 : 1);
  image_create_info.extent.height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) * draw_resolution_scale_y();
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples = VkSampleCountFlagBits(uint32_t(1) << uint32_t(key.msaa_samples));
  }
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat transfer_format;
  bool is_srgb_view_needed = false;
  if (key.is_depth) {
    image_create_info.format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = image_create_info.format;
    // Transfer destination for the stencil transfer's buffer-to-image copy.
    image_create_info.usage |=
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  } else {
    xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
    image_create_info.format = GetColorVulkanFormat(color_format);
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples);
    is_srgb_view_needed = false;
    if (image_create_info.format != transfer_format || is_srgb_view_needed) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    // Source of the render-target-as-texture resolve copy.
    image_create_info.usage |=
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    REXGPU_ERROR("VulkanRenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", static_cast<uint32_t>(key.resource_format));
    return nullptr;
  }
  VkImage image;
  VkDeviceMemory memory;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
          memory)) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
        "image",
        image_create_info.extent.width, image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return nullptr;
  }

  // Create the image views.

  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = image_create_info.format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_color) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
        "render target",
        key.is_depth ? "depth" : "color", image_create_info.extent.width,
        image_create_info.extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
        key.GetFormatName());
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkImageView view_depth_stencil = VK_NULL_HANDLE;
  VkImageView view_stencil = VK_NULL_HANDLE;
  VkImageView view_srgb = VK_NULL_HANDLE;
  VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
  if (key.is_depth) {
    view_create_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_stencil) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a depth / stencil view for a "
          "{}x{} {}xMSAA {} render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_stencil) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
          "{}xMSAA render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_stencil, nullptr);
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
  } else {
    if (is_srgb_view_needed) {
      view_create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_srgb) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create an sRGB view for a {}x{} "
            "{}xMSAA render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples),
            xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
    if (transfer_format != image_create_info.format) {
      view_create_info.format = transfer_format;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                                &view_color_transfer_separate) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
            "{}xMSAA {} render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
        if (view_srgb != VK_NULL_HANDLE) {
          dfn.vkDestroyImageView(device, view_srgb, nullptr);
        }
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
  }

  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_ : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to allocate sampled image descriptors "
        "for a {} render target",
        key.is_depth ? "depth/stencil" : "color");
    if (view_color_transfer_separate != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_color_transfer_separate, nullptr);
    }
    if (view_srgb != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_srgb, nullptr);
    }
    dfn.vkDestroyImageView(device, view_depth_color, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkDescriptorSet descriptor_set_transfer_source =
      descriptor_set_pool.Get(descriptor_set_index_transfer_source);
  VkWriteDescriptorSet descriptor_set_write[2];
  VkDescriptorImageInfo descriptor_set_write_depth_color;
  descriptor_set_write_depth_color.sampler = VK_NULL_HANDLE;
  descriptor_set_write_depth_color.imageView = view_color_transfer_separate != VK_NULL_HANDLE
                                                   ? view_color_transfer_separate
                                                   : view_depth_color;
  descriptor_set_write_depth_color.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  descriptor_set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_set_write[0].pNext = nullptr;
  descriptor_set_write[0].dstSet = descriptor_set_transfer_source;
  descriptor_set_write[0].dstBinding = 0;
  descriptor_set_write[0].dstArrayElement = 0;
  descriptor_set_write[0].descriptorCount = 1;
  descriptor_set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_write[0].pImageInfo = &descriptor_set_write_depth_color;
  descriptor_set_write[0].pBufferInfo = nullptr;
  descriptor_set_write[0].pTexelBufferView = nullptr;
  VkDescriptorImageInfo descriptor_set_write_stencil;
  if (key.is_depth) {
    descriptor_set_write_stencil.sampler = VK_NULL_HANDLE;
    descriptor_set_write_stencil.imageView = view_stencil;
    descriptor_set_write_stencil.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write[1].pNext = nullptr;
    descriptor_set_write[1].dstSet = descriptor_set_transfer_source;
    descriptor_set_write[1].dstBinding = 1;
    descriptor_set_write[1].dstArrayElement = 0;
    descriptor_set_write[1].descriptorCount = 1;
    descriptor_set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write[1].pImageInfo = &descriptor_set_write_stencil;
    descriptor_set_write[1].pBufferInfo = nullptr;
    descriptor_set_write[1].pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, key.is_depth ? 2 : 1, descriptor_set_write, 0, nullptr);

  return new VulkanRenderTarget(key, *this, image, memory, view_depth_color, view_depth_stencil,
                                view_stencil, view_srgb, view_color_transfer_separate,
                                descriptor_set_index_transfer_source);
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !depth_unorm24_vulkan_format_supported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  // Keep parity with D3D12 ROV interlock barrier requests by committing any
  // pending EDRAM shader writes, not only FSI writes.
  CommitEdramBufferShaderWrites();
}

void VulkanRenderTargetCache::GetEdramBufferUsageMasks(EdramBufferUsage usage,
                                                       VkPipelineStageFlags& stage_mask_out,
                                                       VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EdramBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EdramBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanRenderTargetCache::UseEdramBuffer(EdramBufferUsage new_usage) {
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, src_stage_mask, src_access_mask);
  GetEdramBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask,
                                                 dst_stage_mask, src_access_mask,
                                                 dst_access_mask)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted.
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status != EdramBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EdramBufferUsage::kFragmentReadWrite:
      // max because being modified via unordered access requires stricter
      // synchronization than via fragment shader interlocks.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EdramBufferUsage::kComputeWrite:
      assert_true(modification_status == EdramBufferModificationStatus::kViaUnordered);
      modification_status = EdramBufferModificationStatus::kViaUnordered;
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    default:
      assert_always(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEdramBufferShaderWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  if (!(access_mask & VK_ACCESS_SHADER_WRITE_BIT)) {
    // Keep behavior robust similarly to D3D12 when an unexpected state is
    // encountered: avoid emitting an invalid barrier but still treat writes as
    // committed for ownership tracking.
    assert_always("EDRAM writes committed from a non-shader-write usage");
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask, access_mask,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = render_pass_key;
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles = depth_and_color_render_targets[1]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles = depth_and_color_render_targets[2]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles = depth_and_color_render_targets[3]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles = depth_and_color_render_targets[4]->key().base_tiles;
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return &it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  uint32_t rt_index;
  while (rex::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(uint32_t(1) << rt_index);
    const auto& vulkan_rt =
        *static_cast<const VulkanRenderTarget*>(depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      attachment = render_pass_key.color_rts_use_transfer_formats ? vulkan_rt.view_color_transfer()
                                                                  : vulkan_rt.view_depth_color();
    } else {
      attachment = vulkan_rt.view_depth_stencil();
    }
    attachments[attachment_count++] = attachment;
  }

  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = render_pass;
  framebuffer_create_info.attachmentCount = attachment_count;
  framebuffer_create_info.pAttachments = attachments;
  VkExtent2D host_extent;
  if (pitch_tiles_at_32bpp) {
    host_extent.width =
        RenderTargetKey::GetWidth(pitch_tiles_at_32bpp, render_pass_key.msaa_samples);
    {
      // A pass on wide targets (all of a pass's targets share pitch and MSAA,
      // so they are all wide or none) draws into the wide framebuffer.
      RenderTargetKey probe;
      probe.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
      probe.msaa_samples = render_pass_key.msaa_samples;
      if (IsWideKey(probe)) {
        host_extent.width *= 2;
      }
    }
    host_extent.height = GetRenderTargetHeight(pitch_tiles_at_32bpp, render_pass_key.msaa_samples);
  } else {
    assert_zero(render_pass_key.depth_and_color_used);
    // Still needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Limiting to the device limit for the case of no attachments, for which
  // there's no limit imposed by the sizes of the attachments that have been
  // created successfully.
  host_extent.width = std::min(host_extent.width * draw_resolution_scale_x(),
                               device_properties.maxFramebufferWidth);
  host_extent.height = std::min(host_extent.height * draw_resolution_scale_y(),
                                device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return nullptr;
  }
  // Creates at a persistent location - safe to use pointers.
  return &framebuffers_
              .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                       std::forward_as_tuple(framebuffer, host_extent))
              .first->second;
}

VkShaderModule VulkanRenderTargetCache::GetTransferShader(TransferShaderKey key) {
  auto shader_it = transfer_shaders_.find(key);
  if (shader_it != transfer_shaders_.end()) {
    return shader_it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];
  // Compute variant of the stencil bit transfer: set 0 is the output buffer
  // and the source sets follow; the destination pixel comes from the
  // invocation id; the stencil byte goes to the buffer instead of a kill.
  const bool stencil_compute = key.stencil_compute != 0;
  assert_true(!stencil_compute || mode.output == TransferOutput::kStencilBit);
  const uint32_t source_descriptor_set_base = stencil_compute ? 1 : 0;

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color =
      (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferVulkanFormat(source_color_format, key.source_msaa_samples,
                                          &source_color_is_uint);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->extensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      GetColorOwnershipTransferVulkanFormat(dest_color_format, key.dest_msaa_samples,
                                            &dest_color_is_uint);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component = dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component, dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
                                 "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation, key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn, spv::BuiltInFragDepth);
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref = builder.createVariable(
            spv::NoPrecision, spv::StorageClassOutput, type_int, "gl_FragStencilRefARB");
        builder.addDecoration(output_fragment_stencil_ref, spv::DecorationBuiltIn,
                              spv::BuiltInFragStencilRefEXT);
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled = key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false, false,
                              source_is_multisampled, 1, spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(source_color_texture, spv::DecorationDescriptorSet,
                          source_descriptor_set_base +
                              rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                             (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        source_descriptor_set_base +
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth || shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer, spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(host_depth_source_buffer, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false,
                              key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  // Stencil compute output buffer and push constants (StencilComputePushConstants).
  spv::Id stencil_compute_output_buffer = spv::NoResult;
  spv::Id stencil_compute_push_constants = spv::NoResult;
  if (stencil_compute) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(uint32_t));
    spv::Id type_stencil_output_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferStencilOutputBuffer");
    builder.addMemberName(type_stencil_output_buffer, 0, "stencil");
    builder.addMemberDecoration(type_stencil_output_buffer, 0, spv::DecorationOffset, 0);
    builder.addDecoration(type_stencil_output_buffer, spv::DecorationBufferBlock);
    stencil_compute_output_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_stencil_output_buffer, "xe_transfer_stencil_output");
    builder.addDecoration(stencil_compute_output_buffer, spv::DecorationDescriptorSet, 0);
    builder.addDecoration(stencil_compute_output_buffer, spv::DecorationBinding, 0);

    id_vector_temp.clear();
    for (uint32_t i = 0; i < 7; ++i) {
      id_vector_temp.push_back(type_uint);
    }
    spv::Id type_stencil_push_constants =
        builder.makeStructType(id_vector_temp, "XeTransferStencilComputePushConstants");
    static const char* const kMemberNames[] = {"address",     "rect_x",    "rect_y",
                                               "rect_width",  "rect_height", "row_pitch",
                                               "buffer_offset"};
    for (uint32_t i = 0; i < 7; ++i) {
      builder.addMemberName(type_stencil_push_constants, i, kMemberNames[i]);
      builder.addMemberDecoration(type_stencil_push_constants, i, spv::DecorationOffset,
                                  sizeof(uint32_t) * i);
    }
    builder.addDecoration(type_stencil_push_constants, spv::DecorationBlock);
    stencil_compute_push_constants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                               type_stencil_push_constants, "xe_transfer_push_constants");
    id_vector_temp.clear();
  }
  if (!stencil_compute && pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (!stencil_compute &&
      pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (!stencil_compute && pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (stencil_compute) {
    // The address constant is member 0 of the compute push constants.
    push_constants = stencil_compute_push_constants;
    push_constants_member_address = 0;
  } else if (!id_vector_temp.empty()) {
    spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address, spv::DecorationOffset,
          sizeof(uint32_t) *
              rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                             (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address, "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_stencil_mask,
                            "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                                            type_push_constants, "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord = spv::NoResult;
  spv::Id input_global_invocation_id = spv::NoResult;
  spv::Id input_local_invocation_index = spv::NoResult;
  spv::Id stencil_compute_tile = spv::NoResult;
  if (stencil_compute) {
    input_global_invocation_id =
        builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                               builder.makeVectorType(type_uint, 3), "gl_GlobalInvocationID");
    builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                          spv::BuiltInGlobalInvocationId);
    main_interface.push_back(input_global_invocation_id);
    input_local_invocation_index = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput, type_uint, "gl_LocalInvocationIndex");
    builder.addDecoration(input_local_invocation_index, spv::DecorationBuiltIn,
                          spv::BuiltInLocalInvocationIndex);
    main_interface.push_back(input_local_invocation_index);
    // The workgroup's 8x8 stencil bytes as 16 dwords, assembled with
    // workgroup-local atomics and stored as whole dwords - no global atomics
    // and no need to zero the output buffer.
    static_assert(kStencilComputeGroupSizeX == 8 && kStencilComputeGroupSizeY == 8);
    stencil_compute_tile = builder.createVariable(
        spv::NoPrecision, spv::StorageClassWorkgroup,
        builder.makeArrayType(type_uint, builder.makeUintConstant(16), 0), "xe_stencil_tile");
  } else {
    input_fragment_coord = builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                  type_float4, "gl_FragCoord");
    builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
    main_interface.push_back(input_fragment_coord);
  }
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  assert_true(!stencil_compute || key.dest_msaa_samples == xenos::MsaaSamples::k1X);
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id =
          builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn, spv::BuiltInSampleId);
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord;
  // Compute: the invocation within the rectangle, and its byte index in the
  // output buffer (row-major at the row pitch).
  spv::Id stencil_compute_rect_coord = spv::NoResult;
  std::unique_ptr<SpirvBuilder::IfBuilder> stencil_compute_in_rect_if;
  auto load_stencil_compute_push_constant = [&](uint32_t member) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(member)));
    return builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                        stencil_compute_push_constants,
                                                        id_vector_temp),
                              spv::NoPrecision);
  };
  if (stencil_compute) {
    stencil_compute_rect_coord = builder.createRvalueSwizzle(
        spv::NoPrecision, type_uint2,
        builder.createLoad(input_global_invocation_id, spv::NoPrecision), uint_vector_temp);
    spv::Id rect_x = builder.createCompositeExtract(stencil_compute_rect_coord, type_uint, 0);
    spv::Id rect_y = builder.createCompositeExtract(stencil_compute_rect_coord, type_uint, 1);
    // Zero the tile (lanes 0-15), then the whole workgroup waits.
    {
      spv::Id local_index = builder.createLoad(input_local_invocation_index, spv::NoPrecision);
      SpirvBuilder::IfBuilder tile_zero_if(
          builder.createBinOp(spv::OpULessThan, type_bool, local_index,
                              builder.makeUintConstant(16)),
          spv::SelectionControlMaskNone, builder);
      id_vector_temp.clear();
      id_vector_temp.push_back(local_index);
      builder.createStore(builder.makeUintConstant(0),
                          builder.createAccessChain(spv::StorageClassWorkgroup,
                                                    stencil_compute_tile, id_vector_temp));
      tile_zero_if.makeEndIf();
      builder.createControlBarrier(spv::ScopeWorkgroup, spv::ScopeWorkgroup,
                                   spv::MemorySemanticsWorkgroupMemoryMask |
                                       spv::MemorySemanticsAcquireReleaseMask);
    }
    // Out of the rectangle (the dispatch is rounded up to the group size):
    // skip the fetch, but stay for the barrier and the store.
    stencil_compute_in_rect_if = std::make_unique<SpirvBuilder::IfBuilder>(
        builder.createBinOp(
            spv::OpLogicalAnd, type_bool,
            builder.createBinOp(spv::OpULessThan, type_bool, rect_x,
                                load_stencil_compute_push_constant(3)),
            builder.createBinOp(spv::OpULessThan, type_bool, rect_y,
                                load_stencil_compute_push_constant(4))),
        spv::SelectionControlMaskNone, builder);
    // (The loads reuse id_vector_temp, so take them before building the list.)
    spv::Id rect_origin_x = load_stencil_compute_push_constant(1);
    spv::Id rect_origin_y = load_stencil_compute_push_constant(2);
    id_vector_temp.clear();
    id_vector_temp.push_back(rect_origin_x);
    id_vector_temp.push_back(rect_origin_y);
    dest_pixel_coord =
        builder.createBinOp(spv::OpIAdd, type_uint2, stencil_compute_rect_coord,
                            builder.createCompositeConstruct(type_uint2, id_vector_temp));
  } else {
    dest_pixel_coord = builder.createUnaryOp(
        spv::OpConvertFToU, type_uint2,
        builder.createRvalueSwizzle(spv::NoPrecision, type_float2,
                                    builder.createLoad(input_fragment_coord, spv::NoPrecision),
                                    uint_vector_temp));
  }
  spv::Id dest_pixel_x = builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  if (REXCVAR_GET(ac6_wide_world_target)) {
    // AC6 wide world target: a fragment in the right half of a wide target
    // addresses the same EDRAM as its twin in the left half. The identity for
    // every other target, whose fragments never reach its pitch.
    dest_pixel_x = builder.createBinOp(
        spv::OpUMod, type_uint, dest_pixel_x,
        builder.createBinOp(
            spv::OpIMul, type_uint,
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                                builder.makeUintConstant(0),
                                builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
            const_dest_tile_width_pixels));
  }
  spv::Id dest_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y = builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >> uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                              builder.makeUintConstant(0),
                              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(spv::OpBitcast, type_uint,
                                             builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                   dest_tile_pixel_x, builder.makeUintConstant(1),
                                                   builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        if (msaa_2x_attachments_supported_) {
          source_sample_id =
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                                      builder.makeUintConstant(1)),
                                  builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                                 builder.makeUintConstant(1 << 1));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0), dest_tile_pixel_y,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1), builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                dest_tile_pixel_x, builder.makeUintConstant(0),
                                                builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id =
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, source_sample_id,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_tile_pixel_y,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
              builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id = builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), source_tile_pixel_x,
                                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(-int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width_samples >> source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_height_samples >> uint32_t(key.source_msaa_samples >=
                                                                       xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] = builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0], builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int, source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] = builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type = builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining = source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (
          rex::bit_scan_forward(source_color_components_remaining, &source_color_component_index)) {
        source_color_components_remaining &= ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] = builder.createCompositeExtract(
            source_color_vec4, source_color_component_type, source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_uint4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half = builder.createBinOp(
        spv::OpINotEqual, type_bool, source_color_half, builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type, source_color_is_second_half,
          source_color[0][source_color_half_component_count], source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type, source_color_is_second_half,
            source_color[0][source_color_half_component_count + i], source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult && source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
    builder.createStore(builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
                        output_fragment_stencil_ref);
  }

  const bool source_color_16_is_float = IsColor16FormatFloatLike(source_color_format);
  const bool dest_color_16_is_float = IsColor16FormatFloatLike(dest_color_format);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  spv::Id const_float_inv_32767 = builder.makeFloatConstant(1.0f / 32767.0f);
  auto PWLGammaToLinear = [&](spv::Id gamma, bool gamma_pre_saturated) -> spv::Id {
    if (!gamma_pre_saturated) {
      gamma = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                           gamma, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(192.0f / 255.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(8.0f / 1024.0f),
                                               builder.makeFloatConstant(4.0f / 1024.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(-1024.0f),
                                                builder.makeFloatConstant(-256.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(2.0f / 1024.0f),
                                               builder.makeFloatConstant(1.0f / 1024.0f));
    spv::Id offset_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                                builder.makeFloatConstant(-64.0f), const_float_0);
    spv::Id is_piece_at_least_2 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(96.0f / 255.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    spv::Id linear = builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, gamma,
                                                builder.makeFloatConstant(255.0f * 1024.0f)),
                            scale),
        offset);
    linear = builder.createBinOp(spv::OpFAdd, type_float, linear,
                                 builder.createUnaryBuiltinCall(
                                     type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                     builder.createBinOp(spv::OpFMul, type_float, linear, scale)));
    return builder.createBinOp(spv::OpFMul, type_float, linear,
                               builder.makeFloatConstant(1.0f / 1023.0f));
  };
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(
        spv::OpFOrdGreaterThanEqual, type_bool, linear, builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_color_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(type_float2, id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackSource16ComponentToUint(component_0),
        PackSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  auto UnpackDest16ComponentFromUint32 = [&](spv::Id packed_word,
                                             uint32_t component_index) -> spv::Id {
    spv::Id component_offset = component_index & 1 ? const_uint_16 : const_uint_0;
    if (dest_color_is_uint) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_word, component_offset,
                                 const_uint_16);
    }
    if (dest_color_16_is_float) {
      spv::Id component_pair = builder.createUnaryBuiltinCall(
          type_float2, ext_inst_glsl_std_450, GLSLstd450UnpackHalf2x16, packed_word);
      return builder.createCompositeExtract(component_pair, type_float, component_index & 1);
    }
    spv::Id component_snorm = builder.createTriOp(spv::OpBitFieldSExtract, type_int, packed_word,
                                                  component_offset, const_uint_16);
    spv::Id component_float =
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryOp(spv::OpConvertSToF, type_float, component_snorm),
                            const_float_inv_32767);
    return builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                        component_float, const_float_minus_1, const_float_1);
  };

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (gamma_render_target_as_unorm16_) {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                source_color[i][j] = LinearToPWLGamma(source_color[i][j], true);
              }
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(spv::OpFAdd, type_float,
                                          builder.createBinOp(spv::OpFMul, type_float,
                                                              source_color[i][j], unorm_scale),
                                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[i][j],
                                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j), j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[i][0],
                                                                     ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] =
                  builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed[i],
                                       SpirvShaderTranslator::UnclampedFloat32To7e3(
                                           builder, source_color[i][j], ext_inst_glsl_std_450),
                                       builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated =
                builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                             source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(spv::OpFAdd, type_float,
                                        builder.createBinOp(spv::OpFMul, type_float,
                                                            alpha_saturated, unorm_scale_a),
                                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // Route through packed uint32 words so mixed source/destination
        // transfer component types (integer vs float fallback) are handled
        // uniformly.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = PackSource16PairToUint32(source_color[i][0], source_color[i][1]);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] =
                PackSource16PairToUint32(source_color[0][i << 1], source_color[0][(i << 1) + 1]);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[i],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[i],
                                         depth24, depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // If integer transfer formats are unavailable for this sample count,
        // ownership transfer falls back to float formats and raw bits are
        // passed via bitcasts.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      } else {
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed[i >> 1], i));
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (mode.output == TransferOutput::kStencilBit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              source_color[0][0] = LinearToPWLGamma(source_color[0][0], true);
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = PWLGammaToLinear(source_color[0][i], true);
                }
              }
            }
            // Same or converted format - passthrough.
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (uint32_t i = output_fragment_stencil_ref != spv::NoResult ? 0 : 1; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            // When need only depth, not stencil, skip the red component.
            packed_only_depth = true;
            if (output_fragment_stencil_ref != spv::NoResult) {
              // Write the red component to the stencil reference.
              builder.createStore(
                  builder.createUnaryOp(
                      spv::OpBitcast, type_int,
                      builder.createUnaryOp(
                          spv::OpConvertFToU, type_uint,
                          builder.createBinOp(
                              spv::OpFAdd, type_float,
                              builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                  builder.makeFloatConstant(255.0f)),
                              builder.makeFloatConstant(0.5f)))),
                  output_fragment_stencil_ref);
            }
            // Put depth in 0:23.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][1],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 2; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * (i - 1)), component_width);
            }
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (uint32_t i = 0; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 1; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * i), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[0][0],
                                                                  ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed =
                    builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                         SpirvShaderTranslator::UnclampedFloat32To7e3(
                                             builder, source_color[0][i], ext_inst_glsl_std_450),
                                         builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, source_color[0][3],
                  builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // Route through packed uint32 so destination transfer type (integer
          // vs float fallback) is reconstructed consistently.
          packed = PackSource16ComponentToUint(source_color[0][0]);
          if (mode.output != TransferOutput::kStencilBit) {
            packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                          PackSource16ComponentToUint(source_color[0][1]),
                                          const_uint_16, const_uint_16);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth && dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[0],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
                                        builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    } else if (mode.output == TransferOutput::kStencilBit &&
               source_stencil[0] != spv::NoResult) {
      // A stencil bit transfer from a depth / stencil source binds only the
      // stencil texture - the depth is not needed and deliberately not bound -
      // so neither branch above runs and `packed` would be left unset. The
      // stencil bit output below only kills the sample when the bit is clear,
      // and that check is skipped entirely when `packed` is NoResult, so every
      // sample would keep its bit and the destination stencil would come out as
      // 0xFF everywhere regardless of the source. Supply the stencil, which is
      // all the kill needs, in bits 0:7.
      packed = source_stencil[0];
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(8 * i), component_width)),
                    unorm_scale));
              }
              if (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                  gamma_render_target_as_unorm16_) {
                // 8_8_8_8_GAMMA is represented by linear stored in
                // R16G16B16A16_UNORM.
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp[i] = PWLGammaToLinear(id_vector_temp[i], true);
                }
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb = builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              // Color.
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                                            packed, builder.makeUintConstant(30),
                                                            builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed, i));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // If the integer transfer format is unavailable for this sample
              // count, the transfer format is float and this bitcast path is
              // used.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 = builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                                builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1), builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                        builder.makeUintConstant(0), builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                          builder.makeUintConstant(1));
                }
              } else {
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                      builder.makeUintConstant(1), builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples < xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(
                builder.makeIntConstant(int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant =
                builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                             push_constants, id_vector_temp),
                                   spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles =
                builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y =
                builder.createBinOp(spv::OpUDiv, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x =
                builder.createBinOp(spv::OpUMod, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_height_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod = builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false,
                                          false, host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                    builder.makeUintConstant(1), builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp((key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                                       msaa_2x_attachments_supported_)
                                          ? spv::OpBitwiseXor
                                          : spv::OpShiftRightLogical,
                                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(tile_width_samples * tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples),
                                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(
                builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform, host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(spv::OpFMul, type_float, host_depth32,
                                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated =
                builder.createBinOp(spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry = builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block = builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge, spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated, &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(builder, guest_depth24, 0, true,
                                                                   false, ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
            spv::Id depth24_to_depth32_result_block_id = builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 = builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth && output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(spv::OpBitcast, type_int,
                                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                                          builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (stencil_compute) {
          // The whole stencil byte, OR-ed into the workgroup tile (four pixels
          // per dword).
          spv::Id stencil_value =
              packed ? builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                           builder.makeUintConstant(0xFF))
                     : builder.makeUintConstant(0xFF);
          spv::Id local_index = builder.createLoad(input_local_invocation_index, spv::NoPrecision);
          spv::Id shifted_value = builder.createBinOp(
              spv::OpShiftLeftLogical, type_uint, stencil_value,
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, local_index,
                                                      builder.makeUintConstant(3)),
                                  builder.makeUintConstant(3)));
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                       local_index, builder.makeUintConstant(2)));
          spv::Id dword_pointer = builder.createAccessChain(
              spv::StorageClassWorkgroup, stencil_compute_tile, id_vector_temp);
          builder.createQuadOp(spv::OpAtomicOr, type_uint, dword_pointer,
                               builder.makeUintConstant(uint32_t(spv::ScopeWorkgroup)),
                               builder.makeUintConstant(uint32_t(spv::MemorySemanticsMaskNone)),
                               shifted_value);
        } else if (packed) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant =
              builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                           push_constants, id_vector_temp),
                                 spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed, stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  if (stencil_compute) {
    stencil_compute_in_rect_if->makeEndIf();
    stencil_compute_in_rect_if.reset();
    builder.createControlBarrier(spv::ScopeWorkgroup, spv::ScopeWorkgroup,
                                 spv::MemorySemanticsWorkgroupMemoryMask |
                                     spv::MemorySemanticsAcquireReleaseMask);
    // Lanes 0-15 store the tile's dwords: dword j is row j >> 1, columns
    // 4 * (j & 1) .. + 3 of the tile, if inside the rectangle's padded rows.
    spv::Id local_index = builder.createLoad(input_local_invocation_index, spv::NoPrecision);
    SpirvBuilder::IfBuilder store_lane_if(
        builder.createBinOp(spv::OpULessThan, type_bool, local_index,
                            builder.makeUintConstant(16)),
        spv::SelectionControlMaskNone, builder);
    spv::Id rect_x = builder.createCompositeExtract(stencil_compute_rect_coord, type_uint, 0);
    spv::Id rect_y = builder.createCompositeExtract(stencil_compute_rect_coord, type_uint, 1);
    spv::Id tile_x0 = builder.createBinOp(spv::OpBitwiseAnd, type_uint, rect_x,
                                          builder.makeUintConstant(~UINT32_C(7)));
    spv::Id tile_y0 = builder.createBinOp(spv::OpBitwiseAnd, type_uint, rect_y,
                                          builder.makeUintConstant(~UINT32_C(7)));
    spv::Id dword_x = builder.createBinOp(
        spv::OpIAdd, type_uint, tile_x0,
        builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                            builder.createBinOp(spv::OpBitwiseAnd, type_uint, local_index,
                                                builder.makeUintConstant(1)),
                            builder.makeUintConstant(2)));
    spv::Id dword_y = builder.createBinOp(
        spv::OpIAdd, type_uint, tile_y0,
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, local_index,
                            builder.makeUintConstant(1)));
    spv::Id row_pitch = load_stencil_compute_push_constant(5);
    SpirvBuilder::IfBuilder store_in_rect_if(
        builder.createBinOp(
            spv::OpLogicalAnd, type_bool,
            builder.createBinOp(spv::OpULessThan, type_bool, dword_x, row_pitch),
            builder.createBinOp(spv::OpULessThan, type_bool, dword_y,
                                load_stencil_compute_push_constant(4))),
        spv::SelectionControlMaskNone, builder);
    spv::Id byte_index = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIAdd, type_uint,
                            builder.createBinOp(spv::OpIMul, type_uint, dword_y, row_pitch),
                            dword_x),
        load_stencil_compute_push_constant(6));
    id_vector_temp.clear();
    id_vector_temp.push_back(local_index);
    spv::Id tile_dword = builder.createLoad(
        builder.createAccessChain(spv::StorageClassWorkgroup, stencil_compute_tile,
                                  id_vector_temp),
        spv::NoPrecision);
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(0));
    id_vector_temp.push_back(builder.createBinOp(spv::OpShiftRightLogical, type_uint, byte_index,
                                                 builder.makeUintConstant(2)));
    builder.createStore(tile_dword,
                        builder.createAccessChain(spv::StorageClassUniform,
                                                  stencil_compute_output_buffer, id_vector_temp));
    store_in_rect_if.makeEndIf();
    store_lane_if.makeEndIf();
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  spv::Instruction* entry_point;
  if (stencil_compute) {
    builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize,
                             kStencilComputeGroupSizeX, kStencilComputeGroupSizeY, 1);
    entry_point = builder.addEntryPoint(spv::ExecutionModelGLCompute, main_function, "main");
  } else {
    builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
    if (output_fragment_depth != spv::NoResult) {
      builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
    }
    if (output_fragment_stencil_ref != spv::NoResult) {
      builder.addExecutionMode(main_function, spv::ExecutionModeStencilRefReplacingEXT);
    }
    entry_point = builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  }
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer shader 0x{:08X}",
        key.key);
  }
  transfer_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkPipeline const* VulkanRenderTargetCache::GetTransferPipelines(TransferPipelineKey key) {
  auto pipeline_it = transfer_pipelines_.find(key);
  if (pipeline_it != transfer_pipelines_.end()) {
    return pipeline_it->second[0] != VK_NULL_HANDLE ? pipeline_it->second.data() : nullptr;
  }

  const TransferModeInfo& mode = kTransferModes[size_t(key.shader_key.mode)];

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && device_properties.dynamicRendering;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass = GetHostRenderTargetsRenderPass(key.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
      return nullptr;
    }
  }

  VkShaderModule fragment_shader_module = GetTransferShader(key.shader_key);
  if (fragment_shader_module == VK_NULL_HANDLE) {
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }

  uint32_t dest_sample_count = uint32_t(1) << uint32_t(key.shader_key.dest_msaa_samples);
  bool dest_is_masked_sample = dest_sample_count > 1 && !device_properties.sampleRateShading;

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = transfer_passthrough_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].pNext = nullptr;
  shader_stages[1].flags = 0;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = fragment_shader_module;
  shader_stages[1].pName = "main";
  shader_stages[1].pSpecializationInfo = nullptr;
  VkSpecializationMapEntry sample_id_specialization_map_entry;
  uint32_t sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(uint32_t);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries = &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize = sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(float) * 2;
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attribute;
  vertex_input_attribute.location = 0;
  vertex_input_attribute.binding = 0;
  vertex_input_attribute.format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attribute.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount = 1;
  vertex_input_state.pVertexAttributeDescriptions = &vertex_input_attribute;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  // Dynamic, to stay within maxViewportDimensions while preferring a
  // power-of-two factor for converting from pixel coordinates to NDC for exact
  // precision.
  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  // For samples other than the first, will be changed for the pipelines for
  // other samples.
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (dest_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(dest_sample_count);
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
        // supported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Whether the depth / stencil state is used depends on the presence of a
  // depth attachment in the render pass - but not making assumptions about
  // whether the render pass contains any specific attachments, so setting up
  // valid depth / stencil state unconditionally.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp =
        REXCVAR_GET(depth_transfer_not_equal_test) ? VK_COMPARE_OP_NOT_EQUAL : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->extensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by stencil
    // being different.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    // Will be dynamic for stencil bit output.
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Whether the color blend state is used depends on the presence of color
  // attachments in the render pass - but not making assumptions about whether
  // the render pass contains any specific attachments, so setting up valid
  // color blend state unconditionally.
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - rex::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    assert_true(device_properties.independentBlend);
    color_blend_attachments[key.shader_key.dest_color_rt_index].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
  }

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat stencil_attachment_format = VK_FORMAT_UNDEFINED;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;
    if (key.render_pass_key.depth_and_color_used & 0b1) {
      depth_attachment_format = GetDepthVulkanFormat(key.render_pass_key.depth_format);
      stencil_attachment_format = depth_attachment_format;
      pipeline_rendering_create_info.colorAttachmentCount = 0;
      pipeline_rendering_create_info.pColorAttachmentFormats = nullptr;
    } else {
      color_attachment_format =
          GetColorOwnershipTransferVulkanFormat(key.render_pass_key.color_0_view_format);
      pipeline_rendering_create_info.colorAttachmentCount = 1;
      pipeline_rendering_create_info.pColorAttachmentFormats = &color_attachment_format;
    }
    pipeline_rendering_create_info.depthAttachmentFormat = depth_attachment_format;
    pipeline_rendering_create_info.stencilAttachmentFormat = stencil_attachment_format;
  }

  std::array<VkDynamicState, 3> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (mode.output == TransferOutput::kStencilBit) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }

  std::array<VkPipeline, 4> pipelines{};
  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  if (dest_is_masked_sample) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  }
  pipeline_create_info.stageCount = uint32_t(rex::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = transfer_pipeline_layouts_[size_t(mode.pipeline_layout)];
  pipeline_create_info.renderPass = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
        key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    assert_true(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags =
        (pipeline_create_info.flags & ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
        VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (uint32_t i = 1; i < dest_sample_count; ++i) {
      // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
      // supported.
      uint32_t host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1) ? 3 : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = uint32_t(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline for render pass 0x{:08X}, shader "
            "0x{:08X}, sample {}",
            key.render_pass_key.key, key.shader_key.key, i);
        for (uint32_t j = 0; j < i; ++j) {
          dfn.vkDestroyPipeline(device, pipelines[j], nullptr);
        }
        transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
        return nullptr;
      }
    }
  }
  return transfer_pipelines_.emplace(key, pipelines).first->second.data();
}

void VulkanRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  SCOPE_profile_cpu_f("gpu");
  assert_true(GetPath() == Path::kHostRenderTargets);
  {
    // Only worth a mark when there is actually something to do.
    bool any = resolve_clear_rectangle != nullptr;
    for (uint32_t i = 0; !any && i < render_target_count; ++i) {
      any = render_target_transfers && !render_target_transfers[i].empty();
    }
    if (any) {
      command_processor_.GpuTimerMark("edram ownership transfers");
      // The next draw's pass mark closes this interval; force it to re-mark.
      command_processor_.GpuTimerMarkPass(~uint64_t(0) - 1, "edram ownership transfers");
    }
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  VkClearRect resolve_clear_rect;
  // AC6 wide world target: the clear covers both halves (second rect).
  VkClearRect resolve_clear_rects[2];
  uint32_t resolve_clear_rect_count = 1;
  if (resolve_clear_needed) {
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    resolve_clear_rect.rect.offset.x =
        int32_t(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
    resolve_clear_rect.rect.offset.y =
        int32_t(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * draw_resolution_scale_x();
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * draw_resolution_scale_y();
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        // Pipeline.
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        // Descriptor set bindings.
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_, 0,
            uint32_t(rex::countof(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        // Render target constant.
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, SHADER_READ -> DEPTH_STENCIL_ATTACHMENT_WRITE will be
        // done anyway even in the best case, so it's not possible to have all
        // the barriers in one place here.
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Always transitioning both depth and stencil, not storing separate
        // usage flags for depth and stencil.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant), &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        COUNT_profile_add("gpu/edram_depth_store_dispatches", 1);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt || previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_vulkan_rt.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : dest_transfers) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            host_depth_source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            host_depth_source_vulkan_rt.current_layout(), kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    UseEdramBuffer(EdramBufferUsage::kFragmentRead);
  }

  // Perform the transfers and clears.

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  uint32_t transfer_descriptor_sets_bound = 0;
  uint32_t transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();

    // Late barriers in case there was cross-copying that prevented merging of
    // barriers.
    {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }

    // Get the objects needed for transfers to the destination.
    // TODO(Triang3l): Reuse the guest render pass for transfers where possible
    // (if the Vulkan format used for drawing is also usable for transfers - for
    // instance, R8G8B8A8_UNORM can be used for both, so the guest pass can be
    // reused, but R16G16B16A16_SFLOAT render targets use R16G16B16A16_UINT for
    // transfers, so the transfer pass has to be separate) to avoid stores and
    // loads on tile-based devices to make this actually applicable. Also
    // overall perform all non-cross-copying transfers for the current
    // framebuffer configuration in a single pass, to load / store only once.
    RenderPassKey transfer_render_pass_key;
    transfer_render_pass_key.msaa_samples = dest_rt_key.msaa_samples;
    if (dest_rt_key.is_depth) {
      transfer_render_pass_key.depth_and_color_used = 0b1;
      transfer_render_pass_key.depth_format = dest_rt_key.GetDepthFormat();
    } else {
      transfer_render_pass_key.depth_and_color_used = 0b1 << 1;
      transfer_render_pass_key.color_0_view_format = dest_rt_key.GetColorFormat();
      transfer_render_pass_key.color_rts_use_transfer_formats = 1;
    }
    VkRenderPass transfer_render_pass = GetHostRenderTargetsRenderPass(transfer_render_pass_key);
    if (transfer_render_pass == VK_NULL_HANDLE) {
      continue;
    }
    const RenderTarget* transfer_framebuffer_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
    transfer_framebuffer_render_targets[dest_rt_key.is_depth ? 0 : 1] = dest_rt;
    const Framebuffer* transfer_framebuffer =
        GetHostRenderTargetsFramebuffer(transfer_render_pass_key, dest_rt_key.pitch_tiles_at_32bpp,
                                        transfer_framebuffer_render_targets);
    if (!transfer_framebuffer) {
      continue;
    }
    // Don't enter the render pass immediately - may still insert source
    // barriers later.
    VkImageView transfer_dest_view = dest_rt_key.is_depth ? dest_vulkan_rt.view_depth_stencil()
                                                          : dest_vulkan_rt.view_color_transfer();

    if (!current_transfers.empty()) {
      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool stencil_export = vulkan_device->extensions().ext_EXT_shader_stencil_export;
      bool need_stencil_bit_draws = dest_rt_key.is_depth && !stencil_export;
      // Diagnostic: leave the destination stencil alone entirely, to see
      // whether the game reads any of what these passes carry.
      bool skip_stencil = need_stencil_bit_draws &&
                          REXCVAR_GET(ac6_edram_skip_stencil_transfers);
      // Single-sampled destinations take the compute + buffer copy route
      // instead of the eight masked draws, recorded before the render pass.
      bool stencil_via_compute = need_stencil_bit_draws && !skip_stencil &&
                                 dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
                                 REXCVAR_GET(vulkan_edram_stencil_transfer_compute);
      if (stencil_via_compute || skip_stencil) {
        need_stencil_bit_draws = false;
      }
      {
        static bool logged = false;
        if (!logged && dest_rt_key.is_depth) {
          logged = true;
          REXGPU_ERROR("EDRAM transfers: device '{}' shader_stencil_export={} -> depth transfers "
                       "write stencil with {}",
                       vulkan_device->properties().deviceName, stencil_export,
                       stencil_export      ? "single-pass stencil export"
                       : skip_stencil      ? "NOTHING (ac6_edram_skip_stencil_transfers)"
                       : stencil_via_compute
                           ? "a compute pass and a buffer copy where the destination is 1x, "
                             "8 masked draws otherwise"
                           : "8 masked draws per sample");
        }
      }
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->temporary_sort_index() == UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.temporary_sort_index() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          bool host_depth_source_is_copy = host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode = source_rt_key.is_depth
                                               ? TransferMode::kDepthToStencilBit
                                               : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count += transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
                nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_vulkan_rt) {
                if (host_depth_source_is_copy) {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthCopyToDepth
                                                     : TransferMode::kColorAndHostDepthCopyToDepth;
                } else {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthToDepth
                                                     : TransferMode::kColorAndHostDepthToDepth;
                }
              } else {
                new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                                      : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                                    : TransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer, new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(), current_transfer_invocations_.end());

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        if (host_depth_source_vulkan_rt) {
          TransferShaderKey transfer_shader_key = it->shader_key;
          if (transfer_shader_key.mode == TransferMode::kDepthAndHostDepthCopyToDepth ||
              transfer_shader_key.mode == TransferMode::kColorAndHostDepthCopyToDepth) {
            // Reading copied host depth from the EDRAM buffer.
            UseEdramBuffer(EdramBufferUsage::kFragmentRead);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushImageMemoryBarrier(
                host_depth_source_vulkan_rt->image(),
                ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                             VK_IMAGE_ASPECT_STENCIL_BIT),
                host_depth_source_vulkan_rt->current_stage_mask(), kSourceStageMask,
                host_depth_source_vulkan_rt->current_access_mask(), kSourceAccessMask,
                host_depth_source_vulkan_rt->current_layout(), kSourceLayout);
            host_depth_source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                                                  kSourceLayout);
          }
        }
      }

      if (stencil_via_compute && !skip_stencil) {
        if (!RecordStencilBufferTransfers(dest_vulkan_rt, current_transfers,
                                          resolve_clear_rectangle)) {
          REXGPU_ERROR("VulkanRenderTargetCache: stencil transfer via compute failed; stencil "
                       "of the destination is not transferred this time");
        }
      }

      // Perform the transfers for the render target.

      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);

      if (skip_stencil) {
        stencil_clear_rectangle_count = 0;
      }
      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                  stencil_clear_rectangle_count,
                                                  stencil_clear_rect_write_ptr);
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x =
                int32_t(stencil_clear_rectangle.x_pixels * draw_resolution_scale_x());
            stencil_clear_rect_write_ptr->rect.offset.y =
                int32_t(stencil_clear_rectangle.y_pixels * draw_resolution_scale_y());
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels * draw_resolution_scale_x();
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels * draw_resolution_scale_y();
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Prefer power of two viewports for exact division by simply biasing the
      // exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.width),
                         vulkan_device->properties().maxViewportDimensions[0]));
      transfer_viewport.height =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.height),
                         vulkan_device->properties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      float pixels_to_ndc_x = (2.0f / transfer_viewport.width) * float(draw_resolution_scale_x());
      float pixels_to_ndc_y = (2.0f / transfer_viewport.height) * float(draw_resolution_scale_y());
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      command_processor_.SetScissor(transfer_scissor);

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
            nullptr, resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              nullptr, resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_sample_pipeline_count = vulkan_device->properties().sampleRateShading
                                                      ? 1
                                                      : uint32_t(1)
                                                            << uint32_t(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit = (transfer_pipeline_layout_info.used_push_constant_dwords &
                                        kTransferUsedPushConstantDwordStencilMaskBit) != 0;
        {
          // GPU-time attribution by transfer kind, so a heavy "ownership
          // transfers" total can be split into the stencil-bit passes (which
          // AC6 relies on - the tile-seam fix lives there), depth copies and
          // colour copies (which may well copy contents the game never reads).
          const char* transfer_label = transfer_is_stencil_bit ? "edram transfer stencil-bits"
                                       : dest_rt_key.is_depth  ? "edram transfer depth"
                                                               : "edram transfer color";
          command_processor_.GpuTimerMark(transfer_label);
          COUNT_profile_add("gpu/edram_transfer_batches", 1);
          // Which EDRAM reuse patterns cause the transfers: (source -> dest)
          // key histogram, printed once a second. Reads "base/pitch/msaa/fmt".
          if (REXCVAR_GET(gpu_timestamps)) {
            const auto& src_rt = *static_cast<const VulkanRenderTarget*>(it->transfer.source);
            const RenderTargetKey sk = src_rt.key();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: %s %u/%u/%ux -> %s %u/%u/%ux x%u rects",
                          transfer_is_stencil_bit ? "S" : dest_rt_key.is_depth ? "D" : "C",
                          sk.is_depth ? "depth" : "color", uint32_t(sk.base_tiles),
                          uint32_t(sk.GetPitchTiles()), 1u << uint32_t(sk.msaa_samples),
                          dest_rt_key.is_depth ? "depth" : "color", uint32_t(dest_rt_key.base_tiles),
                          uint32_t(dest_pitch_tiles), 1u << uint32_t(dest_rt_key.msaa_samples),
                          transfer_rectangle_count);
            // Per-pattern GPU time: the label carries the pattern, keyed on
            // its hash, so the report shows which alias pair costs what.
            command_processor_.GpuTimerMarkPass(std::hash<std::string_view>{}(buf),
                                                std::string("xfer ") + buf);
            static std::mutex hist_mutex;
            static std::unordered_map<std::string, uint32_t> hist;
            static auto last = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(hist_mutex);
            ++hist[buf];
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(3)) {
              last = now;
              std::vector<std::pair<uint32_t, std::string>> rows;
              for (auto& kv : hist) rows.emplace_back(kv.second, kv.first);
              std::sort(rows.rbegin(), rows.rend());
              REXGPU_ERROR("[EDRAM-XFER] transfer batches in the last 3s, by pattern:");
              for (size_t i = 0; i < rows.size() && i < 12; ++i) {
                REXGPU_ERROR("[EDRAM-XFER]   {:6}  {}", rows[i].first, rows[i].second);
              }
              hist.clear();
            }
          }
        }

        // AC6 wide world target: the rectangles once more at +width, so the
        // right half starts from the same EDRAM contents as the left.
        const uint32_t wide_dest_passes = IsWideKey(dest_rt_key) ? 2 : 1;
        const float wide_dest_offset_ndc =
            dest_rt_key.GetWidth() * pixels_to_ndc_x;
        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count * wide_dest_passes;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(float) * 2 * transfer_vertex_count, sizeof(float),
                transfer_vertex_buffer, transfer_vertex_buffer_offset));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (uint32_t wide_pass = 0; wide_pass < wide_dest_passes; ++wide_pass)
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 = -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x +
                                          (wide_pass ? wide_dest_offset_ndc : 0.0f);
            float transfer_rectangle_y0 = -1.0f + transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 + transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 + transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_buffer.CmdVkBindVertexBuffers(0, 1, &transfer_vertex_buffer,
                                              &transfer_vertex_buffer_offset);

        const VkPipeline* transfer_pipelines = GetTransferPipelines(
            TransferPipelineKey(transfer_render_pass_key, transfer_shader_key));
        if (!transfer_pipelines) {
          continue;
        }
        command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[0]);
        if (last_transfer_pipeline_layout_index != transfer_pipeline_layout_index) {
          last_transfer_pipeline_layout_index = transfer_pipeline_layout_index;
          transfer_descriptor_sets_bound = 0;
          transfer_push_constants_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          VkDescriptorSet descriptor_set_host_depth_stencil_textures =
              host_depth_source_vulkan_rt->GetDescriptorSetTransferSource();
          if (last_descriptor_set_host_depth_stencil_textures !=
              descriptor_set_host_depth_stencil_textures) {
            last_descriptor_set_host_depth_stencil_textures =
                descriptor_set_host_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          VkDescriptorSet descriptor_set_depth_stencil_textures =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_depth_stencil_textures != descriptor_set_depth_stencil_textures) {
            last_descriptor_set_depth_stencil_textures = descriptor_set_depth_stencil_textures;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetColorTextureBit) {
          VkDescriptorSet descriptor_set_color_texture =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_color_texture != descriptor_set_color_texture) {
            last_descriptor_set_color_texture = descriptor_set_color_texture;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetColorTextureBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_vulkan_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        // Apply the new bindings.
        // TODO(Triang3l): Merge binding updates into spans.
        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets & ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetColorTextureBit;
        }
        uint32_t transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords & ~transfer_push_constants_set;
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(uint32_t), &last_host_depth_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(uint32_t), &last_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordAddressBit;
        }

        for (uint32_t j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[j]);
          }
          for (uint32_t k = 0; k < uint32_t(transfer_is_stencil_bit ? 8 : 1); ++k) {
            if (transfer_is_stencil_bit) {
              uint32_t transfer_stencil_bit = uint32_t(1) << k;
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(uint32_t) *
                      rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                     (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(uint32_t), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                      transfer_stencil_bit);
            }
            COUNT_profile_add("gpu/edram_transfer_draws", 1);
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);
      VkClearAttachment resolve_clear_attachment;
      resolve_clear_attachment.colorAttachment = 0;
      std::memset(&resolve_clear_attachment.clearValue, 0,
                  sizeof(resolve_clear_attachment.clearValue));
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        resolve_clear_attachment.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        uint32_t depth_guest_clear_value = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        resolve_clear_attachment.clearValue.depthStencil.stencil = uint32_t(clear_value) & 0xFF;
      } else {
        resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool dest_color_transfer_is_uint = false;
        GetColorOwnershipTransferVulkanFormat(
            dest_rt_key.GetColorFormat(), dest_rt_key.msaa_samples, &dest_color_transfer_is_uint);
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8:
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (dest_rt_key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              // 8_8_8_8_GAMMA is represented by linear stored in
              // R16G16B16A16_UNORM.
              for (uint32_t j = 0; j < 3; ++j) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    xenos::PWLGammaToLinear(resolve_clear_attachment.clearValue.color.float32[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            uint32_t component = uint32_t(clear_value);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component,
                          sizeof(component));
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            uint32_t component_0 = uint32_t(clear_value);
            uint32_t component_1 = uint32_t(clear_value >> 32);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component_0;
              resolve_clear_attachment.clearValue.color.uint32[1] = component_1;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component_0,
                          sizeof(component_0));
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[1], &component_1,
                          sizeof(component_1));
            }
          } break;
        }
      }
      resolve_clear_rects[0] = resolve_clear_rect;
      resolve_clear_rect_count = 1;
      if (IsWideKey(dest_rt_key)) {
        // AC6 wide world target. A resolve-clear between the target's two tile
        // resolves is the guest preparing EDRAM for the second tile - the
        // first tile's colour resolve clears depth - and in one wide render
        // the second tile's contents already sit in the right half, which
        // the second resolve is about to read. That clear stays on the left
        // half, as the EDRAM model says. Any other clear is state both tiles
        // start from and covers both halves.
        bool between_tile_resolves = false;
        for (const auto& kv : wide_awaiting_second_resolve_) {
          between_tile_resolves |= kv.second == dest_rt;
        }
        if (!between_tile_resolves) {
          resolve_clear_rects[1] = resolve_clear_rect;
          resolve_clear_rects[1].rect.offset.x +=
              int32_t(dest_rt_key.GetWidth() * draw_resolution_scale_x());
          resolve_clear_rect_count = 2;
        }
      }
      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, resolve_clear_rect_count,
                                           resolve_clear_rects);
    }
  }
}

VkPipeline VulkanRenderTargetCache::GetDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = BuildRenderTargetSamplingPipeline(key, nullptr);
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

namespace {

// Members of VulkanRenderTargetCache::DirectResolvePushConstants, as seen by
// the generated shader.
enum DirectResolvePushConstant : uint32_t {
  kDirectResolvePushConstantEdramInfo,
  kDirectResolvePushConstantCoordinateInfo,
  kDirectResolvePushConstantDestInfo,
  kDirectResolvePushConstantDestCoordinateInfo,
  kDirectResolvePushConstantDestBase,
  kDirectResolvePushConstantSourceBaseTiles,
  kDirectResolvePushConstantSourcePitchTiles,
  kDirectResolvePushConstantDispatchFirstTile,
  kDirectResolvePushConstantHeightDiv8,
  kDirectResolvePushConstantSourceXOffsetTiles,

  kDirectResolvePushConstantCount,
};

}  // namespace

VkPipeline VulkanRenderTargetCache::BuildRenderTargetSamplingPipeline(
    DumpPipelineKey key, const DirectResolvePipelineKey* direct_key,
    const ResolveToImagePipelineKey* image_key) {
  // Without direct_key, this is the EDRAM dump shader: it samples the host
  // render target and stores the packed guest sample to the EDRAM buffer, from
  // where a separate resolve copy shader moves it to the destination. With
  // direct_key, the same sampling and packing feeds the resolve destination
  // directly, so the round trip through the EDRAM buffer - two passes over the
  // resolved area, the dominant cost of a resolve at raised draw resolutions -
  // is skipped entirely.
  // With image_key, the fused path's region mapping feeds a storage image
  // write of the host texel (the texture load's unpacking of the packed
  // guest dword) instead of a tiled-memory store.
  const bool image = image_key != nullptr;
  const bool direct = direct_key != nullptr || image;
  assert_true(direct_key == nullptr || image_key == nullptr);
  using HostFormat = VulkanTextureCache::ResolveComputeHostFormat;
  const HostFormat image_host_format =
      image ? HostFormat(image_key->host_format) : HostFormat::kCount;
  // How the resolve view's samples map to destination pixels: one EDRAM
  // sample column per pixel at 4x, one sample row per pixel at 2x and 4x.
  const xenos::MsaaSamples image_view_msaa =
      image ? image_key->view_msaa_samples : xenos::MsaaSamples::k1X;
  const uint32_t image_sample_shift_x =
      uint32_t(image_view_msaa >= xenos::MsaaSamples::k4X);
  const uint32_t image_sample_shift_y =
      uint32_t(image_view_msaa >= xenos::MsaaSamples::k2X);
  const xenos::CopySampleSelect image_sample_select =
      image ? image_key->sample_select : xenos::CopySampleSelect::k0;
  // The guest samples this resolve takes: one of them, or the average of a
  // group (the invocation of the first sample of the group does the work).
  std::vector<uint32_t> image_guest_samples;
  if (image && image_view_msaa != xenos::MsaaSamples::k1X) {
    switch (image_sample_select) {
      case xenos::CopySampleSelect::k0:
      case xenos::CopySampleSelect::k1:
      case xenos::CopySampleSelect::k2:
      case xenos::CopySampleSelect::k3:
        image_guest_samples.push_back(uint32_t(image_sample_select));
        break;
      case xenos::CopySampleSelect::k01:
        image_guest_samples = {0, 1};
        break;
      case xenos::CopySampleSelect::k23:
        image_guest_samples = {2, 3};
        break;
      case xenos::CopySampleSelect::k0123:
        image_guest_samples = {0, 1, 2, 3};
        break;
    }
  }
  const bool image_averaging = image_guest_samples.size() > 1;

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);

  // Bindings.
  // The output buffer: the EDRAM buffer, or the resolve destination when
  // fusing. The destination is addressed in single dwords (the guest tiled
  // address function is per-texel), so its element type is always uint.
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  bool output_is_uint2 = format_is_64bpp && !direct;
  spv::Id edram_buffer = spv::NoResult;
  spv::Id output_image = spv::NoResult;
  if (image) {
    spv::ImageFormat image_format = spv::ImageFormatUnknown;
    switch (image_host_format) {
      case HostFormat::kRgba8Unorm:
        image_format = spv::ImageFormatRgba8;
        break;
      case HostFormat::kRgb10A2Unorm:
        image_format = spv::ImageFormatRgb10A2;
        break;
      case HostFormat::kRg16Float:
        image_format = spv::ImageFormatRg16f;
        break;
      case HostFormat::kRgba16Float:
        image_format = spv::ImageFormatRgba16f;
        break;
      case HostFormat::kR32Float:
      case HostFormat::kR32DepthUnorm:
      case HostFormat::kR32DepthFloat:
        image_format = spv::ImageFormatR32f;
        break;
      default:
        assert_unhandled_case(image_host_format);
    }
    if (image_format == spv::ImageFormatRg16f || image_format == spv::ImageFormatRgb10A2) {
      builder.addCapability(spv::CapabilityStorageImageExtendedFormats);
    }
    output_image = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false, false, 2, image_format),
        "xe_resolve_image");
    builder.addDecoration(output_image, spv::DecorationDescriptorSet, kDumpDescriptorSetEdram);
    builder.addDecoration(output_image, spv::DecorationBinding, 0);
    builder.addDecoration(output_image, spv::DecorationNonReadable);
  } else {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(output_is_uint2 ? type_uint2 : type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                          sizeof(uint32_t) << uint32_t(output_is_uint2));
    spv::Id type_edram =
        builder.makeStructType(id_vector_temp, direct ? "XeResolveDest" : "XeEdram");
    builder.addMemberName(type_edram, 0, direct ? "dest" : "edram");
    builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
    builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_edram, spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    edram_buffer = builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_edram,
                                          direct ? "xe_resolve_dest" : "xe_edram");
    builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet, kDumpDescriptorSetEdram);
    builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  }
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(), key.msaa_samples, &source_is_uint);
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false, source_is_multisampled,
                            1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet, kDumpDescriptorSetSource);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          kDumpDescriptorSetSource);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants.
  // The image variant appends image_x, image_y, image_width, image_height.
  constexpr uint32_t kResolveToImagePushConstantImageX = kDirectResolvePushConstantCount;
  constexpr uint32_t kResolveToImagePushConstantCount = kDirectResolvePushConstantCount + 4;
  static_assert(sizeof(ResolveToImagePushConstants) ==
                sizeof(uint32_t) * kResolveToImagePushConstantCount);
  uint32_t push_constant_count = image    ? kResolveToImagePushConstantCount
                                 : direct ? kDirectResolvePushConstantCount
                                          : kDumpPushConstantCount;
  id_vector_temp.clear();
  id_vector_temp.reserve(push_constant_count);
  for (uint32_t i = 0; i < push_constant_count; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants = builder.makeStructType(
      id_vector_temp, direct ? "XeDirectResolvePushConstants" : "XeEdramDumpPushConstants");
  static const char* const kDumpPushConstantNames[] = {"pitches", "offsets"};
  static const char* const kDirectResolvePushConstantNames[] = {
      "edram_info",   "coordinate_info",       "dest_info",          "dest_coordinate_info",
      "dest_base",    "source_base_tiles",     "source_pitch_tiles", "dispatch_first_tile",
      "height_div_8", "source_x_offset_tiles", "image_x",            "image_y",
      "image_width",  "image_height"};
  for (uint32_t i = 0; i < push_constant_count; ++i) {
    builder.addMemberName(
        type_push_constants, i,
        direct ? kDirectResolvePushConstantNames[i] : kDumpPushConstantNames[i]);
    builder.addMemberDecoration(type_push_constants, i, spv::DecorationOffset,
                                int(sizeof(uint32_t) * i));
  }
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants = builder.createVariable(
      spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
      direct ? "xe_direct_resolve_push_constants" : "xe_edram_dump_push_constants");
  auto load_push_constant = [&](uint32_t index) -> spv::Id {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int(index)));
    return builder.createLoad(
        builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
        spv::NoPrecision);
  };

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        spv::BuiltInGlobalInvocationId);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id = builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x = builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) * draw_resolution_scale_x();
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y = builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  // The rows of the dumped rectangle are spaced by the pitch of the resolve
  // region, which the direct path takes from the EDRAM info it already has.
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits = builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id direct_edram_info =
      direct ? load_push_constant(kDirectResolvePushConstantEdramInfo) : spv::NoResult;
  spv::Id pitches_constant = direct ? spv::NoResult : load_push_constant(kDumpPushConstantPitches);
  spv::Id dest_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                          direct ? direct_edram_info : pitches_constant, const_uint_0,
                          const_edram_pitch_tiles_bits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, dest_pitch_tiles, rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  spv::Id offsets_constant = direct ? spv::NoResult : load_push_constant(kDumpPushConstantOffsets);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id dispatch_first_tile =
      direct ? load_push_constant(kDirectResolvePushConstantDispatchFirstTile)
             : builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                                   const_uint_0, const_edram_base_tiles_bits_plus_1);
  spv::Id edram_tile_index_non_wrapped =
      builder.createBinOp(spv::OpIAdd, type_uint, dispatch_first_tile, rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index. Not needed when resolving
  // directly - neither the store nor the load goes through the EDRAM buffer,
  // so its layout (including the depth column swap below) drops out.
  spv::Id edram_sample_address = spv::NoResult;
  if (!direct) {
  edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint, builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, tile_sample_y),
          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }
  }

  // Where this sample lands inside the resolve region, for the fused path. Done
  // before the source coordinates, because the half-pixel offset fill reads a
  // different sample than the one this invocation would otherwise sample.
  spv::Id direct_region_x = spv::NoResult, direct_region_y = spv::NoResult,
          direct_in_region = spv::NoResult, direct_coordinate_info = spv::NoResult;
  if (direct) {
    spv::Id type_bool_region = builder.makeBoolType();
    auto uconst = [&](uint32_t value) { return builder.makeUintConstant(value); };
    auto bin = [&](spv::Op op, spv::Id a, spv::Id b) {
      return builder.createBinOp(op, type_uint, a, b);
    };
    auto bits = [&](spv::Id value, uint32_t offset, uint32_t count) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, value, uconst(offset),
                                 uconst(count));
    };
    direct_coordinate_info = load_push_constant(kDirectResolvePushConstantCoordinateInfo);

    // Only 1x MSAA is fused, so guest samples and guest pixels coincide.
    uint32_t sample_size_log2_x = 3 + uint32_t(format_is_64bpp);
    uint32_t tile_width_unscaled = xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp);
    spv::Id origin_x_samples =
        bin(spv::OpShiftLeftLogical, bits(direct_coordinate_info, 0, 4), uconst(sample_size_log2_x));
    spv::Id origin_y_samples =
        bin(spv::OpShiftLeftLogical, bits(direct_coordinate_info, 4, 1), uconst(3));
    spv::Id region_base_tile = bin(
        spv::OpIAdd,
        bin(spv::OpIAdd,
            bits(direct_edram_info, xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1,
                 xenos::kEdramBaseTilesBits),
            bin(spv::OpIMul,
                bin(spv::OpUDiv, origin_y_samples, uconst(xenos::kEdramTileHeightSamples)),
                dest_pitch_tiles)),
        bin(spv::OpUDiv, origin_x_samples, uconst(tile_width_unscaled)));
    spv::Id origin_in_tile_x =
        bin(spv::OpIMul, bin(spv::OpUMod, origin_x_samples, uconst(tile_width_unscaled)),
            uconst(draw_resolution_scale_x()));
    spv::Id origin_in_tile_y = bin(
        spv::OpIMul, bin(spv::OpUMod, origin_y_samples, uconst(xenos::kEdramTileHeightSamples)),
        uconst(draw_resolution_scale_y()));

    spv::Id region_tile_index = bin(spv::OpISub, edram_tile_index_non_wrapped, region_base_tile);
    spv::Id position_x =
        bin(spv::OpIAdd,
            bin(spv::OpIMul, bin(spv::OpUMod, region_tile_index, dest_pitch_tiles),
                const_tile_width),
            tile_sample_x);
    spv::Id position_y =
        bin(spv::OpIAdd,
            bin(spv::OpIMul, bin(spv::OpUDiv, region_tile_index, dest_pitch_tiles),
                const_tile_height),
            tile_sample_y);
    direct_region_x = bin(spv::OpISub, position_x, origin_in_tile_x);
    direct_region_y = bin(spv::OpISub, position_y, origin_in_tile_y);
    // The region extents are in pixels; with a multisampled resolve view the
    // positions above are in samples.
    spv::Id compare_region_x =
        image_sample_shift_x
            ? bin(spv::OpShiftRightLogical, direct_region_x, uconst(image_sample_shift_x))
            : direct_region_x;
    spv::Id compare_region_y =
        image_sample_shift_y
            ? bin(spv::OpShiftRightLogical, direct_region_y, uconst(image_sample_shift_y))
            : direct_region_y;
    spv::Id region_width = bin(
        spv::OpIMul,
        bin(spv::OpShiftLeftLogical,
            bits(direct_coordinate_info, 5,
                 xenos::kResolveSizeBits - xenos::kResolveAlignmentPixelsLog2),
            uconst(xenos::kResolveAlignmentPixelsLog2)),
        uconst(draw_resolution_scale_x()));
    spv::Id region_height =
        bin(spv::OpIMul,
            bin(spv::OpShiftLeftLogical, load_push_constant(kDirectResolvePushConstantHeightDiv8),
                uconst(xenos::kResolveAlignmentPixelsLog2)),
            uconst(draw_resolution_scale_y()));
    direct_in_region = builder.createBinOp(
        spv::OpLogicalAnd, type_bool_region,
        builder.createBinOp(
            spv::OpLogicalAnd, type_bool_region,
            builder.createBinOp(spv::OpUGreaterThanEqual, type_bool_region, position_x,
                                origin_in_tile_x),
            builder.createBinOp(spv::OpULessThan, type_bool_region, compare_region_x,
                                region_width)),
        builder.createBinOp(
            spv::OpLogicalAnd, type_bool_region,
            builder.createBinOp(spv::OpUGreaterThanEqual, type_bool_region, position_y,
                                origin_in_tile_y),
            builder.createBinOp(spv::OpULessThan, type_bool_region, compare_region_y,
                                region_height)));

    // Half-pixel offset fill: with resolution scaling the guest's half-pixel
    // offset becomes a full-pixel one, leaving the left and the top edges of the
    // region uncovered. The copy shader fills them from the first surely covered
    // column and row, so sample that column and row here instead - the
    // destination address still uses the unbent position.
    uint32_t fill_x = draw_resolution_scale_x() >> 1;
    uint32_t fill_y = draw_resolution_scale_y() >> 1;
    if (fill_x || fill_y) {
      spv::Id fill_enabled = builder.createBinOp(
          spv::OpINotEqual, type_bool_region,
          bin(spv::OpBitwiseAnd, direct_edram_info,
              uconst(UINT32_C(1) << (xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1 +
                                     xenos::kEdramBaseTilesBits + xenos::kRenderTargetFormatBits +
                                     1))),
          const_uint_0);
      auto bend = [&](spv::Id tile_sample, spv::Id region, uint32_t fill) {
        if (!fill) {
          return tile_sample;
        }
        spv::Id needs_fill = builder.createBinOp(
            spv::OpLogicalAnd, type_bool_region, fill_enabled,
            builder.createBinOp(spv::OpULessThan, type_bool_region, region, uconst(fill)));
        return builder.createTriOp(
            spv::OpSelect, type_uint, needs_fill,
            bin(spv::OpIAdd, tile_sample, bin(spv::OpISub, uconst(fill), region)), tile_sample);
      };
      tile_sample_x = bend(tile_sample_x, direct_region_x, fill_x);
      tile_sample_y = bend(tile_sample_y, direct_region_y, fill_y);
    }
  }

  // Get the linear tile index within the source texture.
  spv::Id source_base_tiles =
      direct ? load_push_constant(kDirectResolvePushConstantSourceBaseTiles)
             : builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                                   const_edram_base_tiles_bits_plus_1,
                                   builder.makeUintConstant(xenos::kEdramBaseTilesBits));
  spv::Id source_tile_index = builder.createBinOp(spv::OpISub, type_uint,
                                                  edram_tile_index_non_wrapped, source_base_tiles);
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles =
      direct ? load_push_constant(kDirectResolvePushConstantSourcePitchTiles)
             : builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                                   const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  if (REXCVAR_GET(ac6_wide_world_target)) {
    // AC6 wide world target: the second tile's resolve reads the right half.
    spv::Id source_x_offset_tiles =
        direct ? load_push_constant(kDirectResolvePushConstantSourceXOffsetTiles)
               : builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                                     builder.makeUintConstant(2 * xenos::kEdramPitchTilesBits),
                                     builder.makeUintConstant(8));
    source_tile_index_x =
        builder.createBinOp(spv::OpIAdd, type_uint, source_tile_index_x, source_x_offset_tiles);
  }
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height, source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y =
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_x, const_uint_1);
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      source_sample_id = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x, const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // 2x MSAA source texture sample index - convert from the guest to
      // the Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y, const_uint_1),
              const_uint_0),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_attachments_supported_)),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_attachments_supported_)));
    }
  }

  // Load the source, and pack the value into one or two 32-bit integers.
  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  source_texture_parameters.sampler = builder.createLoad(source_texture, spv::NoPrecision);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
  source_texture_parameters.coords = builder.createCompositeConstruct(type_int2, id_vector_temp);
  if (source_is_multisampled) {
    source_texture_parameters.sample =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  spv::Id type_source_vec4 = builder.makeVectorType(source_component_type, 4);
  spv::Id source_vec4;
  if (image_averaging) {
    // Averaging sample select: the samples of the destination pixel are at
    // the same source pixel, so only the sample index varies. Averaged
    // before packing, like the resolve hardware.
    assert_true(!source_is_uint && !key.is_depth);
    for (size_t i = 0; i < image_guest_samples.size(); ++i) {
      uint32_t guest_sample = image_guest_samples[i];
      uint32_t host_sample =
          key.msaa_samples >= xenos::MsaaSamples::k4X
              ? guest_sample
              : draw_util::GetD3D10SampleIndexForGuest2xMSAA(guest_sample,
                                                             msaa_2x_attachments_supported_);
      source_texture_parameters.sample = builder.makeIntConstant(int32_t(host_sample));
      spv::Id sample_vec4 = builder.createTextureCall(
          spv::NoPrecision, type_source_vec4, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      source_vec4 = i ? builder.createBinOp(spv::OpFAdd, type_source_vec4, source_vec4, sample_vec4)
                      : sample_vec4;
    }
    spv::Id average_scale =
        builder.makeFloatConstant(1.0f / float(image_guest_samples.size()));
    id_vector_temp.clear();
    for (size_t i = 0; i < 4; ++i) {
      id_vector_temp.push_back(average_scale);
    }
    source_vec4 = builder.createBinOp(spv::OpFMul, type_source_vec4, source_vec4,
                                      builder.createCompositeConstruct(type_source_vec4,
                                                                       id_vector_temp));
  } else {
    source_vec4 = builder.createTextureCall(spv::NoPrecision, type_source_vec4, false, true, false,
                                            false, false, source_texture_parameters,
                                            spv::ImageOperandsMaskNone);
  }
  const bool source_color_16_is_float =
      !key.is_depth && IsColor16FormatFloatLike(key.GetColorFormat());
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackDumpSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(builder.makeVectorType(type_float, 2), id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackDumpSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackDumpSource16ComponentToUint(component_0),
        PackDumpSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(spv::NoPrecision, builder.makeVectorType(type_uint, 4), false,
                                  true, false, false, false, source_texture_parameters,
                                  spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 = builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(spv::OpFMul, type_float, source_depth32,
                                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
                                     builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        if (gamma_render_target_as_unorm16_) {
          // 8_8_8_8_GAMMA is represented by linear stored in
          // R16G16B16A16_UNORM.
          id_vector_temp.clear();
          for (uint32_t i = 0; i < 3; ++i) {
            id_vector_temp.push_back(
                LinearToPWLGamma(builder.createCompositeExtract(source_vec4, type_float, i), true));
          }
        }
      }
        [[fallthrough]];
      case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        spv::Id color_0 = builder.createCompositeExtract(source_vec4, type_float, 0);
        if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
            gamma_render_target_as_unorm16_) {
          color_0 = id_vector_temp[0];
        }
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float, color_0, unorm_scale),
                                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (uint32_t i = 1; i < 4; ++i) {
          spv::Id color_i = builder.createCompositeExtract(source_vec4, type_float, i);
          if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
              gamma_render_target_as_unorm16_ && i < 3) {
            color_i = id_vector_temp[i];
          }
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(spv::OpConvertFToU, type_uint,
                                    builder.createBinOp(spv::OpFAdd, type_float,
                                                        builder.createBinOp(spv::OpFMul, type_float,
                                                                            color_i, unorm_scale),
                                                        unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(spv::OpFMul, type_float,
                                    builder.createCompositeExtract(source_vec4, type_float, 0),
                                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(spv::OpFAdd, type_float,
                                      builder.createBinOp(spv::OpFMul, type_float,
                                                          builder.createCompositeExtract(
                                                              source_vec4, type_float, i),
                                                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SpirvShaderTranslator::UnclampedFloat32To7e3(
                  builder, builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                                        builder.makeFloatConstant(3.0f)),
                                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          packed[i] = PackDumpSource16PairToUint32(
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i),
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i + 1));
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4, source_component_type, i);
          if (!source_is_uint) {
            packed_ref = builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  if (direct) {
    // Fused path: place the packed sample straight into the resolve
    // destination, doing what the resolve copy shader would have done with the
    // value after reading it back out of the EDRAM buffer.
    spv::Id type_bool = builder.makeBoolType();
    auto uconst = [&](uint32_t value) { return builder.makeUintConstant(value); };
    auto bin = [&](spv::Op op, spv::Id a, spv::Id b) {
      return builder.createBinOp(op, type_uint, a, b);
    };
    auto add = [&](spv::Id a, spv::Id b) { return bin(spv::OpIAdd, a, b); };
    auto sub = [&](spv::Id a, spv::Id b) { return bin(spv::OpISub, a, b); };
    auto mul = [&](spv::Id a, spv::Id b) { return bin(spv::OpIMul, a, b); };
    auto shl = [&](spv::Id a, uint32_t b) { return bin(spv::OpShiftLeftLogical, a, uconst(b)); };
    auto shr = [&](spv::Id a, uint32_t b) { return bin(spv::OpShiftRightLogical, a, uconst(b)); };
    auto band = [&](spv::Id a, uint32_t b) { return bin(spv::OpBitwiseAnd, a, uconst(b)); };
    auto bor = [&](spv::Id a, spv::Id b) { return bin(spv::OpBitwiseOr, a, b); };
    auto bits = [&](spv::Id value, uint32_t offset, uint32_t count) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, value, uconst(offset),
                                 uconst(count));
    };

    spv::Id coordinate_info = direct_coordinate_info;
    spv::Id dest_info = load_push_constant(kDirectResolvePushConstantDestInfo);
    spv::Id dest_coordinate_info =
        load_push_constant(kDirectResolvePushConstantDestCoordinateInfo);
    spv::Id region_x = direct_region_x;
    spv::Id region_y = direct_region_y;
    spv::Id in_region = direct_in_region;
    (void)coordinate_info;

    if (image) {
      // With a multisampled resolve view, the region positions are in samples:
      // the destination pixel is the sample position shifted down, and only
      // the invocation holding the selected guest sample (the first of the
      // group when averaging) writes it.
      spv::Id dest_region_x =
          image_sample_shift_x ? shr(region_x, image_sample_shift_x) : region_x;
      spv::Id dest_region_y =
          image_sample_shift_y ? shr(region_y, image_sample_shift_y) : region_y;
      if (!image_guest_samples.empty()) {
        spv::Id guest_sample = const_uint_0;
        if (image_sample_shift_x) {
          guest_sample = band(region_x, 1);
        }
        if (image_sample_shift_y) {
          spv::Id row_bit = band(region_y, 1);
          guest_sample = image_sample_shift_x
                             ? bor(guest_sample, shl(row_bit, 1))
                             : row_bit;
        }
        in_region = builder.createBinOp(
            spv::OpLogicalAnd, type_bool, in_region,
            builder.createBinOp(spv::OpIEqual, type_bool, guest_sample,
                                uconst(image_guest_samples[0])));
      }
      // The texel within the level: the texture cache's destination origin
      // already includes the offset within the 32x32-aligned base tile.
      spv::Id image_x = add(dest_region_x, load_push_constant(kResolveToImagePushConstantImageX));
      spv::Id image_y =
          add(dest_region_y, load_push_constant(kResolveToImagePushConstantImageX + 1));
      // Resolve rectangles are 8-aligned and may overshoot the level.
      spv::Id in_image = builder.createBinOp(
          spv::OpLogicalAnd, type_bool,
          builder.createBinOp(spv::OpULessThan, type_bool, image_x,
                              load_push_constant(kResolveToImagePushConstantImageX + 2)),
          builder.createBinOp(spv::OpULessThan, type_bool, image_y,
                              load_push_constant(kResolveToImagePushConstantImageX + 3)));
      in_region = builder.createBinOp(spv::OpLogicalAnd, type_bool, in_region, in_image);

      // The red/blue swap of the copy shaders, by the SOURCE format (as the
      // copy shaders do); the endianness is undone by the texture load, so it
      // is not applied. Only 8888 and 10:10:10:2 have swaps.
      spv::Id source_format =
          bits(direct_edram_info,
               xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1 + xenos::kEdramBaseTilesBits,
               xenos::kRenderTargetFormatBits);
      spv::Id swap_enabled = builder.createBinOp(spv::OpINotEqual, type_bool,
                                                 band(dest_info, UINT32_C(1) << 24), const_uint_0);
      auto swap_value = [&](spv::Id value) {
        spv::Id swapped_8888 = bor(bor(band(value, 0xFF00FF00), shl(band(value, 0xFF), 16)),
                                   band(shr(value, 16), 0xFF));
        spv::Id swapped_1010 = bor(bor(band(value, 0xC00FFC00), shl(band(value, 0x3FF), 20)),
                                   band(shr(value, 20), 0x3FF));
        spv::Id is_8888 = builder.createBinOp(
            spv::OpULessThanEqual, type_bool, source_format,
            uconst(uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)));
        spv::Id is_1010 = builder.createBinOp(
            spv::OpLogicalOr, type_bool,
            builder.createBinOp(
                spv::OpULessThanEqual, type_bool,
                sub(source_format, uconst(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10))),
                uconst(1)),
            builder.createBinOp(
                spv::OpLogicalOr, type_bool,
                builder.createBinOp(
                    spv::OpIEqual, type_bool, source_format,
                    uconst(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))),
                builder.createBinOp(
                    spv::OpIEqual, type_bool, source_format,
                    uconst(uint32_t(
                        xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)))));
        spv::Id swapped = builder.createTriOp(
            spv::OpSelect, type_uint, is_8888, swapped_8888,
            builder.createTriOp(spv::OpSelect, type_uint, is_1010, swapped_1010, value));
        return builder.createTriOp(spv::OpSelect, type_uint, swap_enabled, swapped, value);
      };

      SpirvBuilder::IfBuilder if_in_image(in_region, spv::SelectionControlDontFlattenMask,
                                          builder);
      {
        spv::Id value0 = swap_value(packed[0]);
        spv::Id type_float4 = builder.makeVectorType(type_float, 4);
        spv::Id type_float2 = builder.makeVectorType(type_float, 2);
        spv::Id texel = spv::NoResult;
        auto unpack_unorm = [&](spv::Id value, uint32_t offset, uint32_t width) {
          return builder.createBinOp(
              spv::OpFMul, type_float,
              builder.createUnaryOp(spv::OpConvertUToF, type_float, bits(value, offset, width)),
              builder.makeFloatConstant(1.0f / float((UINT32_C(1) << width) - 1)));
        };
        switch (image_host_format) {
          case HostFormat::kRgba8Unorm: {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(unpack_unorm(value0, 8 * i, 8));
            }
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          case HostFormat::kRgb10A2Unorm: {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 3; ++i) {
              id_vector_temp.push_back(unpack_unorm(value0, 10 * i, 10));
            }
            id_vector_temp.push_back(unpack_unorm(value0, 30, 2));
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          case HostFormat::kRg16Float:
          case HostFormat::kRgba16Float: {
            spv::Id lo = builder.createUnaryBuiltinCall(type_float2, ext_inst_glsl_std_450,
                                                        GLSLstd450UnpackHalf2x16, value0);
            id_vector_temp.clear();
            id_vector_temp.push_back(builder.createCompositeExtract(lo, type_float, 0));
            id_vector_temp.push_back(builder.createCompositeExtract(lo, type_float, 1));
            if (image_host_format == HostFormat::kRgba16Float) {
              spv::Id hi = builder.createUnaryBuiltinCall(type_float2, ext_inst_glsl_std_450,
                                                          GLSLstd450UnpackHalf2x16, packed[1]);
              id_vector_temp.push_back(builder.createCompositeExtract(hi, type_float, 0));
              id_vector_temp.push_back(builder.createCompositeExtract(hi, type_float, 1));
            } else {
              id_vector_temp.push_back(const_float_0);
              id_vector_temp.push_back(const_float_1);
            }
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          case HostFormat::kR32Float:
          case HostFormat::kR32DepthUnorm:
          case HostFormat::kR32DepthFloat: {
            spv::Id r;
            if (image_host_format == HostFormat::kR32Float) {
              r = builder.createUnaryOp(spv::OpBitcast, type_float, value0);
            } else if (image_host_format == HostFormat::kR32DepthUnorm) {
              // The packed dword is stencil in 0:7, depth in 8:31.
              r = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(spv::OpConvertUToF, type_float, shr(value0, 8)),
                  builder.makeFloatConstant(1.0f / float(0xFFFFFF)));
            } else {
              r = SpirvShaderTranslator::Depth20e4To32(builder, value0, 8, false, false,
                                                       ext_inst_glsl_std_450);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(r);
            id_vector_temp.push_back(const_float_0);
            id_vector_temp.push_back(const_float_0);
            id_vector_temp.push_back(const_float_1);
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          default:
            assert_unhandled_case(image_host_format);
        }
        switch (REXCVAR_GET(vulkan_resolve_to_texture_compute_debug)) {
          case 1: {
            // Does the write land where the game samples?
            id_vector_temp.clear();
            id_vector_temp.push_back(const_float_1);
            id_vector_temp.push_back(const_float_0);
            id_vector_temp.push_back(const_float_1);
            id_vector_temp.push_back(const_float_1);
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          case 2: {
            // Did the render target sample arrive at all (before any packing)?
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 3; ++i) {
              spv::Id component = builder.createCompositeExtract(
                  source_vec4, source_is_uint ? type_uint : type_float, i);
              if (source_is_uint) {
                component = builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(spv::OpConvertUToF, type_float, component),
                    builder.makeFloatConstant(1.0f / 65535.0f));
              }
              id_vector_temp.push_back(component);
            }
            id_vector_temp.push_back(const_float_1);
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          case 3: {
            // Did the packing produce anything? Low byte as grey.
            spv::Id grey = builder.createBinOp(
                spv::OpFMul, type_float,
                builder.createUnaryOp(spv::OpConvertUToF, type_float, bits(packed[0], 0, 8)),
                builder.makeFloatConstant(1.0f / 255.0f));
            id_vector_temp.clear();
            id_vector_temp.push_back(grey);
            id_vector_temp.push_back(grey);
            id_vector_temp.push_back(grey);
            id_vector_temp.push_back(const_float_1);
            texel = builder.createCompositeConstruct(type_float4, id_vector_temp);
          } break;
          default:
            break;
        }
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, image_x));
        id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, image_y));
        spv::Id image_coord = builder.createCompositeConstruct(type_int2, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.createLoad(output_image, spv::NoPrecision));
        id_vector_temp.push_back(image_coord);
        id_vector_temp.push_back(texel);
        builder.createNoResultOp(spv::OpImageWrite, id_vector_temp);
      }
      if_in_image.makeEndIf();
    }

    // The destination position, in host texels.
    spv::Id host_dest_x =
        add(region_x, mul(shl(bits(dest_coordinate_info, 20, 4),
                              xenos::kResolveAlignmentPixelsLog2),
                          uconst(draw_resolution_scale_x())));
    spv::Id host_dest_y =
        add(region_y, mul(shl(bits(dest_coordinate_info, 24, 4),
                              xenos::kResolveAlignmentPixelsLog2),
                          uconst(draw_resolution_scale_y())));

    // A scaled destination doesn't expand each guest texel on its own: the
    // texels sharing a 16-byte block stay together, and it's the block that is
    // repeated draw_resolution_scale_x * draw_resolution_scale_y times. So the
    // guest position comes from the host position with the block index divided
    // by the scale, not the texel index.
    uint32_t texels_per_block_log2 = 2 - uint32_t(format_is_64bpp);
    bool scaled = draw_resolution_scale_x() > 1 || draw_resolution_scale_y() > 1;
    spv::Id guest_x, guest_y, sub_index = spv::NoResult, texel_in_block = spv::NoResult;
    if (scaled) {
      spv::Id host_block_x = shr(host_dest_x, texels_per_block_log2);
      guest_x = shl(bin(spv::OpUDiv, host_block_x, uconst(draw_resolution_scale_x())),
                    texels_per_block_log2);
      guest_y = bin(spv::OpUDiv, host_dest_y, uconst(draw_resolution_scale_y()));
      sub_index =
          add(mul(bin(spv::OpUMod, host_block_x, uconst(draw_resolution_scale_x())),
                  uconst(draw_resolution_scale_y())),
              bin(spv::OpUMod, host_dest_y, uconst(draw_resolution_scale_y())));
      texel_in_block = band(host_dest_x, (UINT32_C(1) << texels_per_block_log2) - 1);
    } else {
      guest_x = host_dest_x;
      guest_y = host_dest_y;
    }
    spv::Id dest_x = guest_x;
    spv::Id dest_y = guest_y;
    spv::Id dest_pitch_div_32 =
        bits(dest_coordinate_info, 0,
             xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 - xenos::kTextureTileWidthHeightLog2);
    uint32_t bpb_log2 = 2 + uint32_t(format_is_64bpp);
    // texture_util::GetTiledOffset2D, in the shader.
    spv::Id tiled_2d;
    {
      spv::Id macro = shl(add(shr(dest_x, 5), mul(shr(dest_y, 5), dest_pitch_div_32)),
                          bpb_log2 + 7);
      spv::Id micro = shl(add(band(dest_x, 7), shl(band(dest_y, 0xE), 2)), bpb_log2);
      spv::Id offset = add(add(add(macro, shl(band(micro, ~UINT32_C(0xF)), 1)), band(micro, 0xF)),
                           shl(band(dest_y, 1), 4));
      tiled_2d = add(
          add(add(add(shl(band(offset, ~UINT32_C(0x1FF)), 3), shl(band(dest_y, 16), 7)),
                  shl(band(offset, 0x1C0), 2)),
              shl(band(add(shr(band(dest_y, 8), 2), shr(dest_x, 3)), 3), 6)),
          band(offset, 0x3F));
    }
    // texture_util::GetTiledOffset3D, in the shader - the destination may be a
    // slice of a 3D texture.
    spv::Id dest_z = bits(dest_info, 4, 3);
    spv::Id tiled_3d;
    {
      spv::Id dest_height_div_32 =
          bits(dest_coordinate_info,
               xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 - xenos::kTextureTileWidthHeightLog2,
               xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 - xenos::kTextureTileWidthHeightLog2);
      spv::Id macro_outer =
          mul(add(shr(dest_y, 4), mul(shr(dest_z, 2), shl(dest_height_div_32, 1))),
              dest_pitch_div_32);
      spv::Id macro =
          shl(band(shl(add(shr(dest_x, 5), macro_outer), bpb_log2 + 6), 0xFFFFFFF), 1);
      spv::Id micro = shr(shl(add(band(dest_x, 7), shl(band(dest_y, 6), 2)), bpb_log2 + 6), 6);
      spv::Id offset_outer = band(add(shr(dest_y, 3), shr(dest_z, 2)), 1);
      spv::Id offset1 =
          add(offset_outer, shl(band(add(shr(dest_x, 3), shl(offset_outer, 1)), 3), 1));
      spv::Id offset2 = add(add(add(shl(add(macro, band(micro, ~UINT32_C(15))), 1), band(micro, 15)),
                                shl(band(dest_z, 3), bpb_log2 + 6)),
                            shl(band(dest_y, 1), 4));
      spv::Id address = shl(band(offset1, 1), 3);
      address = add(address, band(shr(offset2, 6), 7));
      address = shl(address, 3);
      address = add(address, band(offset1, ~UINT32_C(1)));
      address = shl(address, 2);
      address = add(address, band(offset2, ~UINT32_C(511)));
      address = shl(address, 3);
      tiled_3d = add(address, band(offset2, 63));
    }
    spv::Id dest_offset = builder.createTriOp(
        spv::OpSelect, type_uint,
        builder.createBinOp(spv::OpINotEqual, type_bool, band(dest_info, 8), const_uint_0),
        tiled_3d, tiled_2d);
    if (scaled) {
      dest_offset =
          add(add(mul(dest_offset, uconst(draw_resolution_scale_x() * draw_resolution_scale_y())),
                  shl(sub_index, 4)),
              shl(texel_in_block, bpb_log2));
    } else {
      dest_offset = add(dest_offset, load_push_constant(kDirectResolvePushConstantDestBase));
    }

    // Apply the destination swap and endianness, as the resolve copy shader
    // does to the value it reads from the EDRAM buffer.
    spv::Id source_format = bits(direct_edram_info,
                                 xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1 +
                                     xenos::kEdramBaseTilesBits,
                                 xenos::kRenderTargetFormatBits);
    spv::Id swap_enabled = builder.createBinOp(spv::OpINotEqual, type_bool,
                                               band(dest_info, UINT32_C(1) << 24), const_uint_0);
    spv::Id endian = band(dest_info, 7);
    spv::Id endian_8_in_16 = builder.createBinOp(
        spv::OpLogicalOr, type_bool,
        builder.createBinOp(spv::OpIEqual, type_bool, endian,
                            uconst(uint32_t(xenos::Endian128::k8in16))),
        builder.createBinOp(spv::OpIEqual, type_bool, endian,
                            uconst(uint32_t(xenos::Endian128::k8in32))));
    spv::Id endian_16_in_32 = builder.createBinOp(
        spv::OpLogicalOr, type_bool,
        builder.createBinOp(spv::OpIEqual, type_bool, endian,
                            uconst(uint32_t(xenos::Endian128::k8in32))),
        builder.createBinOp(spv::OpIEqual, type_bool, endian,
                            uconst(uint32_t(xenos::Endian128::k16in32))));
    auto transform_value = [&](spv::Id value) {
      // Red/blue swap, by the source render target format, as in the copy
      // shaders.
      spv::Id swapped_8888 = bor(bor(band(value, 0xFF00FF00), shl(band(value, 0xFF), 16)),
                                 band(shr(value, 16), 0xFF));
      spv::Id swapped_1010 = bor(bor(band(value, 0xC00FFC00), shl(band(value, 0x3FF), 20)),
                                 band(shr(value, 20), 0x3FF));
      spv::Id is_8888 = builder.createBinOp(
          spv::OpULessThanEqual, type_bool, source_format,
          uconst(uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)));
      spv::Id is_1010 = builder.createBinOp(
          spv::OpLogicalOr, type_bool,
          builder.createBinOp(
              spv::OpULessThanEqual, type_bool,
              sub(source_format, uconst(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10))),
              uconst(1)),
          builder.createBinOp(
              spv::OpLogicalOr, type_bool,
              builder.createBinOp(
                  spv::OpIEqual, type_bool, source_format,
                  uconst(uint32_t(
                      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))),
              builder.createBinOp(
                  spv::OpIEqual, type_bool, source_format,
                  uconst(uint32_t(xenos::ColorRenderTargetFormat::
                                      k_2_10_10_10_FLOAT_AS_16_16_16_16)))));
      spv::Id swapped = builder.createTriOp(
          spv::OpSelect, type_uint, is_8888, swapped_8888,
          builder.createTriOp(spv::OpSelect, type_uint, is_1010, swapped_1010, value));
      spv::Id result = builder.createTriOp(spv::OpSelect, type_uint, swap_enabled, swapped, value);
      result = builder.createTriOp(
          spv::OpSelect, type_uint, endian_8_in_16,
          bor(shl(band(result, 0x00FF00FF), 8), shr(band(result, 0xFF00FF00), 8)), result);
      result = builder.createTriOp(spv::OpSelect, type_uint, endian_16_in_32,
                                   bor(shl(result, 16), shr(result, 16)), result);
      return result;
    };

    if (!image) {
    SpirvBuilder::IfBuilder if_in_region(in_region, spv::SelectionControlDontFlattenMask, builder);
    {
      spv::Id dest_dword_index = shr(dest_offset, 2);
      for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
        id_vector_temp.clear();
        // The only SSBO structure member.
        id_vector_temp.push_back(builder.makeIntConstant(0));
        id_vector_temp.push_back(builder.createUnaryOp(
            spv::OpBitcast, type_int, i ? add(dest_dword_index, uconst(1)) : dest_dword_index));
        builder.createStore(
            transform_value(packed[i]),
            builder.createAccessChain(spv::StorageClassUniform, edram_buffer, id_vector_temp));
      }
    }
    if_in_region.makeEndIf();
    }
  } else {
    // Write the packed value to the EDRAM buffer.
    spv::Id store_value = packed[0];
    if (format_is_64bpp) {
      id_vector_temp.clear();
      id_vector_temp.push_back(packed[0]);
      id_vector_temp.push_back(packed[1]);
      store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
    }
    id_vector_temp.clear();
    // The only SSBO structure member.
    id_vector_temp.push_back(builder.makeIntConstant(0));
    id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    builder.createStore(store_value, builder.createAccessChain(spv::StorageClassUniform,
                                                               edram_buffer, id_vector_temp));
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize, kDumpSamplesPerGroupX,
                           kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the pipeline, and store the handle even if creation fails not to try
  // to create it again later.
  VkPipelineLayout pipeline_layout;
  if (image) {
    pipeline_layout = key.is_depth ? resolve_to_image_pipeline_layout_depth_
                                   : resolve_to_image_pipeline_layout_color_;
  } else if (direct) {
    pipeline_layout =
        key.is_depth ? direct_resolve_pipeline_layout_depth_ : direct_resolve_pipeline_layout_color_;
  } else {
    pipeline_layout = key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_;
  }
  VkPipeline pipeline = ui::vulkan::util::CreateComputePipeline(
      command_processor_.GetVulkanDevice(), pipeline_layout,
      reinterpret_cast<const uint32_t*>(shader_code.data()), sizeof(uint32_t) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create a render target {} pipeline "
        "for {}-sample render targets with format {}",
        image ? "resolve to image" : direct ? "direct resolve" : "dumping",
        UINT32_C(1) << uint32_t(key.msaa_samples),
        key.is_depth ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                     : xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
  }
  return pipeline;
}

VkPipeline VulkanRenderTargetCache::GetDirectResolvePipeline(DirectResolvePipelineKey key) {
  auto pipeline_it = direct_resolve_pipelines_.find(key);
  if (pipeline_it != direct_resolve_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  // Only the "fast" copy shaders are fused so far. They write the EDRAM sample
  // to the destination unchanged apart from the red/blue swap and the
  // endianness, which is exactly what the shared packing already produces - the
  // format-converting "full" shaders would need their conversion replicated
  // here as well. Multisampled sources are not fused either: the copy shader
  // picks one sample by offsetting within the EDRAM, and with resolution
  // scaling the relation between a sample offset and a scaled texel needs to be
  // settled before it can be reproduced from the dump-side dispatch.
  // 64bpp is left out for now on top of that: its EDRAM tile is half as wide,
  // but the resolve region's offset within the tile is still expressed in
  // 32bpp-equivalent samples, and that conversion is untested here.
  bool copy_shader_fusable =
      key.copy_shader == draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA;
  if (copy_shader_fusable && key.dump_pipeline_key.msaa_samples == xenos::MsaaSamples::k1X) {
    pipeline = BuildRenderTargetSamplingPipeline(key.dump_pipeline_key, &key);
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                                                     draw_util::ResolveCopyShaderIndex copy_shader,
                                                     bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  if (direct_resolve_pipeline_layout_color_ == VK_NULL_HANDLE ||
      direct_resolve_pipeline_layout_depth_ == VK_NULL_HANDLE) {
    return false;
  }

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyDispatchesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_, direct_resolve_dispatches_);
  if (direct_resolve_dispatches_.empty()) {
    return false;
  }

  // Every tile of the resolve region must be owned by a render target. Where
  // no render target owns one, the dump path still copies whatever the EDRAM
  // buffer holds for it - possibly written by an earlier dump - while the
  // fused path would leave the destination untouched, so fall back rather than
  // resolve a partial region.
  uint64_t covered_tiles = 0;
  for (const ResolveCopyDispatch& resolve_copy_dispatch : direct_resolve_dispatches_) {
    covered_tiles += uint64_t(resolve_copy_dispatch.dispatch.width_tiles) *
                     resolve_copy_dispatch.dispatch.height_tiles;
  }
  if (covered_tiles != uint64_t(dump_row_length_used) * dump_rows) {
    return false;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    const auto* render_target = static_cast<const VulkanRenderTarget*>(rectangle.render_target);
    if (render_target == nullptr) {
      return false;
    }
    RenderTargetKey rt_key = render_target->key();
    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = rt_key.msaa_samples;
    dump_pipeline_key.resource_format = rt_key.resource_format;
    dump_pipeline_key.is_depth = rt_key.is_depth;
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    if (GetDirectResolvePipeline(direct_pipeline_key) == VK_NULL_HANDLE) {
      return false;
    }
  }

  return true;
}

void VulkanRenderTargetCache::IssueDirectResolveCopy(
    const draw_util::ResolveInfo& resolve_info, draw_util::ResolveCopyShaderIndex copy_shader,
    draw_util::ResolveCopyShaderConstants copy_shader_constants,
    VkDescriptorSet descriptor_set_dest, bool draw_resolution_scaled,
    uint32_t dest_binding_offset) {
  assert_true(GetPath() == Path::kHostRenderTargets);
  assert_false(direct_resolve_dispatches_.empty());

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);

  // The scaled destination is bound at its own base, so the shader adds
  // nothing; the unscaled one is bound at an offset into the shared memory.
  DirectResolvePushConstants push_constants;
  push_constants.resolve = copy_shader_constants;
  push_constants.resolve.dest_base =
      draw_resolution_scaled ? 0 : (copy_shader_constants.dest_base - dest_binding_offset);
  push_constants.height_div_8 = resolve_info.height_div_8;

  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  VkPipelineLayout last_pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  const RenderTarget* last_render_target = nullptr;
  for (const ResolveCopyDispatch& resolve_copy_dispatch : direct_resolve_dispatches_) {
    const ResolveCopyDumpRectangle& rectangle =
        dump_rectangles_[resolve_copy_dispatch.rectangle_index];
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();

    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = rt_key.msaa_samples;
    dump_pipeline_key.resource_format = rt_key.resource_format;
    dump_pipeline_key.is_depth = rt_key.is_depth;
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    VkPipeline pipeline = GetDirectResolvePipeline(direct_pipeline_key);
    // TryResolveCopyDirectly has checked every rectangle already.
    assert_true(pipeline != VK_NULL_HANDLE);
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout =
        rt_key.is_depth ? direct_resolve_pipeline_layout_depth_ : direct_resolve_pipeline_layout_color_;
    if (last_pipeline_layout != pipeline_layout) {
      last_pipeline_layout = pipeline_layout;
      last_source_descriptor_set = VK_NULL_HANDLE;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetEdram, 1, &descriptor_set_dest, 0,
                                             nullptr);
    }

    if (last_render_target != rectangle.render_target) {
      last_render_target = rectangle.render_target;
      vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkDescriptorSet source_descriptor_set = vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetSource, 1, &source_descriptor_set, 0,
                                             nullptr);
    }

    push_constants.source_base_tiles = rt_key.base_tiles;
    push_constants.source_pitch_tiles = rt_key.GetPitchTiles();
    push_constants.source_x_offset_tiles = wide_resolve_source_x_offset_tiles_;
    if (wide_resolve_source_x_offset_tiles_ && WideLogTake()) {
      REXGPU_ERROR("[WIDE]   path direct offset {} {}", wide_resolve_source_x_offset_tiles_,
                   rt_key.is_depth ? "depth" : "color");
    }
    push_constants.dispatch_first_tile = dump_base + resolve_copy_dispatch.dispatch.offset;
    command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(push_constants), &push_constants);

    command_processor_.SubmitBarriers(true);
    command_buffer.CmdVkDispatch(
        (draw_resolution_scale_x() * (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
             resolve_copy_dispatch.dispatch.width_tiles +
         (kDumpSamplesPerGroupX - 1)) /
            kDumpSamplesPerGroupX,
        (draw_resolution_scale_y() * xenos::kEdramTileHeightSamples *
             resolve_copy_dispatch.dispatch.height_tiles +
         (kDumpSamplesPerGroupY - 1)) /
            kDumpSamplesPerGroupY,
        1);
  }
}

VkPipeline VulkanRenderTargetCache::GetResolveToImagePipeline(ResolveToImagePipelineKey key) {
  auto pipeline_it = resolve_to_image_pipelines_.find(key);
  if (pipeline_it != resolve_to_image_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = BuildRenderTargetSamplingPipeline(key.dump_pipeline_key, nullptr, &key);
  resolve_to_image_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::TryPrepareResolveComputeToTexture(
    const draw_util::ResolveInfo& resolve_info, draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    VulkanTextureCache& texture_cache, bool draw_resolution_scaled) {
  ++resolve_compute_attempt_count_;
  resolve_copy_to_texture_rt_ = nullptr;
  auto reject = [this](const char* reason) {
    for (auto& entry : resolve_compute_rejects_) {
      if (!std::strcmp(entry.first, reason)) {
        ++entry.second;
        return false;
      }
    }
    resolve_compute_rejects_.emplace_back(reason, 1);
    return false;
  };
  // The fast 32bpp shaders are what the sampling shader's packing replaces:
  // depth, or an unbiased bitwise-equivalent colour format. 64bpp region
  // addressing is untested in the fused path.
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA &&
      copy_shader != draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA) {
    return reject("copy shader");
  }
  const reg::RB_COPY_DEST_INFO& dest_info = resolve_info.copy_dest_info;
  if (dest_info.copy_dest_array) {
    return reject("array");
  }
  if (uint32_t(dest_info.copy_dest_endian) >= 4) {
    return reject("endian128");
  }
  bool is_depth = resolve_info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram_info =
      is_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
  xenos::CopySampleSelect sample_select =
      resolve_info.copy_dest_coordinate_info.copy_sample_select;
  xenos::TextureFormat dest_format = xenos::TextureFormat(dest_info.copy_dest_format);
  switch (dest_format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      break;
    default:
      return reject("dest format");
  }
  if (!is_depth && xenos::ColorRenderTargetFormat(edram_info.format) ==
                       xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
      gamma_render_target_as_unorm16_) {
    return reject("gamma unorm16");
  }

  // One owner for the whole span.
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.size() != 1) {
    return reject(dump_rectangles_.empty() ? "no owner" : "several owners");
  }
  const ResolveCopyDumpRectangle& rectangle = dump_rectangles_[0];
  if (!rectangle.render_target || rectangle.row_first != 0 || rectangle.rows != dump_rows ||
      rectangle.row_first_start != 0 || rectangle.row_last_end != dump_row_length_used) {
    return reject("partial owner");
  }
  auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
  RenderTargetKey rt_key = vulkan_rt.key();
  if (!rt_key.is_depth && rt_key.Is64bpp()) {
    return reject("owner 64bpp");
  }
  if (rt_key.GetPitchTiles() != dump_pitch) {
    return reject("owner pitch");
  }
  if (dump_base < rt_key.base_tiles) {
    return reject("owner base");
  }
  // A colour owner must pack the dest's bits; a depth owner packs
  // stencil | depth << 8, which is the guest dword of both the depth texture
  // formats and of a depth-as-colour read.
  if (!rt_key.is_depth && !is_depth &&
      rt_key.GetColorFormat() != xenos::ColorRenderTargetFormat(edram_info.format)) {
    return reject("owner format");
  }
  if (!rt_key.is_depth && is_depth) {
    return reject("colour owner for depth");
  }
  // A multisampled resolve view: the shader maps the view's samples to
  // destination pixels and keeps the selected one - that works whatever the
  // owner's sample layout is, since the fetch maps the same EDRAM position
  // through the owner. Averaging a group, though, fetches the group's
  // samples at one source pixel, which is the right set only when the owner
  // lays its samples out the way the view does.
  if (edram_info.msaa_samples != xenos::MsaaSamples::k1X &&
      !xenos::IsSingleCopySampleSelected(sample_select)) {
    if (is_depth || rt_key.is_depth) {
      return reject("averaging depth");
    }
    if (rt_key.msaa_samples != edram_info.msaa_samples) {
      return reject("averaging owner mismatch");
    }
    bool source_is_uint = false;
    GetColorOwnershipTransferVulkanFormat(rt_key.GetColorFormat(), rt_key.msaa_samples,
                                          &source_is_uint);
    if (source_is_uint) {
      return reject("averaging uint source");
    }
  }

  uint32_t width = resolve_info.coordinate_info.width_div_8 << xenos::kResolveAlignmentPixelsLog2;
  uint32_t height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
  const char* texture_reject_reason;
  if (!texture_cache.PrepareResolveCopyDestinations(
          resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length, dest_format, uint32_t(dest_info.copy_dest_endian),
          resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32, 2,
          resolve_info.copy_dest_coordinate_info.offset_x_div_8,
          resolve_info.copy_dest_coordinate_info.offset_y_div_8, draw_resolution_scaled, width,
          height, VK_FORMAT_UNDEFINED, false, texture_reject_reason)) {
    return reject(texture_reject_reason);
  }

  // The destinations must be writable as storage images before the resolve
  // range is marked as resolved - a failure after that would leave them
  // declared current but never written.
  VulkanTextureCache::ResolveComputeHostFormat host_format;
  if (!texture_cache.CheckResolveComputeDestinations(host_format)) {
    return reject("destination not storage-writable");
  }
  ResolveToImagePipelineKey pipeline_key;
  pipeline_key.dump_pipeline_key.msaa_samples = rt_key.msaa_samples;
  pipeline_key.dump_pipeline_key.resource_format = rt_key.resource_format;
  pipeline_key.dump_pipeline_key.is_depth = rt_key.is_depth;
  pipeline_key.host_format = uint32_t(host_format);
  pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
  pipeline_key.view_msaa_samples = edram_info.msaa_samples;
  pipeline_key.sample_select = sample_select;
  if (GetResolveToImagePipeline(pipeline_key) == VK_NULL_HANDLE) {
    return reject("no pipeline");
  }
  resolve_compute_pipeline_key_ = pipeline_key;
  resolve_compute_push_constants_.resolve = copy_shader_constants;
  resolve_compute_push_constants_.resolve.dest_base = 0;
  resolve_compute_push_constants_.source_base_tiles = rt_key.base_tiles;
  resolve_compute_push_constants_.source_pitch_tiles = rt_key.GetPitchTiles();
  resolve_compute_push_constants_.source_x_offset_tiles = wide_resolve_source_x_offset_tiles_;
  if (wide_resolve_source_x_offset_tiles_ && WideLogTake()) {
    REXGPU_ERROR("[WIDE]   path compute-to-image offset {} {}", wide_resolve_source_x_offset_tiles_,
                 rt_key.is_depth ? "depth" : "color");
  }
  resolve_compute_push_constants_.dispatch_first_tile = dump_base;
  resolve_compute_push_constants_.height_div_8 = resolve_info.height_div_8;
  resolve_copy_to_texture_rt_ = &vulkan_rt;
  resolve_copy_to_texture_dump_row_length_ = dump_row_length_used;
  resolve_copy_to_texture_dump_rows_ = dump_rows;
  return true;
}

void VulkanRenderTargetCache::IssueResolveComputeToTexture(VulkanTextureCache& texture_cache) {
  assert_not_null(resolve_copy_to_texture_rt_);
  VulkanRenderTarget& vulkan_rt = *resolve_copy_to_texture_rt_;
  resolve_copy_to_texture_rt_ = nullptr;
  RenderTargetKey rt_key = vulkan_rt.key();

  texture_cache.BeginResolveComputeDestinations(resolve_compute_destinations_);
  // Checked in TryPrepareResolveComputeToTexture.
  VkPipeline pipeline = GetResolveToImagePipeline(resolve_compute_pipeline_key_);
  assert_true(pipeline != VK_NULL_HANDLE);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_processor_.GpuTimerMark("resolve compute to texture");

  // Source: sampled by compute.
  command_processor_.PushImageMemoryBarrier(
      vulkan_rt.image(),
      ui::vulkan::util::InitializeSubresourceRange(
          rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                          : VK_IMAGE_ASPECT_COLOR_BIT),
      vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, vulkan_rt.current_layout(),
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  command_processor_.SubmitBarriers(true);

  VkPipelineLayout pipeline_layout = rt_key.is_depth ? resolve_to_image_pipeline_layout_depth_
                                                     : resolve_to_image_pipeline_layout_color_;
  command_processor_.BindExternalComputePipeline(pipeline);
  VkDescriptorSet source_descriptor_set = vulkan_rt.GetDescriptorSetTransferSource();
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                         kDumpDescriptorSetSource, 1, &source_descriptor_set, 0,
                                         nullptr);
  uint32_t tile_width_samples = (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
                                draw_resolution_scale_x();
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  uint32_t group_count_x =
      (tile_width_samples * resolve_copy_to_texture_dump_row_length_ + kDumpSamplesPerGroupX - 1) /
      kDumpSamplesPerGroupX;
  uint32_t group_count_y =
      (tile_height_samples * resolve_copy_to_texture_dump_rows_ + kDumpSamplesPerGroupY - 1) /
      kDumpSamplesPerGroupY;
  for (const VulkanTextureCache::ResolveComputeDestination& destination :
       resolve_compute_destinations_) {
    VkDescriptorSet image_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
        VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageImageCompute);
    if (image_descriptor_set == VK_NULL_HANDLE) {
      continue;
    }
    VkDescriptorImageInfo image_info;
    image_info.sampler = VK_NULL_HANDLE;
    image_info.imageView = destination.storage_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write;
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = nullptr;
    write.dstSet = image_descriptor_set;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image_info;
    write.pBufferInfo = nullptr;
    write.pTexelBufferView = nullptr;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                           kDumpDescriptorSetEdram, 1, &image_descriptor_set, 0,
                                           nullptr);
    ResolveToImagePushConstants push_constants;
    push_constants.direct = resolve_compute_push_constants_;
    push_constants.image_x = destination.x;
    push_constants.image_y = destination.y;
    push_constants.image_width = destination.x + destination.width;
    push_constants.image_height = destination.y + destination.height;
    command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(push_constants), &push_constants);
    command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
  }
  ++resolve_compute_count_;
  COUNT_profile_add("gpu/render_target_cache/resolve_compute_to_texture", 1);
  texture_cache.EndResolveComputeDestinations();
}

bool VulkanRenderTargetCache::TryPrepareResolveCopyToTexture(
    const draw_util::ResolveInfo& resolve_info, VulkanTextureCache& texture_cache,
    bool draw_resolution_scaled) {
  ++resolve_copy_to_texture_attempt_count_;
  resolve_copy_to_texture_rt_ = nullptr;
  auto reject = [this](const char* reason) {
    for (auto& entry : resolve_copy_to_texture_rejects_) {
      if (!std::strcmp(entry.first, reason)) {
        ++entry.second;
        return false;
      }
    }
    resolve_copy_to_texture_rejects_.emplace_back(reason, 1);
    return false;
  };

  // The copy is a bit copy: everything the copy shader would have changed on
  // the way to memory, and everything the load shader would have undone on the
  // way back, has to cancel out.
  if (resolve_info.IsCopyingDepth()) {
    return reject("depth");
  }
  const reg::RB_COPY_DEST_INFO& dest_info = resolve_info.copy_dest_info;
  if (dest_info.copy_dest_array) {
    return reject("array");
  }
  if (dest_info.copy_dest_exp_bias) {
    return reject("exp bias");
  }
  const draw_util::ResolveEdramInfo& edram_info = resolve_info.color_edram_info;
  xenos::ColorRenderTargetFormat rt_format = xenos::ColorRenderTargetFormat(edram_info.format);
  xenos::TextureFormat dest_format = xenos::TextureFormat(dest_info.copy_dest_format);
  // The red/blue swap (Direct3D 9's A8R8G8B8, so every AC6 resolve) is not
  // applied to the copied bits; the texture's views compose it instead, which
  // is exact for four 8-bit channels only.
  bool rb_swap = bool(dest_info.copy_dest_swap);
  if (rb_swap && dest_format != xenos::TextureFormat::k_8_8_8_8) {
    return reject("swap");
  }
  // Texture keys hold a 2-bit Endian; the 64/128-bit swaps have no texture
  // equivalent.
  if (uint32_t(dest_info.copy_dest_endian) >= 4) {
    return reject("endian128");
  }
  // Multisampled sources: only an averaging sample select over all the
  // samples is a hardware resolve. Guest 2x is native 2-sample only when the
  // device supports it; emulated as 4x it writes samples 0 and 3 alone, and
  // the hardware average would mix in the two unwritten ones.
  xenos::MsaaSamples msaa_samples = edram_info.msaa_samples;
  if (msaa_samples != xenos::MsaaSamples::k1X) {
    if (!REXCVAR_GET(vulkan_resolve_to_texture_msaa)) {
      return reject("msaa (disabled)");
    }
    xenos::CopySampleSelect sample_select =
        resolve_info.copy_dest_coordinate_info.copy_sample_select;
    bool averaging_all =
        (msaa_samples == xenos::MsaaSamples::k2X && sample_select == xenos::CopySampleSelect::k01) ||
        (msaa_samples == xenos::MsaaSamples::k4X &&
         sample_select == xenos::CopySampleSelect::k0123);
    if (!averaging_all) {
      if (resolve_copy_to_texture_msaa_log_count_ < 12) {
        ++resolve_copy_to_texture_msaa_log_count_;
        REXGPU_ERROR("[RTT] msaa: {}x source, sample select {}, {} -> fmt {}, {}x{} px, dest {:08X}",
                     1u << uint32_t(msaa_samples), uint32_t(sample_select),
                     xenos::GetColorRenderTargetFormatName(rt_format), uint32_t(dest_format),
                     resolve_info.coordinate_info.width_div_8 << 3, resolve_info.height_div_8 << 3,
                     resolve_info.copy_dest_base);
      }
      return reject("msaa sample select");
    }
    if (msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_) {
      return reject("msaa 2x emulated");
    }
  }
  if (!xenos::IsColorResolveFormatBitwiseEquivalent(rt_format, xenos::ColorFormat(dest_format))) {
    return reject("format");
  }

  // One render target must own the whole tile span - the dump path would
  // otherwise read whatever the EDRAM buffer holds for the rest.
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.size() != 1) {
    return reject(dump_rectangles_.empty() ? "no owner" : "several owners");
  }
  const ResolveCopyDumpRectangle& rectangle = dump_rectangles_[0];
  if (!rectangle.render_target || rectangle.row_first != 0 || rectangle.rows != dump_rows ||
      rectangle.row_first_start != 0 || rectangle.row_last_end != dump_row_length_used) {
    return reject("partial owner");
  }
  auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
  RenderTargetKey rt_key = vulkan_rt.key();
  if (rt_key.is_depth) {
    return reject("owner kind");
  }
  if (rt_key.msaa_samples != msaa_samples) {
    return reject("owner msaa");
  }
  // The owner's pixels are what the dump would have packed; they are only the
  // resolve's bits if the formats agree.
  if (rt_key.GetColorFormat() != rt_format) {
    return reject("owner format");
  }
  if (rt_key.GetPitchTiles() != dump_pitch) {
    return reject("owner pitch");
  }
  if (dump_base < rt_key.base_tiles) {
    return reject("owner base");
  }
  VkFormat rt_vk_format = GetColorVulkanFormat(rt_format);
  if (rt_vk_format == VK_FORMAT_UNDEFINED) {
    return reject("owner host format");
  }

  // Source rectangle in unscaled render target pixels (CPU mirror of the dump
  // shader's addressing): an EDRAM tile is 80x16 samples, so 80x16 pixels at
  // 1x, 80x8 at 2x, 40x8 at 4x (and half as wide at 64bpp); the resolve
  // offsets within the tile are in pixels.
  uint32_t width = resolve_info.coordinate_info.width_div_8 << xenos::kResolveAlignmentPixelsLog2;
  uint32_t height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
  uint32_t rt_pitch_tiles = rt_key.GetPitchTiles();
  uint32_t source_tile = dump_base - rt_key.base_tiles;
  uint32_t tile_width = (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) >>
                        uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t tile_height =
      xenos::kEdramTileHeightSamples >> uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t src_x = (source_tile % rt_pitch_tiles) * tile_width +
                   (resolve_info.coordinate_info.edram_offset_x_div_8
                    << xenos::kResolveAlignmentPixelsLog2);
  uint32_t src_y = (source_tile / rt_pitch_tiles) * tile_height +
                   (resolve_info.coordinate_info.edram_offset_y_div_8
                    << xenos::kResolveAlignmentPixelsLog2);
  // AC6 wide world target: the second tile's resolve reads the right half.
  const uint32_t wide_multiplier = IsWideKey(rt_key) ? 2 : 1;
  src_x += wide_resolve_source_x_offset_tiles_ * tile_width;
  if (wide_resolve_source_x_offset_tiles_ && WideLogTake()) {
    REXGPU_ERROR("[WIDE]   path image-copy offset {} src_x {}", wide_resolve_source_x_offset_tiles_,
                 src_x);
  }
  if (src_x + width > rt_key.GetWidth() * wide_multiplier ||
      src_y + height > GetRenderTargetHeight(rt_key.pitch_tiles_at_32bpp, rt_key.msaa_samples)) {
    return reject("source outside owner");
  }

  uint32_t bpp_log2 = rt_key.Is64bpp() ? 3 : 2;
  const char* texture_reject_reason;
  if (!texture_cache.PrepareResolveCopyDestinations(
          resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length, dest_format, uint32_t(dest_info.copy_dest_endian),
          resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32, bpp_log2,
          resolve_info.copy_dest_coordinate_info.offset_x_div_8,
          resolve_info.copy_dest_coordinate_info.offset_y_div_8, draw_resolution_scaled, width,
          height, rt_vk_format, rb_swap, texture_reject_reason)) {
    return reject(texture_reject_reason);
  }

  // Host pixels: the render target and a scaled-resolve texture are scaled
  // alike, so the copy is 1:1.
  resolve_copy_to_texture_multisampled_ = msaa_samples != xenos::MsaaSamples::k1X;
  resolve_copy_to_texture_source_x_ = src_x * draw_resolution_scale_x();
  resolve_copy_to_texture_source_y_ = src_y * draw_resolution_scale_y();
  // Half-pixel offset fill: with resolution scaling the guest's half-pixel
  // offset becomes a full-pixel one and leaves the left / top edge of the
  // region uncovered; the copy shaders fill destination columns and rows
  // below the fill from the first surely covered column / row, and the copies
  // have to as well or the tile seam comes back.
  resolve_copy_to_texture_fill_x_ = 0;
  resolve_copy_to_texture_fill_y_ = 0;
  if (draw_resolution_scaled && edram_info.fill_half_pixel_offset) {
    resolve_copy_to_texture_fill_x_ = draw_resolution_scale_x() >> 1;
    resolve_copy_to_texture_fill_y_ = draw_resolution_scale_y() >> 1;
  }

  resolve_copy_to_texture_rt_ = &vulkan_rt;
  ++resolve_copy_to_texture_success_count_;
  if (resolve_copy_to_texture_success_count_ == 1) {
    REXGPU_ERROR("VulkanRenderTargetCache: resolving into texture images by copy");
  }
  return true;
}

void VulkanRenderTargetCache::IssueResolveCopyToTexture(VulkanTextureCache& texture_cache) {
  assert_not_null(resolve_copy_to_texture_rt_);
  VulkanRenderTarget& vulkan_rt = *resolve_copy_to_texture_rt_;
  resolve_copy_to_texture_rt_ = nullptr;
  command_processor_.PushImageMemoryBarrier(
      vulkan_rt.image(), ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
      vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
      vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT, vulkan_rt.current_layout(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  // Submits the barriers (ending any render pass) before each copy.
  RenderTargetKey source_rt_key = vulkan_rt.key();
  VulkanTextureCache::ResolveCopySourceInfo source_info;
  source_info.image = vulkan_rt.image();
  source_info.format = source_rt_key.is_depth
                           ? GetDepthVulkanFormat(source_rt_key.GetDepthFormat())
                           : GetColorVulkanFormat(source_rt_key.GetColorFormat());
  source_info.width = source_rt_key.GetWidth() * draw_resolution_scale_x() *
                      (IsWideKey(source_rt_key) ? 2 : 1);
  source_info.height =
      GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp, source_rt_key.msaa_samples) *
      draw_resolution_scale_y();
  source_info.multisampled = resolve_copy_to_texture_multisampled_;
  texture_cache.IssueResolveCopies(source_info, resolve_copy_to_texture_source_x_,
                                   resolve_copy_to_texture_source_y_,
                                   resolve_copy_to_texture_fill_x_, resolve_copy_to_texture_fill_y_);
  COUNT_profile_add("gpu/render_target_cache/resolve_copies_to_texture", 1);
}

void VulkanRenderTargetCache::LogResolveCopyToTextureStats() {
  std::string rejects;
  for (const auto& entry : resolve_copy_to_texture_rejects_) {
    rejects += fmt::format(" [{}: {}]", entry.first, entry.second);
  }
  // Error level so the tally survives the performance-mode log level.
  REXGPU_ERROR("VulkanRenderTargetCache: resolve copies to texture images: {} of {} attempts;{}",
              resolve_copy_to_texture_success_count_, resolve_copy_to_texture_attempt_count_,
              rejects.empty() ? " no rejections" : rejects.c_str());
}

VkPipeline VulkanRenderTargetCache::GetStencilComputePipeline(TransferShaderKey key) {
  assert_true(key.stencil_compute);
  auto pipeline_it = stencil_compute_pipelines_.find(key);
  if (pipeline_it != stencil_compute_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule shader_module = GetTransferShader(key);
  if (shader_module != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    VkComputePipelineCreateInfo pipeline_create_info;
    pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext = nullptr;
    pipeline_create_info.flags = 0;
    pipeline_create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_create_info.stage.pNext = nullptr;
    pipeline_create_info.stage.flags = 0;
    pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_create_info.stage.module = shader_module;
    pipeline_create_info.stage.pName = "main";
    pipeline_create_info.stage.pSpecializationInfo = nullptr;
    pipeline_create_info.layout = key.mode == TransferMode::kDepthToStencilBit
                                      ? stencil_compute_pipeline_layout_depth_
                                      : stencil_compute_pipeline_layout_color_;
    pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
    pipeline_create_info.basePipelineIndex = -1;
    if (dfn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                     &pipeline) != VK_SUCCESS) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the stencil transfer compute "
                   "pipeline 0x{:08X}",
                   key.key);
      pipeline = VK_NULL_HANDLE;
    }
  }
  stencil_compute_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::RecordStencilBufferTransfers(
    VulkanRenderTarget& dest_vulkan_rt, const std::vector<Transfer>& transfers,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
  assert_true(dest_rt_key.is_depth);
  assert_true(dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X);
  uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
  uint32_t scale_x = draw_resolution_scale_x();
  uint32_t scale_y = draw_resolution_scale_y();

  // Gather the rectangles, their pipelines and their places in the buffer.
  struct Job {
    const Transfer* transfer;
    VkPipeline pipeline;
    Transfer::Rectangle rectangle;  // Scaled.
    VkDeviceSize buffer_offset;
    uint32_t row_pitch;
  };
  std::vector<Job> jobs;
  VkDeviceSize buffer_size = 0;
  for (const Transfer& transfer : transfers) {
    assert_not_null(transfer.source);
    auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
    RenderTargetKey source_rt_key = source_vulkan_rt.key();
    TransferShaderKey shader_key;
    shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
    shader_key.dest_resource_format = dest_rt_key.resource_format;
    shader_key.source_msaa_samples = source_rt_key.msaa_samples;
    shader_key.source_resource_format = source_rt_key.resource_format;
    shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
    shader_key.stencil_compute = 1;
    shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToStencilBit
                                             : TransferMode::kColorToStencilBit;
    VkPipeline pipeline = GetStencilComputePipeline(shader_key);
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
    Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
    uint32_t rectangle_count =
        transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples,
                               false, rectangles, resolve_clear_rectangle);
    for (uint32_t i = 0; i < rectangle_count; ++i) {
      Job& job = jobs.emplace_back();
      job.transfer = &transfer;
      job.pipeline = pipeline;
      job.rectangle.x_pixels = rectangles[i].x_pixels * scale_x;
      job.rectangle.y_pixels = rectangles[i].y_pixels * scale_y;
      job.rectangle.width_pixels = rectangles[i].width_pixels * scale_x;
      job.rectangle.height_pixels = rectangles[i].height_pixels * scale_y;
      // Dwords are shared by four pixels; rows start dword-aligned so the
      // buffer-image copy can address them.
      job.row_pitch = rex::align(job.rectangle.width_pixels, UINT32_C(4));
      job.buffer_offset = buffer_size;
      buffer_size += VkDeviceSize(job.row_pitch) * job.rectangle.height_pixels;
    }
  }
  if (jobs.empty()) {
    return true;
  }

  VulkanCommandProcessor::ScratchBufferAcquisition scratch_buffer_acquisition(
      command_processor_.AcquireScratchGpuBuffer(buffer_size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                 VK_ACCESS_SHADER_WRITE_BIT));
  VkBuffer scratch_buffer = scratch_buffer_acquisition.buffer();
  if (scratch_buffer == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorSet descriptor_set_output = command_processor_.AllocateSingleTransientDescriptor(
      VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
  if (descriptor_set_output == VK_NULL_HANDLE) {
    return false;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  {
    VkDescriptorBufferInfo buffer_info;
    buffer_info.buffer = scratch_buffer;
    buffer_info.offset = 0;
    buffer_info.range = buffer_size;
    VkWriteDescriptorSet write;
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = nullptr;
    write.dstSet = descriptor_set_output;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pImageInfo = nullptr;
    write.pBufferInfo = &buffer_info;
    write.pTexelBufferView = nullptr;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }

  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_processor_.GpuTimerMark("edram transfer stencil (compute)");

  // The sources were transitioned for fragment reads by the caller; extend
  // that to compute.
  for (const Transfer& transfer : transfers) {
    auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
    constexpr VkPipelineStageFlags kStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    command_processor_.PushImageMemoryBarrier(
        source_vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            source_vulkan_rt.key().is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        source_vulkan_rt.current_stage_mask(), kStageMask, source_vulkan_rt.current_access_mask(),
        VK_ACCESS_SHADER_READ_BIT, source_vulkan_rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    source_vulkan_rt.SetUsage(kStageMask, VK_ACCESS_SHADER_READ_BIT,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  command_processor_.SubmitBarriers(true);
  command_processor_.GpuTimerMark("edram transfer stencil (compute) dispatch");

  VkPipeline last_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout last_layout = VK_NULL_HANDLE;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  for (const Job& job : jobs) {
    auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(job.transfer->source);
    RenderTargetKey source_rt_key = source_vulkan_rt.key();
    VkPipelineLayout layout = source_rt_key.is_depth ? stencil_compute_pipeline_layout_depth_
                                                     : stencil_compute_pipeline_layout_color_;
    if (last_pipeline != job.pipeline) {
      last_pipeline = job.pipeline;
      command_processor_.BindExternalComputePipeline(job.pipeline);
    }
    if (last_layout != layout) {
      last_layout = layout;
      last_source_descriptor_set = VK_NULL_HANDLE;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                                             &descriptor_set_output, 0, nullptr);
    }
    VkDescriptorSet source_descriptor_set = source_vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1,
                                             &source_descriptor_set, 0, nullptr);
    }
    StencilComputePushConstants push_constants;
    push_constants.address.dest_pitch = dest_pitch_tiles;
    push_constants.address.source_pitch = source_rt_key.GetPitchTiles();
    push_constants.address.source_to_dest =
        int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
    push_constants.rect_x = job.rectangle.x_pixels;
    push_constants.rect_y = job.rectangle.y_pixels;
    push_constants.rect_width = job.rectangle.width_pixels;
    push_constants.rect_height = job.rectangle.height_pixels;
    push_constants.row_pitch = job.row_pitch;
    push_constants.buffer_offset = uint32_t(job.buffer_offset);
    command_buffer.CmdVkPushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(push_constants), &push_constants);
    command_buffer.CmdVkDispatch(
        (job.rectangle.width_pixels + kStencilComputeGroupSizeX - 1) / kStencilComputeGroupSizeX,
        (job.rectangle.height_pixels + kStencilComputeGroupSizeY - 1) / kStencilComputeGroupSizeY,
        1);
  }

  // Copy the bytes into the stencil aspect. The layout the copy goes through
  // decides whether the driver decompresses the depth/stencil image around it.
  const VkImageLayout kStencilCopyDestLayout = REXCVAR_GET(vulkan_edram_stencil_copy_general)
                                                   ? VK_IMAGE_LAYOUT_GENERAL
                                                   : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  command_processor_.GpuTimerMark("edram transfer stencil (compute) copy");
  command_processor_.PushBufferMemoryBarrier(
      scratch_buffer, 0, VK_WHOLE_SIZE,
      scratch_buffer_acquisition.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      scratch_buffer_acquisition.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT),
      VK_ACCESS_TRANSFER_READ_BIT);
  command_processor_.PushImageMemoryBarrier(
      dest_vulkan_rt.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                   VK_IMAGE_ASPECT_STENCIL_BIT),
      dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
      dest_vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
      dest_vulkan_rt.current_layout(), kStencilCopyDestLayout);
  dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kStencilCopyDestLayout);
  command_processor_.SubmitBarriers(false);
  VkBufferImageCopy* regions = command_buffer.CmdCopyBufferToImageEmplace(
      scratch_buffer, dest_vulkan_rt.image(), kStencilCopyDestLayout, uint32_t(jobs.size()));
  for (size_t i = 0; i < jobs.size(); ++i) {
    const Job& job = jobs[i];
    VkBufferImageCopy& region = regions[i];
    region.bufferOffset = job.buffer_offset;
    region.bufferRowLength = job.row_pitch;
    region.bufferImageHeight = job.rectangle.height_pixels;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = int32_t(job.rectangle.x_pixels);
    region.imageOffset.y = int32_t(job.rectangle.y_pixels);
    region.imageOffset.z = 0;
    region.imageExtent.width = job.rectangle.width_pixels;
    region.imageExtent.height = job.rectangle.height_pixels;
    region.imageExtent.depth = 1;
  }
  COUNT_profile_add("gpu/edram_transfer_stencil_compute_rects", int64_t(jobs.size()));

  // Back to the draw usage for the depth draws of the same transfer.
  {
    VkPipelineStageFlags dest_dst_stage_mask;
    VkAccessFlags dest_dst_access_mask;
    VkImageLayout dest_new_layout;
    dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
    command_processor_.PushImageMemoryBarrier(
        dest_vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                     VK_IMAGE_ASPECT_STENCIL_BIT),
        dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
        dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
        dest_vulkan_rt.current_layout(), dest_new_layout);
    dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
  }
  return true;
}

bool VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                                uint32_t dump_rows, uint32_t dump_pitch) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)->SetTemporarySortIndex(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to sort the invocations.
  UseEdramBuffer(EdramBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                            : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, vulkan_rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.temporary_sort_index() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout =
        rt_key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_;

    // Only need to bind the EDRAM buffer once (relying on pipeline layout
    // compatibility).
    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetEdram, 1,
                                             &edram_storage_buffer_descriptor_set_, 0, nullptr);
    }

    VkDescriptorSet source_descriptor_set = vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetSource, 1, &source_descriptor_set, 0,
                                             nullptr);
    }

    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    pitches.source_x_offset_tiles = wide_resolve_source_x_offset_tiles_;
    if (wide_resolve_source_x_offset_tiles_ && WideLogTake()) {
      REXGPU_ERROR("[WIDE]   path dump offset {} {} pitches {:08X}", wide_resolve_source_x_offset_tiles_,
                   rt_key.is_depth ? "depth" : "color", pitches.pitches);
    }
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_bound = false;
    }
    if (!pitches_bound) {
      pitches_bound = true;
      command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                        sizeof(uint32_t) * kDumpPushConstantPitches,
                                        sizeof(last_pitches), &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                          sizeof(uint32_t) * kDumpPushConstantOffsets,
                                          sizeof(last_offsets), &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      command_buffer.CmdVkDispatch(
          (draw_resolution_scale_x() *
               (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          (draw_resolution_scale_y() * xenos::kEdramTileHeightSamples * dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1);
    }
    MarkEdramBufferModified();
  }
  return all_pipelines_available;
}


bool VulkanRenderTargetCache::IsWideKey(RenderTargetKey key) const {
  // AC6's world: surface pitch 640 at 2x MSAA is 8 tiles at 32bpp. Its colour
  // (8888) and depth (D24S8) targets both match; nothing else in the game
  // does. A 64bpp key at the same pitch would be 16 tiles and does not.
  return REXCVAR_GET(ac6_wide_world_target) && key.msaa_samples == xenos::MsaaSamples::k2X &&
         key.pitch_tiles_at_32bpp == 8;
}

bool VulkanRenderTargetCache::IsWideSurface(uint32_t surface_pitch,
                                            xenos::MsaaSamples msaa_samples) const {
  RenderTargetKey probe;
  probe.msaa_samples = msaa_samples;
  probe.pitch_tiles_at_32bpp = xenos::GetSurfacePitchTiles(surface_pitch, msaa_samples, false);
  return IsWideKey(probe);
}

void VulkanRenderTargetCache::NoteDrawIntoBoundTargets() {
  for (RenderTarget* rt : wide_bound_last_update_) {
    if (rt) {
      ++wide_draws_since_resolve_[rt];
    }
  }
}

}  // namespace rex::graphics::vulkan
