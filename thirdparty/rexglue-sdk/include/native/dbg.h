// Native runtime - Debug utilities
// Part of the AC6 Recompilation native foundation

#pragma once

#include <atomic>
#include <cstdint>

#include <fmt/format.h>

namespace rex::debug {

// Returns true if a debugger is attached to this process.
// The state may change at any time (attach after launch, etc), so do not
// cache this value. Determining if the debugger is attached is expensive,
// though, so avoid calling it frequently.
bool IsDebuggerAttached();

// Breaks into the debugger if it is attached.
// If no debugger is present, a signal will be raised.
void Break();

namespace detail {
void DebugPrint(const char* s);
}

// Prints a message to the attached debugger.
// This bypasses the normal logging mechanism. If no debugger is attached it's
// likely to no-op.
template <typename... Args>
void DebugPrint(fmt::string_view format, const Args&... args) {
  detail::DebugPrint(fmt::vformat(format, fmt::make_format_args(args...)).c_str());
}

// CPU profiling.
//
// The scope macros aggregate wall time per named site across every thread and
// report once a second while the `profiling` cvar is on. Sites nest: a scope
// also reports SELF time, i.e. its inclusive time minus the time spent inside
// scopes opened underneath it, which is what tells you whether a stage is
// itself expensive or is just containing something that is.
//
// Off, a scope costs one relaxed atomic load and a predictable branch, so the
// annotations can stay in hot paths (they blanket the GPU thread already).
namespace profiling {

// Registers a site once and returns its index. Call through the macros; they
// keep the registration in a function-local static so it happens once.
//
// Sites are keyed on file:line, so two different functions never merge into one
// row - the existing call sites all pass the SUBSYSTEM as `category` ("gpu",
// "cpu", "hid") rather than anything site-specific, so category alone would
// collapse the entire graphics stack into a single bucket. The display name is
// composed as "category/function".
uint32_t RegisterSite(const char* category, const char* function, const char* file, int line);
// is_rate distinguishes the two kinds of counter. A GAUGE (COUNT_profile_set)
// is a level - "how many textures are resident" - and is reported as-is. A RATE
// (COUNT_profile_add) is an event count, reported PER GUEST FRAME and reset each
// report, which is what makes "submissions per frame" or "transfer draws per
// frame" readable.
uint32_t RegisterCounter(const char* name, bool is_rate);

void ScopeEnter(uint32_t site);
void ScopeExit(uint32_t site);

void SetCounter(uint32_t counter, int64_t value);
void AddCounter(uint32_t counter, int64_t delta);

// Names the calling thread in the report. Threads that never call this are
// reported by their OS id.
void NameThread(const char* name);
void ForgetThread();

// One frame boundary. Drives the periodic report; the per-frame columns are
// divided by the frames counted here.
void Flip();

void Flush();
void Shutdown();

// True while `profiling` is on. Read directly by the macros so a disabled
// scope never makes a call.
extern std::atomic<bool> g_enabled;
inline bool IsEnabled() { return g_enabled.load(std::memory_order_relaxed); }

// RAII half of the scope macros.
class ScopedTimer {
 public:
  explicit ScopedTimer(uint32_t site) : site_(site), active_(IsEnabled()) {
    if (active_) {
      ScopeEnter(site_);
    }
  }
  ~ScopedTimer() {
    if (active_) {
      ScopeExit(site_);
    }
  }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;

 private:
  uint32_t site_;
  bool active_;
};

}  // namespace profiling

#define REX_PROFILE_CAT_(a, b) a##b
#define REX_PROFILE_CAT(a, b) REX_PROFILE_CAT_(a, b)

#define REX_PROFILE_SCOPE(name)                                                 \
  static const uint32_t REX_PROFILE_CAT(rex_profile_site_, __LINE__) =          \
      ::rex::debug::profiling::RegisterSite(name, __func__, __FILE__, __LINE__); \
  ::rex::debug::profiling::ScopedTimer REX_PROFILE_CAT(rex_profile_scope_,      \
                                                       __LINE__)(              \
      REX_PROFILE_CAT(rex_profile_site_, __LINE__))

#define SCOPE_profile_cpu_f(name) REX_PROFILE_SCOPE(name)
// The detail argument is accepted for source compatibility but not recorded -
// per-invocation detail would need a sampling buffer, not an accumulator.
#define SCOPE_profile_cpu_i(name, detail) REX_PROFILE_SCOPE(name)

// GPU profiling stubs. These need timestamp query pools on the backend, which
// the Vulkan path does not have yet - deliberately still no-ops so they do not
// silently report CPU time as if it were GPU time.
#define SCOPE_profile_gpu_f(name)
#define SCOPE_profile_gpu_i(name, detail)

#define PROFILE_THREAD_ENTER(name) ::rex::debug::profiling::NameThread(name)
#define PROFILE_THREAD_EXIT() ::rex::debug::profiling::ForgetThread()

#define COUNT_profile_set(name, value)                                    \
  do {                                                                    \
    if (::rex::debug::profiling::IsEnabled()) {                           \
      static const uint32_t REX_PROFILE_CAT(rex_profile_counter_,         \
                                            __LINE__) =                   \
          ::rex::debug::profiling::RegisterCounter(name, false);          \
      ::rex::debug::profiling::SetCounter(                                \
          REX_PROFILE_CAT(rex_profile_counter_, __LINE__),                \
          static_cast<int64_t>(value));                                   \
    }                                                                     \
  } while (0)

#define COUNT_profile_add(name, value)                                    \
  do {                                                                    \
    if (::rex::debug::profiling::IsEnabled()) {                           \
      static const uint32_t REX_PROFILE_CAT(rex_profile_counter_,         \
                                            __LINE__) =                   \
          ::rex::debug::profiling::RegisterCounter(name, true);           \
      ::rex::debug::profiling::AddCounter(                                \
          REX_PROFILE_CAT(rex_profile_counter_, __LINE__),                \
          static_cast<int64_t>(value));                                   \
    }                                                                     \
  } while (0)

// Kept for source compatibility with the call sites that predate the
// profiling namespace.
class Profiler {
 public:
  static void OnThreadEnter(const char* name = nullptr) { profiling::NameThread(name); }
  static void OnThreadExit() { profiling::ForgetThread(); }
  static void ThreadEnter(const char* name = nullptr) { profiling::NameThread(name); }
  static void ThreadExit() { profiling::ForgetThread(); }
  static void Flip() { profiling::Flip(); }
  static void Flush() { profiling::Flush(); }
  static void Shutdown() { profiling::Shutdown(); }
  static bool is_enabled() { return profiling::IsEnabled(); }
};

}  // namespace rex::debug
