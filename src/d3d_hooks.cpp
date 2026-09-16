#include "d3d_hooks.h"

#include <algorithm>
#include <chrono>
#include <shared_mutex>
#include <unordered_set>

#include "ac6_backend_fixes/ac6_fullres_effects.h"
#include "ac6_register_shadow.h"

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ac6_d3d_trace, false, "AC6/Render",
                    "Log every D3D device state change and draw call");
REXCVAR_DEFINE_BOOL(ac6_render_capture, false, "AC6/Render",
                    "Capture per-frame draw, clear, and resolve records for the native renderer");

namespace {
const rex::LogCategoryId kLogGPU = rex::log::GPU;
}  // namespace

namespace {

std::shared_mutex g_shadow_mutex;
ac6::d3d::ShadowState g_shadow{};

ac6::d3d::DrawStats g_live_stats{};

std::shared_mutex g_snapshot_mutex;
ac6::d3d::DrawStatsSnapshot g_snapshot{};

std::shared_mutex g_capture_mutex;
ac6::d3d::FrameCaptureSnapshot g_capture_snapshot{};
ac6::d3d::FrameCaptureSummary g_capture_summary{};
uint64_t g_capture_live_frame_index = 1;
uint32_t g_capture_live_sequence = 0;
std::vector<ac6::d3d::DrawCallRecord> g_live_draws;
std::vector<ac6::d3d::ClearRecord> g_live_clears;
std::vector<ac6::d3d::ResolveRecord> g_live_resolves;

template <typename Container>
uint32_t CountNonZero(const Container& values) {
    uint32_t count = 0;
    for (const auto& value : values) {
        if (value) {
            ++count;
        }
    }
    return count;
}

uint32_t CountNonZeroStreams(const std::array<ac6::d3d::StreamBinding, ac6::d3d::kMaxStreams>& streams) {
    uint32_t count = 0;
    for (const auto& stream : streams) {
        if (stream.buffer) {
            ++count;
        }
    }
    return count;
}

uint32_t CountNonZeroSamplers(
    const std::array<ac6::d3d::SamplerBinding, ac6::d3d::kMaxSamplers>& samplers) {
    uint32_t count = 0;
    for (const auto& sampler : samplers) {
        if (sampler.mag_filter || sampler.min_filter || sampler.mip_filter || sampler.mip_level ||
            sampler.border_color) {
            ++count;
        }
    }
    return count;
}

void HashU32(uint64_t& hash, uint32_t value) {
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    hash ^= value;
    hash *= kFnvPrime;
}

void HashU64(uint64_t& hash, uint64_t value) {
    HashU32(hash, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    HashU32(hash, static_cast<uint32_t>(value >> 32));
}

uint64_t ComputeVertexFetchLayoutSignature(const ac6::d3d::ShadowState& shadow) {
    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    uint64_t hash = kFnvOffsetBasis;
    HashU32(hash, shadow.vertex_declaration);
    for (const auto& stream : shadow.streams) {
        HashU32(hash, stream.buffer);
        HashU32(hash, stream.offset);
        HashU32(hash, stream.stride);
    }
    return hash;
}

uint64_t ComputeTextureFetchLayoutSignature(const ac6::d3d::ShadowState& shadow) {
    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    uint64_t hash = kFnvOffsetBasis;
    for (uint32_t texture : shadow.texture_fetch_ptrs) {
        HashU32(hash, texture);
    }
    return hash;
}

uint64_t ComputeResourceBindingSignature(const ac6::d3d::ShadowState& shadow) {
    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    uint64_t hash = kFnvOffsetBasis;
    for (uint32_t target : shadow.render_targets) {
        HashU32(hash, target);
    }
    HashU32(hash, shadow.depth_stencil);
    for (uint32_t texture : shadow.textures) {
        HashU32(hash, texture);
    }
    for (const auto& sampler : shadow.samplers) {
        HashU32(hash, sampler.mag_filter);
        HashU32(hash, sampler.min_filter);
        HashU32(hash, sampler.mip_filter);
        HashU32(hash, sampler.mip_level);
        HashU32(hash, sampler.border_color);
    }
    HashU64(hash, shadow.vertex_fetch_layout_signature);
    HashU64(hash, shadow.texture_fetch_layout_signature);
    return hash;
}

void HashDrawRecord(uint64_t& hash, const ac6::d3d::DrawCallRecord& draw) {
    HashU32(hash, static_cast<uint32_t>(draw.kind));
    HashU32(hash, draw.primitive_type);
    HashU32(hash, draw.start);
    HashU32(hash, draw.count);
    HashU32(hash, draw.flags);
    HashU32(hash, draw.shadow_state.render_targets[0]);
    HashU32(hash, draw.shadow_state.depth_stencil);
    HashU32(hash, draw.shadow_state.viewport.width);
    HashU32(hash, draw.shadow_state.viewport.height);
}

// D3DPRIMITIVETYPE (D3D9); used by DrawIndexed* and DrawPrimitive.
void IncrementTopologyHistogram(ac6::d3d::FrameCaptureSummary& summary, uint32_t primitive_type) {
    switch (primitive_type) {
        case 1:
            ++summary.topology_pointlist;
            break;
        case 2:
            ++summary.topology_linelist;
            break;
        case 3:
            ++summary.topology_linestrip;
            break;
        case 4:
            ++summary.topology_trianglelist;
            break;
        case 5:
            ++summary.topology_trianglestrip;
            break;
        case 6:
            ++summary.topology_trianglefan;
            break;
        default:
            ++summary.topology_other;
            break;
    }
}

ac6::d3d::FrameCaptureSummary MakeFrameCaptureSummary(
    const ac6::d3d::FrameCaptureSnapshot& frame_capture) {
    ac6::d3d::FrameCaptureSummary summary;
    summary.capture_enabled = REXCVAR_GET(ac6_render_capture);
    summary.frame_index = frame_capture.frame_index;
    summary.frame_stats = frame_capture.stats;
    summary.draw_count = static_cast<uint32_t>(frame_capture.draws.size());
    summary.clear_count = static_cast<uint32_t>(frame_capture.clears.size());
    summary.resolve_count = static_cast<uint32_t>(frame_capture.resolves.size());
    summary.frame_end_render_target_count =
        CountNonZero(frame_capture.frame_end_shadow.render_targets);
    summary.frame_end_texture_count = CountNonZero(frame_capture.frame_end_shadow.textures);
    summary.frame_end_stream_count = CountNonZeroStreams(frame_capture.frame_end_shadow.streams);
    summary.frame_end_sampler_count = CountNonZeroSamplers(frame_capture.frame_end_shadow.samplers);
    summary.frame_end_texture_fetch_count =
        CountNonZero(frame_capture.frame_end_shadow.texture_fetch_ptrs);
    summary.frame_end_render_target_0 = frame_capture.frame_end_shadow.render_targets[0];
    summary.frame_end_depth_stencil = frame_capture.frame_end_shadow.depth_stencil;
    summary.frame_end_viewport_width = frame_capture.frame_end_shadow.viewport.width;
    summary.frame_end_viewport_height = frame_capture.frame_end_shadow.viewport.height;
    if (!frame_capture.draws.empty()) {
        summary.first_draw_render_target_0 =
            frame_capture.draws.front().shadow_state.render_targets[0];
        summary.last_draw_render_target_0 =
            frame_capture.draws.back().shadow_state.render_targets[0];
        const auto& last_draw = frame_capture.draws.back();
        summary.last_draw_primitive_type = last_draw.primitive_type;
        summary.last_draw_count = last_draw.count;
        summary.last_draw_flags = last_draw.flags;
    }
    if (summary.capture_enabled && !frame_capture.draws.empty()) {
        constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
        uint64_t signature = kFnvOffsetBasis;
        std::unordered_set<uint32_t> unique_rt0s;
        unique_rt0s.reserve(std::min<size_t>(frame_capture.draws.size(), 128));
        uint32_t previous_rt0 = frame_capture.draws.front().shadow_state.render_targets[0];
        for (const auto& draw : frame_capture.draws) {
            switch (draw.kind) {
                case ac6::d3d::DrawCallKind::kIndexed:
                    ++summary.indexed_draw_count;
                    break;
                case ac6::d3d::DrawCallKind::kVerticesUP:
                    ++summary.indexed_shared_draw_count;
                    break;
                case ac6::d3d::DrawCallKind::kPrimitive:
                    ++summary.primitive_draw_count;
                    break;
            }
            IncrementTopologyHistogram(summary, draw.primitive_type);
            uint32_t rt0 = draw.shadow_state.render_targets[0];
            unique_rt0s.insert(rt0);
            if (rt0 != previous_rt0) {
                ++summary.rt0_switch_count;
                previous_rt0 = rt0;
            }
            HashDrawRecord(signature, draw);
        }
        summary.unique_rt0_count = static_cast<uint32_t>(unique_rt0s.size());
        summary.record_signature = signature;
        summary.record_signature_valid = true;
    }
    return summary;
}

void RememberDevice(uint32_t device) {
    std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
    g_shadow.device = device;
}

ac6::d3d::ShadowState SnapshotShadowState(uint32_t device) {
    std::shared_lock<std::shared_mutex> lock(g_shadow_mutex);
    ac6::d3d::ShadowState shadow = g_shadow;
    if (device != 0) {
        shadow.device = device;
    }
    shadow.vertex_fetch_layout_signature = ComputeVertexFetchLayoutSignature(shadow);
    shadow.texture_fetch_layout_signature = ComputeTextureFetchLayoutSignature(shadow);
    shadow.resource_binding_signature = ComputeResourceBindingSignature(shadow);
    return shadow;
}

ac6::d3d::DrawStatsSnapshot SnapshotDrawStats() {
    return ac6::d3d::DrawStatsSnapshot{
        g_live_stats.draw_calls.load(std::memory_order_relaxed),
        g_live_stats.draw_calls_indexed.load(std::memory_order_relaxed),
        g_live_stats.draw_calls_up.load(std::memory_order_relaxed),
        g_live_stats.draw_calls_primitive.load(std::memory_order_relaxed),
        g_live_stats.total_indices.load(std::memory_order_relaxed),
        g_live_stats.total_vertices.load(std::memory_order_relaxed),
        g_live_stats.set_texture_calls.load(std::memory_order_relaxed),
        g_live_stats.set_render_target_calls.load(std::memory_order_relaxed),
        g_live_stats.set_depth_stencil_calls.load(std::memory_order_relaxed),
        g_live_stats.set_vertex_decl_calls.load(std::memory_order_relaxed),
        g_live_stats.set_index_buffer_calls.load(std::memory_order_relaxed),
        g_live_stats.set_stream_source_calls.load(std::memory_order_relaxed),
        g_live_stats.set_viewport_calls.load(std::memory_order_relaxed),
        g_live_stats.set_sampler_state_calls.load(std::memory_order_relaxed),
        g_live_stats.set_texture_fetch_calls.load(std::memory_order_relaxed),
        g_live_stats.clear_calls.load(std::memory_order_relaxed),
        g_live_stats.resolve_calls.load(std::memory_order_relaxed),
    };
}

void CaptureDrawCall(ac6::d3d::DrawCallKind kind, uint32_t device, uint32_t primitive_type,
                     uint32_t start, uint32_t count, uint32_t flags) {
    if (!REXCVAR_GET(ac6_render_capture)) {
        return;
    }

    ac6::d3d::DrawCallRecord record;
    record.kind = kind;
    record.primitive_type = primitive_type;
    record.start = start;
    record.count = count;
    record.flags = flags;
    record.shadow_state = SnapshotShadowState(device);

    std::unique_lock<std::shared_mutex> lock(g_capture_mutex);
    record.sequence = ++g_capture_live_sequence;
    g_live_draws.push_back(std::move(record));
}

void CaptureClear(uint8_t* base, uint32_t device, uint32_t rect_count, uint32_t rects_ptr,
                  uint32_t flags, uint32_t color, uint32_t stencil, float depth) {
    if (!REXCVAR_GET(ac6_render_capture)) {
        return;
    }

    ac6::d3d::ClearRecord record;
    record.rect_count = rect_count;
    record.flags = flags;
    record.color = color;
    record.stencil = stencil;
    record.depth = depth;
    record.shadow_state = SnapshotShadowState(device);
    if (rects_ptr && rect_count) {
        const uint32_t captured_rect_count =
            std::min<uint32_t>(rect_count, ac6::d3d::kMaxClearRectsPerRecord);
        record.captured_rect_count = captured_rect_count;
        for (uint32_t i = 0; i < captured_rect_count; ++i) {
            const uint32_t rect_ptr = rects_ptr + i * 16;
            record.rects[i].left = PPC_LOAD_U32(rect_ptr + 0);
            record.rects[i].top = PPC_LOAD_U32(rect_ptr + 4);
            record.rects[i].right = PPC_LOAD_U32(rect_ptr + 8);
            record.rects[i].bottom = PPC_LOAD_U32(rect_ptr + 12);
        }
    }

    std::unique_lock<std::shared_mutex> lock(g_capture_mutex);
    record.sequence = ++g_capture_live_sequence;
    g_live_clears.push_back(std::move(record));
}

void CaptureResolve(uint32_t device, const PPCContext& ctx) {
    if (!REXCVAR_GET(ac6_render_capture)) {
        return;
    }

    ac6::d3d::ResolveRecord record;
    record.guest_lr = static_cast<uint32_t>(ctx.lr);
    record.args = {
        ctx.r4.u32,
        ctx.r5.u32,
        ctx.r6.u32,
        ctx.r7.u32,
        ctx.r8.u32,
        ctx.r9.u32,
        ctx.r10.u32,
    };
    record.depth_or_scale = static_cast<float>(ctx.f1.f64);
    record.shadow_state = SnapshotShadowState(device);

    std::unique_lock<std::shared_mutex> lock(g_capture_mutex);
    record.sequence = ++g_capture_live_sequence;
    g_live_resolves.push_back(std::move(record));
}

}  // namespace

// ---------------------------------------------------------------------------
// Guest D3D9 entry points.
//
// Every identity below was verified against the recompiled body in
// generated/ac6recomp_recomp.11.cpp on 2026-09-14; twelve of the original
// nineteen labels were wrong (the earlier "SetRenderTarget" and
// "SetDepthStencil" hooks were debug-monitor functions whose r4/r5 are never
// read, so the shadow state they fed was garbage). r3 is the device in every
// D3D function. "PURE-SETTER" means the body only writes the device struct; the
// draws, Clear, Resolve and the window-scissor emitter write PM4 directly.
//
// The game also writes shader constants and the vertex declaration INLINE,
// bypassing the Set* entry points, so this shadow can never be complete - the
// draw path must read the device struct itself at draw time
// (ac6_register_shadow.cpp does exactly that).
// ---------------------------------------------------------------------------

PPC_EXTERN_FUNC(__imp__rex_sub_821DEF18);  // DrawVertices (non-indexed)
PPC_EXTERN_FUNC(__imp__rex_sub_821DF300);  // DrawIndexedVertices
PPC_EXTERN_FUNC(__imp__rex_sub_821DEA48);  // BeginVertices (DrawVerticesUP path)
PPC_EXTERN_FUNC(__imp__rex_sub_821DD0A8);  // SetStreamSource
PPC_EXTERN_FUNC(__imp__rex_sub_821DD260);  // SetRenderTarget
PPC_EXTERN_FUNC(__imp__rex_sub_821DD5C8);  // SetDepthStencilSurface
PPC_EXTERN_FUNC(__imp__rex_sub_821DE600);  // SetVertexShader
PPC_EXTERN_FUNC(__imp__rex_sub_821DE308);  // SetPixelShader
PPC_EXTERN_FUNC(__imp__rex_sub_821DE7D0);  // SetVertexDeclaration
PPC_EXTERN_FUNC(__imp__rex_sub_821DD1C8);  // SetIndexBuffer
PPC_EXTERN_FUNC(__imp__rex_sub_821DA698);  // window-scissor emitter (internal)
PPC_EXTERN_FUNC(__imp__rex_sub_821DC538);  // SetSamplerState_MinFilter
PPC_EXTERN_FUNC(__imp__rex_sub_821DC6C8);  // SetSamplerState_MagFilter
PPC_EXTERN_FUNC(__imp__rex_sub_821DC9C0);  // SetSamplerState_AnisotropyBias
PPC_EXTERN_FUNC(__imp__rex_sub_821DCA68);  // SetSamplerState_MipMapLodBias
PPC_EXTERN_FUNC(__imp__rex_sub_821DCB08);  // SetSamplerState_MaxMipLevel
PPC_EXTERN_FUNC(__imp__rex_sub_821DCB88);  // SetSamplerState_MinMipLevel
PPC_EXTERN_FUNC(__imp__rex_sub_821DBAF8);  // SetRenderState_Wrap0
PPC_EXTERN_FUNC(__imp__rex_sub_821E2380);  // Clear
PPC_EXTERN_FUNC(__imp__rex_sub_821E10C8);  // SetTexture
PPC_EXTERN_FUNC(__imp__rex_sub_821E2BB8);  // Resolve

PPC_EXTERN_FUNC(__imp__rex_sub_821E61A8);  // fence wait (dev, fence, flags)
PPC_EXTERN_FUNC(__imp__rex_sub_821F03B0);  // D3DDevice_Swap

// D3D fence wait (0x821E61A8): the guest spins here until the GPU - Xenia's
// command processor - has executed up to a fence. Its per-frame time on the
// main thread is the guest's "waiting for the emulated GPU" cost, which is what
// separates real sim/record work from spin in the thread's busy%.
PPC_FUNC_IMPL(rex_sub_821E61A8) {
    PPC_FUNC_PROLOGUE();
    SCOPE_profile_cpu_f("guest");
    __imp__rex_sub_821E61A8(ctx, base);
}

// D3DDevice_Swap (0x821F03B0): contains the VdSwap and, on the 360 D3D, the
// fence wait that throttles the CPU to one frame ahead of the GPU.
PPC_FUNC_IMPL(rex_sub_821F03B0) {
    PPC_FUNC_PROLOGUE();
    SCOPE_profile_cpu_f("guest");
    __imp__rex_sub_821F03B0(ctx, base);
}

// D3DDevice_DrawVertices (0x821DEF18) - EMITS PM4
// r3=pDevice, r4=PrimitiveType, r5=StartVertex, r6=VertexCount
PPC_FUNC_IMPL(rex_sub_821DEF18) {
    PPC_FUNC_PROLOGUE();

    const uint32_t prim_type = ctx.r4.u32;
    const uint32_t start_vertex = ctx.r5.u32;
    const uint32_t vertex_count = ctx.r6.u32;
    RememberDevice(ctx.r3.u32);

    COUNT_profile_add("gpu/guest_d3d_draws", 1);
    g_live_stats.draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.draw_calls_primitive.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.total_vertices.fetch_add(vertex_count, std::memory_order_relaxed);
    CaptureDrawCall(ac6::d3d::DrawCallKind::kPrimitive, ctx.r3.u32, prim_type, start_vertex,
                    vertex_count, 0);
    // r3 is also the return register - capture the device before the call.
    const uint32_t shadow_device = ctx.r3.u32;
    const uint32_t shadow_wp = ac6::shadow::BeginVerify(base, shadow_device);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "DrawVertices: prim={} start={} count={}",
            prim_type, start_vertex, vertex_count);
    }

    __imp__rex_sub_821DEF18(ctx, base);
    ac6::shadow::EndVerify(base, shadow_device, shadow_wp, start_vertex, "DrawVertices");
}

