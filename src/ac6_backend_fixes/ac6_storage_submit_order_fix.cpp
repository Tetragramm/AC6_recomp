// AC6 storage job submit-order race - preventive fix, ported from the retail
// title update.
//
// MECHANISM (found by an offline diff of the retail title update against the
// base executable this port is compiled from):
//
// The async save/storage layer - save.dat / replay.dat / dlcolorname.dat I/O
// and CResourceLoaderDpl's file reads, all reached through the save/load
// manager sub_82158D90 and its per-operation submitters - queues jobs on the
// game's second worker pool via JobPoolB::Submit (rex_sub_821D4A78). At twelve
// call sites the submitter stores the job's OPERATION CODE to [job+4] only
// AFTER Submit returns:
//
//     mr   r5, r31          ; job
//     bl   JobPoolB::Submit ; worker may run job->vtbl[1] from here on
//     li   r10, <op>
//     stw  r3, 0x61A4(r31)  ; ticket (submitter-side only, safe late)
//     stw  r10, 4(r31)      ; operation code - TOO LATE
//
// The pool-B worker (rex_sub_821D4F20) invokes job->vtbl[1](job, arg) as soon
// as it dequeues the job. The job objects are reused, so a worker that wins
// the race reads the PREVIOUS operation's code and performs the wrong storage
// operation. This is the same submit-side race family as the effect mode-word
// flicker (ac6_effect_mode_fix): on the 360 fixed core assignment and guest
// priorities made the window effectively unhittable in the common case; the
// port discards guest priorities and affinities, and its instant I/O keeps
// workers hot during the burst submits the save-select and DLC-content-check
// screens produce.
//
// The retail title update fixes exactly this: at all twelve sites it moves the
// [job+4] store BEFORE the Submit call (verified instruction-by-instruction on
// the patched image; the update leaves JobPoolB itself untouched). The port
// runs the base executable, so it inherits the unfixed ordering.
//
// THE FIX: a midasm hook at each of the twelve `bl` sites stores the operation
// code to [job+4] before the call - r5 already holds the job pointer (Submit's
// job argument, verified at every site), and the code is a per-site constant
// (2,3,6,7,8,9,0xA,0xB,0xC,0xD,0x10) except at 0x821C5A78 where it is carried
// in r29. The original post-call store still executes and re-stores the same
// value; that is harmless - the code is a submit-time input the worker reads,
// exactly why the title update wants it written before submission.
//
// No in-port failure has ever been attributed to this race; the fix is
// insurance against a rare, unreproducible wrong-operation on save data, taken
// because the vendor considered the reorder worth shipping. Semantics with the
// fix are identical to the unfixed path whenever the race does not fire.

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(
    ac6_fix_storage_submit_order, true, "AC6",
    "Store each async storage job's operation code before JobPoolB::Submit instead of "
    "after it, as the retail title update does at the same twelve call sites. Prevents "
    "a fast pool-B worker from reading a stale operation code on a reused job object "
    "(rare wrong save/replay/content-cache operation). On by default; semantics are "
    "identical to stock whenever the race does not occur.");

namespace {

struct SiteInfo {
  uint32_t guest_bl;  // guest address of the bl JobPoolB::Submit instruction
  uint32_t op_code;   // operation code stored to [job+4]; 0 = carried in r29
};

// Indices must match the kSite* constants below.
constexpr SiteInfo kSites[] = {
    {0x821D3C8C, 0x2}, {0x821D3D0C, 0x3}, {0x821D3DC8, 0x6}, {0x821D3E58, 0x7},
    {0x821D3F70, 0xA}, {0x821D4030, 0xB}, {0x821D40F0, 0xC}, {0x821C7538, 0xD},
    {0x821C8EC4, 0x8}, {0x821C8F78, 0x9}, {0x821C9298, 0x10}, {0x821C5A78, 0x0},
};
constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

std::atomic<uint32_t> g_engaged_total{0};
std::atomic<bool> g_site_logged[kSiteCount] = {};

void StoreOpCodeEarly(unsigned site, uint32_t job_va, uint32_t op_code) {
  if (!REXCVAR_GET(ac6_fix_storage_submit_order)) {
    return;
  }
  if (!job_va) {
    // Defensive: never seen (r5 is always a live object here), but a null job
    // would mean our model of the site is wrong - log once rather than fault.
    if (!g_site_logged[site].exchange(true, std::memory_order_relaxed)) {
      REXLOG_WARN("ac6_fix_storage_submit_order: null job at site 0x{:08X} - site skipped",
                  kSites[site].guest_bl);
    }
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(job_va + 4), op_code);
  const uint32_t total = g_engaged_total.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_site_logged[site].exchange(true, std::memory_order_relaxed)) {
    // Verified live 2026-08-06: six sites engaged within 100 ms of the boot
    // storage check, all on one reused job object cycling operation codes -
    // exactly the burst-submit shape the race analysis predicted.
    REXLOG_INFO(
        "ac6_fix_storage_submit_order ENGAGED: site 0x{:08X} job=0x{:08X} op=0x{:X} "
        "(engagements so far: {})",
        kSites[site].guest_bl, job_va, op_code, total);
  }
}

}  // namespace

// One hook per site; the constant is baked here so the hook needs only r5.
// Signatures must match the registers lists in ac6recomp_config.toml.
void ac6StorageOrderHookOp2(PPCRegister& r5) { StoreOpCodeEarly(0, r5.u32, 0x2); }
void ac6StorageOrderHookOp3(PPCRegister& r5) { StoreOpCodeEarly(1, r5.u32, 0x3); }
void ac6StorageOrderHookOp6(PPCRegister& r5) { StoreOpCodeEarly(2, r5.u32, 0x6); }
void ac6StorageOrderHookOp7(PPCRegister& r5) { StoreOpCodeEarly(3, r5.u32, 0x7); }
void ac6StorageOrderHookOpA(PPCRegister& r5) { StoreOpCodeEarly(4, r5.u32, 0xA); }
void ac6StorageOrderHookOpB(PPCRegister& r5) { StoreOpCodeEarly(5, r5.u32, 0xB); }
void ac6StorageOrderHookOpC(PPCRegister& r5) { StoreOpCodeEarly(6, r5.u32, 0xC); }
void ac6StorageOrderHookOpD(PPCRegister& r5) { StoreOpCodeEarly(7, r5.u32, 0xD); }
void ac6StorageOrderHookOp8(PPCRegister& r5) { StoreOpCodeEarly(8, r5.u32, 0x8); }
void ac6StorageOrderHookOp9(PPCRegister& r5) { StoreOpCodeEarly(9, r5.u32, 0x9); }
void ac6StorageOrderHookOp10(PPCRegister& r5) { StoreOpCodeEarly(10, r5.u32, 0x10); }

// 0x821C5A78: the operation code is computed by the caller and carried in r29
// (a nonvolatile, already live before the call - the stock post-call store is
// `stw r29,4(r31)`).
void ac6StorageOrderHookVar(PPCRegister& r5, PPCRegister& r29) {
  StoreOpCodeEarly(11, r5.u32, r29.u32);
}
