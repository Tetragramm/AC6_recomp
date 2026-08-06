// AC6 one-frame effects/clouds flicker - cure.
//
// SYMPTOM (reported in the wild since 2026-07-11, reproduces identically in
// stock Xenia): for a single frame the afterburner / light sprites render
// brighter and world-locked, and the clouds look doubled. Self-heals the next
// frame. Triggered by HOST-side CPU pressure only.
//
// MECHANISM (round 141, measured over six instrumented runs):
//
// The effect UPDATE and the effect DRAW are two asynchronous jobs on the
// game's second worker pool. Both enter through the SAME vtable slot -
// CX360EffectManager / CAce6EffectManager vtbl[+0x04] = rex_sub_822B3248 -
// and are told apart ONLY by a shared plain-store mode word at
// [mgr + 0xA1E84], with no per-job copy of it. Every dispatch arrives from
// lr = 0x821D4F80, inside the pool worker body rex_sub_821D4F20.
//
//     mode 0   -> f1 = dt ; rex_sub_822B2E20              UPDATE
//     mode 1   -> f1 = dt ; rex_sub_822B0848
//     mode 2   -> vtbl[+0x100] = rex_sub_820B25D0         DRAW
//     mode >=3 -> return
//
// The submitter writes the mode, then submits; the worker reads it later. On
// the 360 the two jobs never overlapped - fixed core assignment, deterministic
// scheduling - so the shared word was safe. rexglue discards both guest
// priorities and guest affinities, so that ordering guarantee is gone, and a
// host stall lets the second submit's write land before the first job has read
// its own value.
//
// When that happens BOTH jobs read mode 2: the update job draws instead, the
// update never runs, and the effect + cloud passes are emitted twice - ~+137
// draw requests into one draw-order bucket, +124 draws, every effect generator
// and observer doubling in lockstep and reverting the next frame. The two
// generator classes involved are CEffectGeneratorAfterBurner and
// CEffectGeneratorLightSprite, which are exactly the effects reported.
//
// THE FIX: snapshot the mode at submit time and dispatch from that snapshot,
// so a job runs with the parameter it was submitted with.
//
// Two things this deliberately does NOT do, both learned by measurement:
//
//  1. It does not write the mode word back and let the guest re-read it. That
//     was tried: the write landed, read back correct, and the guest still saw
//     the racing value ~6 instructions later. The concurrent writer owns that
//     window and no store can hold it. Measured 0.76% residual vs 0.00% here.
//
//  2. It does not take over "only when the snapshot disagrees with memory".
//     That sounds like a smaller blast radius and is actually the same bug in
//     miniature: on the agreeing path the guest still re-reads, so a write
//     landing after the check reintroduces exactly the hole in (1). Partial
//     takeover always leaves one direction open - pass through on mode 2 and a
//     racing write turns a draw into an update instead, which is the same
//     artifact mirrored. Full takeover is what makes it airtight.
//
// VALIDATION (identical binary, one cvar, host CPU stress throughout):
//     fix ON  : doubled-frame rate 0.00% (0 / 3748), 0.0 detections per 1k
//     fix OFF : doubled-frame rate 4.41%,           28.2 detections per 1k
// and cured rather than silenced - the effect manager still runs its normal
// two dispatches and exactly one draw per frame.

#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(
    ac6_effect_mode_fix, false, "AC6/Fixes",
    "Fix the one-frame effects/clouds flicker under heavy CPU load by "
    "snapshotting the effect job mode at submit. Off by default.");

// Dispatch targets, called directly. NOT via PPC_CALL_INDIRECT_FUNC: that macro
// degrades to __builtin_debugtrap() unless ppc_config.h was included first,
// which it is not in this translation unit - it would trap on the first
// corrected frame.
PPC_EXTERN_FUNC(rex_sub_820B25D0);  // CX360EffectManager vtbl[+0x100] DRAW  (mode 2)
PPC_EXTERN_FUNC(rex_sub_822B0AD8);  // CAce6EffectManager vtbl[+0x100] DRAW  (mode 2)
PPC_EXTERN_FUNC(rex_sub_822B2E20);  // UPDATE, f1 = dt     (mode 0)
PPC_EXTERN_FUNC(rex_sub_822B0848);  // f1 = dt             (mode 1)

PPC_EXTERN_FUNC(__imp__rex_sub_821D4A78);  // worker-pool Submit(tag, thr, job, arg)
PPC_EXTERN_FUNC(__imp__rex_sub_822B3248);  // effect manager vtbl[+0x04], the dispatcher