// D3DDevice_DrawIndexedVertices (0x821DF300) - EMITS PM4
// r3=pDevice, r4=PrimitiveType, r5=BaseVertexIndex, r6=StartIndex, r7=IndexCount
PPC_FUNC_IMPL(rex_sub_821DF300) {
    PPC_FUNC_PROLOGUE();

    const uint32_t prim_type = ctx.r4.u32;
    const uint32_t base_vertex = ctx.r5.u32;
    const uint32_t start_index = ctx.r6.u32;
    const uint32_t index_count = ctx.r7.u32;
    RememberDevice(ctx.r3.u32);

    COUNT_profile_add("gpu/guest_d3d_draws", 1);
    g_live_stats.draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.draw_calls_indexed.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.total_indices.fetch_add(index_count, std::memory_order_relaxed);
    // The record's flags slot carries the base vertex index.
    CaptureDrawCall(ac6::d3d::DrawCallKind::kIndexed, ctx.r3.u32, prim_type, start_index,
                    index_count, base_vertex);
    // r3 is also the return register - capture the device before the call.
    const uint32_t shadow_device = ctx.r3.u32;
    const uint32_t shadow_wp = ac6::shadow::BeginVerify(base, shadow_device);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "DrawIndexedVertices: prim={} base={} start={} count={}",
            prim_type, base_vertex, start_index, index_count);
    }

    __imp__rex_sub_821DF300(ctx, base);
    ac6::shadow::EndVerify(base, shadow_device, shadow_wp, base_vertex, "DrawIndexedVertices");
}

