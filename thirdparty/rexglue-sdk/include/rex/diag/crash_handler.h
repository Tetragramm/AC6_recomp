/**
 * @file        rex/diag/crash_handler.h
 * @brief       Crash reporting to the exact guest instruction, symbol-free at
 *              runtime.
 *
 * On any fatal event (unhandled SEH exception, abort/assert_always,
 * std::terminate, pure-virtual call, invalid CRT parameter) one
 * crash-<timestamp>.txt is written that stands alone: the fault, the guest
 * call stack and registers (via the thread-local PPCContext and the PPC
 * backchain - no symbols needed), the host stack as raw exe RVAs (decoded
 * offline against the reproducible-build PDB), a guest-vs-host fault
 * classification, and the embedded session report + log tail.
 *
 * An atexit hook additionally writes a lighter report when the process exits
 * without the orderly shutdown path having run. TerminateProcess and power
 * loss are physically uncatchable and leave nothing behind by definition.
 *
 * This is self-contained: it needs no other diagnostics module. A host that
 * has more context to offer (a session report, say) can push it into the
 * crash file with SetContextProvider.
 *
 * The Windows implementation lives in diag_crash_handler.cpp; other
 * platforms currently get no-op stubs (a POSIX signal implementation slots
 * in behind this same header).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct PPCFuncMapping;

namespace rex {
class LogCaptureSink;
}

namespace rex::diag::crash {

struct InstallOptions {
  /** Where crash-<timestamp>.txt files go (same directory as the report). */
  std::filesystem::path directory;
  std::string app_name;
  std::string build_title;
  std::string build_commit;
  std::string build_timestamp;
  /** Ties the crash file to the run. Leave empty to have one generated. */
  std::string session_id;
};

/**
 * Supplies an extra block copied verbatim into the crash file under
 * [CONTEXT]. Called ON THE CRASH PATH, so it must not allocate, take locks,
 * or touch the filesystem - hand back a pointer to memory that already
 * exists. No provider = no section.
 */
using ContextProvider = void (*)(const char** out_data, size_t* out_size);
void SetContextProvider(ContextProvider provider);

/** Install all fatal-event hooks. Idempotent. Preallocates every buffer the
 *  crash path needs so the handlers never touch the heap. */
void Install(const InstallOptions& options);

/** Guest memory range for fault classification and safe backchain reads.
 *  Pass the host base of the guest mapping and its extent in bytes. */
void SetGuestMemoryBounds(const void* host_base, uint64_t extent_bytes);

/**
 * The generated guest->host function table (0-guest-terminated). A copy is
 * sorted by host address at call time so a crash can name the guest function
 * (rex_sub_XXXXXXXX) containing any host return address with no symbols.
 */
void SetGuestFunctionTable(const PPCFuncMapping* mappings);

/** Ring-buffer sink whose tail gets embedded into crash reports (best
 *  effort: skipped without blocking if its lock is held). */
void SetLogTailSink(rex::LogCaptureSink* sink);

/** Reserve handler stack space on the calling thread so a stack-overflow
 *  report can still be written. Call once per guest thread. */
void PrepareCurrentThread();

/** The orderly shutdown path ran; the atexit unexpected-exit report is
 *  suppressed. */
void NotifyOrderlyShutdown();

}  // namespace rex::diag::crash