namespace {

constexpr uint32_t kEffModeOffset = 0xA1E84;
constexpr uint32_t kEffDispatcher = 0x822B3248;
// The two manager classes share the dispatcher at vtbl[+0x04] but have
// DIFFERENT draws at vtbl[+0x100]. Only these two reference the dispatcher, so
// the set is closed.
constexpr uint32_t kEffDrawX360 = 0x820B25D0;  // CX360EffectManager
constexpr uint32_t kEffDrawAce6 = 0x822B0AD8;  // CAce6EffectManager

// Submissions are matched to dispatches by the exact (job, arg) pair rather
// than positionally. The pool has PER-THREAD queues, so dispatch order is not
// submit order; a positional match hands each job the other one's mode, which
// swaps them. (That was a real bug in an earlier revision - its fingerprint was
// corrections arriving as "2 -> 0" immediately followed by "0 -> 2".) The
// worker calls job->vtbl[1](job, arg) with the same arg the submitter passed,
// so the pair identifies a submission uniquely without assuming anything about
// ordering, threads or queue depth.
struct EffModeEntry {
  uint32_t job = 0;
  uint32_t arg = 0;
  uint32_t mode = 0;
  bool used = true;
};

struct EffModeTable {
  std::mutex mu;
  EffModeEntry e[16];
  int next = 0;
  uint64_t corrections = 0;
  uint64_t matched = 0;
  uint64_t unmatched = 0;
  uint64_t dropped = 0;
  // (job, arg) is assumed to identify a submission uniquely. If two
  // outstanding submissions ever share a key, the newest-first lookup at
  // dispatch can hand a job the other one's mode - and that failure is SILENT,
  // because `unmatched` only counts "no entry found", never "found the wrong
  // entry". So measure the assumption instead of resting on it:
  //   key_collisions - same key outstanding twice, same mode  (harmless)
  //   key_conflicts  - same key outstanding twice, DIFFERENT modes (the
  //                    dangerous case; if this is ever non-zero the key needs
  //                    a third component, e.g. the target thread index)
  uint64_t key_collisions = 0;
  uint64_t key_conflicts = 0;
  // A mode-2 dispatch whose vtbl[+0x100] matches neither known draw. Falls
  // through to the original racy path, so it must never be silent.
  uint64_t unknown_draw = 0;
};

EffModeTable g_modefix;

// A pool job is the effect manager iff its vtable slot +4 is the dispatcher.
// Both CX360EffectManager and CAce6EffectManager share that slot.
bool IsEffectManagerJob(uint8_t* base, uint32_t job) {
  if (job < 0x82000000u || job >= 0xC0000000u) {
    return false;
  }
  const uint32_t vt = rex::memory::load_and_swap<uint32_t>(base + job);
  if (vt < 0x82000000u || vt >= 0x82A00000u) {
    return false;
  }
  return rex::memory::load_and_swap<uint32_t>(base + vt + 4) == kEffDispatcher;
}

}  // namespace

// Snapshot the mode word at the moment the job is queued - that is the
// parameter this particular job was submitted with.
PPC_FUNC_IMPL(rex_sub_821D4A78) {
  PPC_FUNC_PROLOGUE();
  if (REXCVAR_GET(ac6_effect_mode_fix)) {
    const uint32_t job = ctx.r5.u32;
    if (IsEffectManagerJob(base, job)) {
      const uint32_t mode = rex::memory::load_and_swap<uint32_t>(base + job + kEffModeOffset);
      const uint32_t arg = ctx.r6.u32;
      std::lock_guard<std::mutex> lk(g_modefix.mu);
      // Is (job, arg) actually unique among outstanding submissions? See the
      // counter declarations - a duplicate key with a differing mode is the one
      // way this fix can go wrong without any counter noticing.
      for (const EffModeEntry& other : g_modefix.e) {
        if (other.used || other.job != job || other.arg != arg) {
          continue;
        }
        ++g_modefix.key_collisions;
        if (other.mode != mode) {
          ++g_modefix.key_conflicts;
          if (g_modefix.key_conflicts <= 8) {
            REXLOG_WARN(
                "[AC6-MODEFIX] key collision: job {:08X} arg {:08X} outstanding with "
                "mode {} while submitting mode {} - (job, arg) is not a unique key here "
                "and a dispatch could take the wrong one (conflicts {}, benign {})",
                job, arg, other.mode, mode, g_modefix.key_conflicts, g_modefix.key_collisions);
          }
        }
      }
      // Overwriting a still-unused slot means a submission was never claimed by
      // a dispatch. Count it rather than losing it silently.
      EffModeEntry& slot = g_modefix.e[g_modefix.next];
      if (!slot.used) {
        ++g_modefix.dropped;
      }
      slot.job = job;
      slot.arg = arg;
      slot.mode = mode;
      slot.used = false;
      g_modefix.next = (g_modefix.next + 1) & 15;
    }
  }
  __imp__rex_sub_821D4A78(ctx, base);
}