// D3DDevice_BeginVertices (0x821DEA48) - EMITS PM4
// Compiler-specialised: r4/r6 are ignored, the primitive is hard-coded to
// QUADLIST and the stride to 20 bytes; r5=VertexCount. Returns the allocated
// vertex pointer in r3 and its only caller (0x821DEED0, DrawVerticesUP) fills it
// in and commits. This is the "draw from CPU memory" path.
PPC_FUNC_IMPL(rex_sub_821DEA48) {
    PPC_FUNC_PROLOGUE();

    const uint32_t vertex_count = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    COUNT_profile_add("gpu/guest_d3d_draws", 1);
    g_live_stats.draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.draw_calls_up.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.total_vertices.fetch_add(vertex_count, std::memory_order_relaxed);
    CaptureDrawCall(ac6::d3d::DrawCallKind::kVerticesUP, ctx.r3.u32, /*QUADLIST*/ 13, 0,
                    vertex_count, 0);
    // r3 is also the return register - capture the device before the call.
    const uint32_t shadow_device = ctx.r3.u32;
    const uint32_t shadow_wp = ac6::shadow::BeginVerify(base, shadow_device);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "BeginVertices(UP): count={}", vertex_count);
    }

    __imp__rex_sub_821DEA48(ctx, base);
    ac6::shadow::EndVerify(base, shadow_device, shadow_wp, 0, "BeginVertices");
}

