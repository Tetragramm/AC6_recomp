#include "ac6_register_shadow.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/memory.h>

REXCVAR_DEFINE_BOOL(ac6_register_shadow_verify, false, "AC6/Renderer",
                    "Rebuild the Xenos register file from the guest D3D device struct at each "
                    "draw and diff it against the one Xenia built from PM4. Proves the struct "
                    "map before any of the native draw path is written.");
REXCVAR_DEFINE_INT32(ac6_register_shadow_log_limit, 40, "AC6/Renderer",
                     "How many distinct mismatching registers to name before going quiet.");

namespace ac6::shadow {

namespace {

// A run of the guest device struct that mirrors a contiguous run of Xenos
// registers. Offsets and bases come from the literals passed to the three
// register flushers at their call sites in rex_sub_821DEF18
// (generated/ac6recomp_recomp.11.cpp:19941-20200); the flusher itself decides
// the stride (821EC478 = 16 regs/mask bit, 821EC0D8 = 1, 821EC318 = 6).
//
// Counts are pinned by the gaps between consecutive blocks, and the arithmetic
// closes: 1152 + 32*24 = 1920, 1920 + 256*16 = 6016, 6016 + 4096 = 10112.
struct ShadowBlock {
  uint32_t device_offset;
  uint32_t register_base;
  uint32_t register_count;
  const char* name;
};

constexpr ShadowBlock kBlocks[] = {
    // 32 texture fetch constants of 6 dwords each (== 96 vertex fetch
    // constants of 2 dwords - the same store, addressed two ways).
    {1152, 0x4800, 32 * 6, "fetch constants"},
    // 256 float4 ALU constants each.
    {1920, 0x4000, 256 * 4, "vertex ALU constants"},
    {6016, 0x4400, 256 * 4, "pixel ALU constants"},
    // Bool + loop constants; 256 bytes to the next block.
    {10112, 0x4900, 64, "bool/loop constants"},
    // Render-state groups. Each count is the gap to the next block. The three
    // dwords between the 0x2000 block and 10444 are not padding: they are the
    // window offset / scissor shadow the scissor emitter (0x821DA698) flushes.
    {10368, 0x2000, 16, "reg 0x2000"},
    {10432, 0x2080, 3, "window scissor"},
    {10444, 0x2100, 21, "reg 0x2100"},
    {10528, 0x2180, 5, "reg 0x2180"},
    {10548, 0x2200, 12, "reg 0x2200"},
    {10596, 0x2280, 21, "reg 0x2280"},
    {10680, 0x2300, 38, "reg 0x2300"},
    {10832, 0x2380, 8, "reg 0x2380"},
    // Six user clip planes, flushed by 0x821EC198 (`li r29,9096; addi r31,r30,
    // 10272`), 16 bytes each - ending exactly where the 0x2000 block starts.
    {10272, 0x2388, 24, "clip planes"},
};

std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_draws_with_mismatch{0};
std::atomic<uint64_t> g_mismatch_total{0};
std::atomic<int32_t> g_logged{0};

const char* RegisterName(uint32_t index) {
  const auto* info = rex::graphics::RegisterFile::GetRegisterInfo(index);
  return (info && info->name) ? info->name : "?";
}

}  // namespace

void GuestRegisterShadow::Expand(const uint8_t* base, uint32_t device) {
  std::memset(mapped_, 0, sizeof(mapped_));
  if (!base || !device) {
    return;
  }
  for (const ShadowBlock& block : kBlocks) {
    const uint32_t last = block.register_base + block.register_count;
    if (last > kRegisterCount) {
      continue;  // a bad entry must not corrupt neighbouring registers
    }
    const uint8_t* src = base + device + block.device_offset;
    for (uint32_t i = 0; i < block.register_count; ++i) {
      // The guest wrote these big-endian; the host register file is native.
      values_[block.register_base + i] =
          rex::memory::load_and_swap<uint32_t>(src + i * sizeof(uint32_t));
    }
    for (uint32_t i = block.register_base; i < last; ++i) {
      mapped_[i >> 6] |= uint64_t(1) << (i & 63);
    }
  }
}

namespace {

// Device struct offsets of the command-buffer write pointer. It points AT the
// last written dword (the emitter is `stwu rX,4(rY)`), so a call's packets
// occupy (before, after].
constexpr uint32_t kDeviceWritePtr = 48;
// Current command-buffer segment base. If it changes across a call, the call's
// packets are not contiguous and cannot be walked.
constexpr uint32_t kDeviceSegmentBase = 14912;
thread_local uint32_t t_segment_before = 0;

// Coverage: which registers the guest has flushed at least once, so the pass
// criterion can say "N distinct registers verified" rather than just "no
// mismatches" - a map that never gets exercised proves nothing.
std::mutex g_seen_mutex;
uint64_t g_seen[(0x5003 + 63) / 64]{};
std::atomic<uint64_t> g_registers_checked{0};
std::atomic<uint64_t> g_unmapped_writes{0};
std::atomic<uint64_t> g_skipped_calls{0};

}  // namespace

uint32_t GuestRegisterShadow::VerifyEmitted(const uint8_t* base, uint32_t wp_before,
                                            uint32_t wp_after, const char* what) {
  if (!base || wp_after == wp_before) {
    return 0;
  }
  // The emitter can hand the call a fresh segment mid-way (0x821E60A8), in
  // which case the packets are not contiguous with wp_before. Only walk a
  // forward, sane-sized range; count anything else as skipped rather than
  // misreport it.
  if (wp_after < wp_before || wp_after - wp_before > (1u << 20)) {
    if (g_skipped_calls.fetch_add(1, std::memory_order_relaxed) < 12) {
      REXLOG_ERROR("[AC6-REGSHADOW] {} skipped: wp {:08X} -> {:08X}", what, wp_before, wp_after);
    }
    return 0;
  }
  uint32_t mismatches = 0;
  const int32_t limit = REXCVAR_GET(ac6_register_shadow_log_limit);
  uint32_t ptr = wp_before + 4;
  const uint32_t end = wp_after + 4;
  while (ptr < end) {
    const uint32_t header = rex::memory::load_and_swap<uint32_t>(base + ptr);
    const uint32_t type = header >> 30;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    ptr += 4;
    if (type == 0) {
      // TYPE0: `count` register values starting at base_index; bit 15 means
      // every value goes to the same register.
      const uint32_t reg_base = header & 0x7FFF;
      const bool one_reg = (header & 0x8000) != 0;
      for (uint32_t i = 0; i < count && ptr < end; ++i, ptr += 4) {
        const uint32_t reg = one_reg ? reg_base : reg_base + i;
        if (reg >= kRegisterCount) {
          continue;
        }
        const uint32_t emitted = rex::memory::load_and_swap<uint32_t>(base + ptr);
        const bool mapped = (mapped_[reg >> 6] >> (reg & 63)) & 1;
        if (!mapped) {
          // The guest flushes a register outside every block we know about:
          // that is a hole in the map, worth a line even though it is not a
          // value mismatch.
          if (g_unmapped_writes.fetch_add(1, std::memory_order_relaxed) < 16) {
            REXLOG_ERROR("[AC6-REGSHADOW] {} flushes UNMAPPED reg 0x{:04X} ({}) = {:08X}", what,
                         reg, RegisterName(reg), emitted);
          }
          continue;
        }
        g_registers_checked.fetch_add(1, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(g_seen_mutex);
          g_seen[reg >> 6] |= uint64_t(1) << (reg & 63);
        }
        if (values_[reg] == emitted) {
          continue;
        }
        ++mismatches;
        if (g_logged.load(std::memory_order_relaxed) < limit) {
          g_logged.fetch_add(1, std::memory_order_relaxed);
          REXLOG_ERROR("[AC6-REGSHADOW] {} reg 0x{:04X} ({}): struct {:08X} != emitted {:08X}",
                       what, reg, RegisterName(reg), values_[reg], emitted);
        }
      }
    } else if (type == 3) {
      ptr += count * 4;  // TYPE3: opcode packet, `count` payload dwords
    } else if (type == 2) {
      // One-dword NOP (0x80000000).
    } else {
      // TYPE1 never occurs. Seeing one means the walk has desynchronised -
      // the call's packets were split across two command-buffer segments
      // (0x821EBA88 does that for large block writes) and we have walked into
      // whatever lies between them. Everything compared so far in this call
      // is suspect, so discard the call rather than report garbage.
      g_skipped_calls.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
  }
  g_draws.fetch_add(1, std::memory_order_relaxed);
  if (mismatches) {
    g_draws_with_mismatch.fetch_add(1, std::memory_order_relaxed);
    g_mismatch_total.fetch_add(mismatches, std::memory_order_relaxed);
  }
  return mismatches;
}

void GuestRegisterShadow::LogSummary() {
  const uint64_t draws = g_draws.load(std::memory_order_relaxed);
  if (!draws) {
    return;
  }
  uint32_t distinct = 0;
  {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    for (uint64_t word : g_seen) {
      distinct += uint32_t(__builtin_popcountll(word));
    }
  }
  REXLOG_ERROR(
      "[AC6-REGSHADOW] {} calls verified, {} with mismatches ({} registers), {} register "
      "writes checked across {} distinct registers, {} unmapped writes, {} calls skipped",
      draws, g_draws_with_mismatch.load(std::memory_order_relaxed),
      g_mismatch_total.load(std::memory_order_relaxed),
      g_registers_checked.load(std::memory_order_relaxed), distinct,
      g_unmapped_writes.load(std::memory_order_relaxed),
      g_skipped_calls.load(std::memory_order_relaxed));
}

namespace {
// One per guest thread: the expansion is ~10 KB and must not be shared.
thread_local GuestRegisterShadow t_shadow;
}  // namespace

uint32_t BeginVerify(const uint8_t* base, uint32_t device) {
  if (!REXCVAR_GET(ac6_register_shadow_verify) || !base || !device) {
    return 0;
  }
  t_segment_before = rex::memory::load_and_swap<uint32_t>(base + device + kDeviceSegmentBase);
  return rex::memory::load_and_swap<uint32_t>(base + device + kDeviceWritePtr);
}

void EndVerify(const uint8_t* base, uint32_t device, uint32_t wp_before, uint32_t vgt_indx_offset,
               const char* what) {
  if (!REXCVAR_GET(ac6_register_shadow_verify) || !base || !device || !wp_before) {
    return;
  }
  // Expand AFTER the draw: its two derivation helpers (0x821ED210 for the SQ_*
  // shader-control registers, 0x821EBD40 for RB_HIZCONTROL) compute their
  // results at draw time and store them back into the struct, so the struct is
  // only complete once the draw has run. Expanding before it showed exactly
  // those registers as the only mismatches.
  const uint32_t wp_after = rex::memory::load_and_swap<uint32_t>(base + device + kDeviceWritePtr);
  if (rex::memory::load_and_swap<uint32_t>(base + device + kDeviceSegmentBase) != t_segment_before) {
    g_skipped_calls.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  t_shadow.Expand(base, device);
  t_shadow.SetDrawArgument(0x2102 /* VGT_INDX_OFFSET */, vgt_indx_offset);
  t_shadow.VerifyEmitted(base, wp_before, wp_after, what);
}

}  // namespace ac6::shadow