// Dispatch from the snapshot instead of re-reading the racy mode word.
// Anything unmodelled falls through to the original, untouched.
PPC_FUNC_IMPL(rex_sub_822B3248) {
  PPC_FUNC_PROLOGUE();
  if (REXCVAR_GET(ac6_effect_mode_fix)) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t self_arg = ctx.r4.u32;
    uint32_t want = 0;
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(g_modefix.mu);
      // Newest first, so a repeated (job, arg) resolves to the latest submit.
      for (int i = 0; i < 16 && !have; ++i) {
        EffModeEntry& slot = g_modefix.e[(g_modefix.next - 1 - i) & 15];
        if (!slot.used && slot.job == self && slot.arg == self_arg) {
          want = slot.mode;
          slot.used = true;
          have = true;
        }
      }
      if (!have) {
        ++g_modefix.unmatched;
      }
    }
    if (have) {
      const uint32_t now = rex::memory::load_and_swap<uint32_t>(base + self + kEffModeOffset);
      {
        std::lock_guard<std::mutex> lk(g_modefix.mu);
        if (now != want) {
          ++g_modefix.corrections;
          if (g_modefix.corrections <= 8 || (g_modefix.corrections & 0xFF) == 0) {
            REXLOG_INFO(
                "[AC6-MODEFIX] mode word reads {} but this job was submitted with {} "
                "(corrected {}, in-order {}, unmatched {}, dropped {}, key conflicts {}, "
                "unmodelled draws {})",
                now, want, g_modefix.corrections, g_modefix.matched, g_modefix.unmatched,
                g_modefix.dropped, g_modefix.key_conflicts, g_modefix.unknown_draw);
          }
        } else {
          ++g_modefix.matched;
        }
      }

      const uint32_t dt_bits = ctx.r4.u32;
      if (want == 2) {
        // BOTH manager classes share the dispatcher at vtbl[+0x04], but they do
        // NOT share the draw at vtbl[+0x100]:
        //     CX360EffectManager (vtable 0x8205829C) -> 0x820B25D0
        //     CAce6EffectManager (vtable 0x82057BDC) -> 0x822B0AD8
        // An earlier revision only knew the first, so a CAce6EffectManager job
        // failed the check and fell through to the original - the racy path -
        // silently, because nothing counted it. That is a real escape hatch and
        // it is how a flicker survived on a tester's machine.
        const uint32_t vt = rex::memory::load_and_swap<uint32_t>(base + self);
        const uint32_t draw = (vt >= 0x82000000u && vt < 0x82A00000u)
                                  ? rex::memory::load_and_swap<uint32_t>(base + vt + 0x100)
                                  : 0u;
        if (draw == kEffDrawX360) {
          rex_sub_820B25D0(ctx, base);  // r3 = this, r4 = arg already in place
          return;
        }
        if (draw == kEffDrawAce6) {
          rex_sub_822B0AD8(ctx, base);
          return;
        }
        // Still unmodelled: fall through to the original rather than guess -
        // but COUNT it. Only two classes reference the dispatcher, so this
        // should be unreachable; if it ever fires, the escape hatch is open
        // again and we want to know instead of losing flickers to it.
        {
          std::lock_guard<std::mutex> lk(g_modefix.mu);
          ++g_modefix.unknown_draw;
          if (g_modefix.unknown_draw <= 8) {
            REXLOG_WARN(
                "[AC6-MODEFIX] unmodelled draw slot: this={:08X} vtable={:08X} "
                "vtbl[+0x100]={:08X} - falling through to the racy path ({} so far)",
                self, vt, draw, g_modefix.unknown_draw);
          }
        }
      } else if (want == 0 || want == 1) {
        float dt = 0.0f;
        std::memcpy(&dt, &dt_bits, sizeof(dt));
        ctx.fpscr.disableFlushMode();
        ctx.f1.f64 = static_cast<double>(dt);
        if (want == 0) {
          rex_sub_822B2E20(ctx, base);
        } else {
          rex_sub_822B0848(ctx, base);
        }
        return;
      } else if (want >= 3) {
        return;  // the original returns without dispatching
      }
    }
  }
  __imp__rex_sub_822B3248(ctx, base);
}