// D3DDevice_SetStreamSource (0x821DD0A8) - PURE-SETTER
// r3=pDevice, r4=StreamNumber, r5=pVertexBuffer, r6=OffsetInBytes, r7=Stride,
// r8=precomputed 64-bit fetch dirty bit. Writes vertex fetch constant 95-stream
// at device+1912-8*stream and the buffer pointer at device+12452+4*stream.
PPC_FUNC_IMPL(rex_sub_821DD0A8) {
    PPC_FUNC_PROLOGUE();

    const uint32_t stream = ctx.r4.u32;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t offset = ctx.r6.u32;
    const uint32_t stride = ctx.r7.u32;
    RememberDevice(ctx.r3.u32);

    if (stream < ac6::d3d::kMaxStreams) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.streams[stream].buffer = buffer;
        g_shadow.streams[stream].offset = offset;
        g_shadow.streams[stream].stride = stride;
    }
    g_live_stats.set_stream_source_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "SetStreamSource: stream={} buffer=0x{:08X} offset={} stride={}",
            stream, buffer, offset, stride);
    }

    __imp__rex_sub_821DD0A8(ctx, base);
}

// D3DDevice_SetRenderTarget (0x821DD260) - PURE-SETTER
// r3=pDevice, r4=Index, r5=pSurface. Stores the surface at device+12432+4*index
// and its ColorInfo (surface+28) into the RB_COLOR_INFO shadow; index 0 also
// refreshes RB_SURFACE_INFO / PA_SC_AA_CONFIG via 0x821DA600.
PPC_FUNC_IMPL(rex_sub_821DD260) {
    PPC_FUNC_PROLOGUE();

    const uint32_t index = ctx.r4.u32;
    const uint32_t surface = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (index < ac6::d3d::kMaxRenderTargets) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.render_targets[index] = surface;
    }
    // The full-res effects fix latches RT0 here to tell the R32F downscaler
    // apart from the effects module at SetViewport time (the device slot can
    // be stale then, because the game binds after setting the viewport).
    ac6::backend::NoteRt0Bound(index, surface);
    g_live_stats.set_render_target_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "SetRenderTarget: index={} surface=0x{:08X}",
            index, surface);
    }

    __imp__rex_sub_821DD260(ctx, base);
}

// D3DDevice_SetDepthStencilSurface (0x821DD5C8) - PURE-SETTER
// r3=pDevice, r4=pSurface -> device+12448, RB_DEPTH_INFO and RB_HIZCONTROL shadows.
PPC_FUNC_IMPL(rex_sub_821DD5C8) {
    PPC_FUNC_PROLOGUE();

    const uint32_t surface = ctx.r4.u32;
    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.depth_stencil = surface;
    }
    g_live_stats.set_depth_stencil_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetDepthStencilSurface: surface=0x{:08X}", surface);
    }

    __imp__rex_sub_821DD5C8(ctx, base);
}

// D3DDevice_SetVertexShader (0x821DE600) - PURE-SETTER
// r3=pDevice, r4=pShader -> device+12688; also copies the shader's embedded
// fetch-constant defaults into the fetch shadow at device+1152.
PPC_FUNC_IMPL(rex_sub_821DE600) {
    PPC_FUNC_PROLOGUE();

    const uint32_t shader = ctx.r4.u32;
    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.vertex_shader = shader;
    }

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetVertexShader: shader=0x{:08X}", shader);
    }

    __imp__rex_sub_821DE600(ctx, base);
}

// D3DDevice_SetPixelShader (0x821DE308) - PURE-SETTER
// r3=pDevice, r4=pShader -> device+12684.
PPC_FUNC_IMPL(rex_sub_821DE308) {
    PPC_FUNC_PROLOGUE();

    const uint32_t shader = ctx.r4.u32;
    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.pixel_shader = shader;
    }

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetPixelShader: shader=0x{:08X}", shader);
    }

    __imp__rex_sub_821DE308(ctx, base);
}

// D3DDevice_SetVertexDeclaration (0x821DE7D0) - PURE-SETTER
// r3=pDevice, r4=pDecl -> device+11812. NOTE the game also writes 11812 inline,
// so this hook sees only some of the changes.
PPC_FUNC_IMPL(rex_sub_821DE7D0) {
    PPC_FUNC_PROLOGUE();

    const uint32_t decl = ctx.r4.u32;
    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.vertex_declaration = decl;
    }
    g_live_stats.set_vertex_decl_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetVertexDeclaration: decl=0x{:08X}", decl);
    }

    __imp__rex_sub_821DE7D0(ctx, base);
}

// D3DDevice_SetIndexBuffer (0x821DD1C8) - PURE-SETTER
// r3=pDevice, r4=pIndexBuffer -> device+12428.
PPC_FUNC_IMPL(rex_sub_821DD1C8) {
    PPC_FUNC_PROLOGUE();

    const uint32_t buffer = ctx.r4.u32;
    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.index_buffer = buffer;
    }
    g_live_stats.set_index_buffer_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetIndexBuffer: buffer=0x{:08X}", buffer);
    }

    __imp__rex_sub_821DD1C8(ctx, base);
}

// Window-scissor emitter (0x821DA698) - EMITS PM4. Internal, not an API entry.
// r3=pDevice, r4=left, r5=top, r6=right, r7=bottom. Non-tiled it writes TYPE0
// 0x00022080 (PA_SC_WINDOW_OFFSET / WINDOW_SCISSOR_TL / _BR) straight into the
// command buffer; tiled it emits a SET_BIN_MASK sequence per tile. Reached from
// SetScissorRect (0x821DCF28), SetViewport's tail, Clear and Resolve. The real
// SetViewport is 0x821DD028 -> 0x821DA978 and is hooked by ac6_fullres_effects.
PPC_FUNC_IMPL(rex_sub_821DA698) {
    PPC_FUNC_PROLOGUE();

    RememberDevice(ctx.r3.u32);
    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        // Stored as an extent so the signature code's width/height keep meaning.
        g_shadow.viewport.x = ctx.r4.u32;
        g_shadow.viewport.y = ctx.r5.u32;
        g_shadow.viewport.width = ctx.r6.u32 - ctx.r4.u32;
        g_shadow.viewport.height = ctx.r7.u32 - ctx.r5.u32;
    }
    g_live_stats.set_viewport_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "WindowScissor: ({},{})-({},{})",
            ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    }

    __imp__rex_sub_821DA698(ctx, base);
}

// D3DDevice_Resolve (0x821E2BB8) - EMITS PM4
// r3=pDevice, r4=Flags (low 3 bits = RT index, bit 2 = depth), r5=pSourceRect,
// r6=pDestTexture, r7=pDestPoint, r8=DestLevel, r9=DestSlice, r10=pClearColor,
// f1=ClearZ. Tile-aware.
PPC_FUNC_IMPL(rex_sub_821E2BB8) {
    PPC_FUNC_PROLOGUE();

    RememberDevice(ctx.r3.u32);
    g_live_stats.resolve_calls.fetch_add(1, std::memory_order_relaxed);
    // Record the dest as the silhouette mask base if the downscaler just ran,
    // then lift the effects module's 640x360 source rect to 1280x720.
    ac6::backend::FxNoteResolveDest(ctx.r6.u32, base);
    ac6::backend::FullresFixResolveRect(ctx, base);
    CaptureResolve(ctx.r3.u32, ctx);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "Resolve: flags=0x{:X} dest=0x{:08X}", ctx.r4.u32, ctx.r6.u32);
    }

    __imp__rex_sub_821E2BB8(ctx, base);
}

// Sampler state setters - all PURE-SETTER, all r3=pDevice, r4=Sampler, r5=Value.
// Each edits bits of the 6-dword texture fetch constant at device+1152+24*sampler.
// The field names in SamplerBinding are historical; the comments say what each
// actually holds.

// SetSamplerState_MinFilter (0x821DC538): fetch dword3 bits 21-22 (+aniso bits).
PPC_FUNC_IMPL(rex_sub_821DC538) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].min_filter = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_MinFilter: sampler={} value={}", sampler,
                         value);
    }

    __imp__rex_sub_821DC538(ctx, base);
}

// SetSamplerState_MagFilter (0x821DC6C8): fetch dword3 bits 19-20 (+aniso bits).
PPC_FUNC_IMPL(rex_sub_821DC6C8) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].mag_filter = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_MagFilter: sampler={} value={}", sampler,
                         value);
    }

    __imp__rex_sub_821DC6C8(ctx, base);
}

// SetSamplerState_MinMipLevel (0x821DCB88): Xbox D3DSAMP_MINMIPLEVEL = hardware
// mip_max_level, fetch dword4 bits 6-9, clamped against the bound texture.
// Historically stored in the "min_filter"-adjacent slot as sampler state A.
PPC_FUNC_IMPL(rex_sub_821DCB88) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].min_mip_level = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_MinMipLevel: sampler={} value={}", sampler,
                         value);
    }

    __imp__rex_sub_821DCB88(ctx, base);
}

// SetSamplerState_MipMapLodBias (0x821DCA68): float-as-uint -> fixed point into
// fetch dword4 bits 12-21 (lod_bias). Stored in SamplerBinding::mip_filter,
// which is therefore NOT a D3DTEXTUREFILTERTYPE.
PPC_FUNC_IMPL(rex_sub_821DCA68) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].mip_filter = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_MipMapLodBias: sampler={} raw=0x{:08X}",
                         sampler, value);
    }

    __imp__rex_sub_821DCA68(ctx, base);
}

// SetSamplerState_AnisotropyBias (0x821DC9C0): float-as-uint -> fixed point into
// fetch dword5 bits 5-8 (aniso_bias; border_color is bits 0-1 and untouched).
// Stored in SamplerBinding::border_color for historical reasons.
PPC_FUNC_IMPL(rex_sub_821DC9C0) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].border_color = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_AnisotropyBias: sampler={} raw=0x{:08X}",
                         sampler, value);
    }

    __imp__rex_sub_821DC9C0(ctx, base);
}

// SetSamplerState_MaxMipLevel (0x821DCB08): D3DSAMP_MAXMIPLEVEL = hardware
// mip_min_level, fetch dword4 bits 2-5, clamped against the bound texture.
PPC_FUNC_IMPL(rex_sub_821DCB08) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    if (sampler < ac6::d3d::kMaxSamplers) {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        g_shadow.samplers[sampler].mip_level = value;
    }
    g_live_stats.set_sampler_state_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetSamplerState_MaxMipLevel: sampler={} value={}", sampler,
                         value);
    }

    __imp__rex_sub_821DCB08(ctx, base);
}

// SetRenderState_Wrap0 (0x821DBAF8) - PURE-SETTER
// r3=pDevice, r4=Value -> low nibble of device+10540 (SQ_WRAPPING_0). Was
// labelled SetShaderGPRAlloc; the real GPR allocation is 0x821DE960. Kept
// hooked only for the trace.
PPC_FUNC_IMPL(rex_sub_821DBAF8) {
    PPC_FUNC_PROLOGUE();

    RememberDevice(ctx.r3.u32);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetRenderState_Wrap0: value=0x{:08X}", ctx.r4.u32);
    }

    __imp__rex_sub_821DBAF8(ctx, base);
}

// D3DDevice_Clear (0x821E2380) - EMITS PM4
// r3=pDevice, r4=Count, r5=pRects, r6=Flags, r7=Color, f1=Z, r8=Stencil, r9=passthrough
PPC_FUNC_IMPL(rex_sub_821E2380) {
    PPC_FUNC_PROLOGUE();

    RememberDevice(ctx.r3.u32);
    g_live_stats.clear_calls.fetch_add(1, std::memory_order_relaxed);
    // How the game clears: full-target vs rect-limited, and which aspects.
    // Decides whether "a clear elides the EDRAM ownership transfer" is safe.
    if (ctx.r4.u32) {
        COUNT_profile_add("gpu/guest_clears_rect", 1);
    } else {
        COUNT_profile_add("gpu/guest_clears_full", 1);
    }
    if (ctx.r6.u32 & 0x1) COUNT_profile_add("gpu/guest_clears_target", 1);
    if (ctx.r6.u32 & 0x2) COUNT_profile_add("gpu/guest_clears_zbuffer", 1);
    if (ctx.r6.u32 & 0x4) COUNT_profile_add("gpu/guest_clears_stencil", 1);
    CaptureClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                 static_cast<float>(ctx.f1.f64));

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU,
            "Clear: count={} flags=0x{:X} color=0x{:08X} stencil={}",
            ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    }

    __imp__rex_sub_821E2380(ctx, base);
}

// D3DDevice_SetTexture (0x821E10C8) - PURE-SETTER
// r3=pDevice, r4=Sampler, r5=pTexture, r6=precomputed 64-bit fetch dirty bit.
// Merges the texture's fetch words (texture+28..+48) with the sampler-owned bits
// already in device+1152+24*sampler and stores the object at device+12536+4*sampler.
PPC_FUNC_IMPL(rex_sub_821E10C8) {
    PPC_FUNC_PROLOGUE();

    const uint32_t sampler = ctx.r4.u32;
    const uint32_t texture = ctx.r5.u32;
    RememberDevice(ctx.r3.u32);

    {
        std::unique_lock<std::shared_mutex> lock(g_shadow_mutex);
        if (sampler < ac6::d3d::kMaxTextures) {
            g_shadow.textures[sampler] = texture;
        }
        // Kept in step: the signature code counts bound textures from here.
        if (sampler < ac6::d3d::kMaxFetchConstants) {
            g_shadow.texture_fetch_ptrs[sampler] = texture;
        }
    }
    g_live_stats.set_texture_calls.fetch_add(1, std::memory_order_relaxed);
    g_live_stats.set_texture_fetch_calls.fetch_add(1, std::memory_order_relaxed);

    if (REXCVAR_GET(ac6_d3d_trace)) {
        REXLOG_CAT_TRACE(kLogGPU, "SetTexture: sampler={} texture=0x{:08X}", sampler, texture);
    }

    __imp__rex_sub_821E10C8(ctx, base);
}

namespace ac6::d3d {

void OnFrameBoundary() {
    ac6::d3d::DrawStatsSnapshot draw_stats = SnapshotDrawStats();

    // Stage 1 register-shadow verification summary, once a second.
    if (REXCVAR_GET(ac6_register_shadow_verify)) {
        static auto last_summary = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_summary >= std::chrono::seconds(1)) {
            last_summary = now;
            ac6::shadow::GuestRegisterShadow::LogSummary();
        }
    }

    std::unique_lock<std::shared_mutex> lock(g_snapshot_mutex);
    g_snapshot = draw_stats;
    lock.unlock();

    if (!REXCVAR_GET(ac6_render_capture)) {
        std::unique_lock<std::shared_mutex> capture_lock(g_capture_mutex);
        g_capture_summary = {};
        g_capture_summary.capture_enabled = false;
        g_capture_summary.frame_index = g_capture_live_frame_index++;
        g_capture_summary.frame_stats = draw_stats;
        g_capture_snapshot = {};
        g_capture_snapshot.frame_index = g_capture_summary.frame_index;
        g_capture_snapshot.stats = draw_stats;
        g_capture_live_sequence = 0;
        g_live_draws.clear();
        g_live_clears.clear();
        g_live_resolves.clear();
        g_live_stats.Reset();
        return;
    }

    ac6::d3d::FrameCaptureSnapshot frame_capture;
    frame_capture.stats = draw_stats;
    frame_capture.frame_end_shadow = SnapshotShadowState(0);

    std::unique_lock<std::shared_mutex> capture_lock(g_capture_mutex);
    frame_capture.frame_index = g_capture_live_frame_index++;
    frame_capture.draws.swap(g_live_draws);
    frame_capture.clears.swap(g_live_clears);
    frame_capture.resolves.swap(g_live_resolves);
    g_capture_live_sequence = 0;
    g_capture_summary = MakeFrameCaptureSummary(frame_capture);
    g_capture_snapshot = std::move(frame_capture);

    g_live_stats.Reset();
}

DrawStatsSnapshot GetDrawStats() {
    std::shared_lock<std::shared_mutex> lock(g_snapshot_mutex);
    return g_snapshot;
}

FrameCaptureSnapshot TakeFrameCapture(FrameCaptureSummary* summary_out) {
    std::unique_lock<std::shared_mutex> lock(g_capture_mutex);
    if (summary_out) {
        *summary_out = g_capture_summary;
    }
    FrameCaptureSnapshot snapshot = std::move(g_capture_snapshot);
    g_capture_snapshot = {};
    return snapshot;
}

ShadowState GetShadowState() {
    std::shared_lock<std::shared_mutex> lock(g_shadow_mutex);
    return g_shadow;
}

}  // namespace ac6::d3d
