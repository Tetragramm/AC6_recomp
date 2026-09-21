/**
 * @file        core/profiling.cpp
 * @brief       Named CPU scope aggregation behind the SCOPE_profile_* macros
 *
 * The macros in native/dbg.h were empty stubs, so the ~22 scope annotations
 * and the counters already placed through the graphics stack produced nothing.
 * This gives them a real backend: wall time is accumulated per named site
 * across every thread and reported once an interval while `profiling` is on.
 *
 * Sites nest, so each reports INCLUSIVE time (the whole scope) and SELF time
 * (inclusive minus whatever nested scopes ran inside it). Self time is what
 * distinguishes a stage that is itself expensive from one that merely contains
 * something expensive - the distinction that matters when the outermost scope
 * is the whole GPU thread.
 *
 * Accumulation is per thread and lock-free; the only locks are on registration
 * (once per site) and on the report.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include <native/dbg.h>

REXCVAR_DEFINE_BOOL(profiling, false, "Profiling",
                    "Aggregate the named CPU scopes and report a per-frame breakdown to the log. "
                    "Reports at error level so it survives ac6_performance_mode.");
REXCVAR_DEFINE_DOUBLE(profiling_interval_s, 1.0, "Profiling",
                      "Seconds between profiling reports.");
REXCVAR_DEFINE_INT32(profiling_rows, 20, "Profiling",
                     "How many scopes to list per report, ordered by self time.");

namespace rex::debug::profiling {

std::atomic<bool> g_enabled{false};

namespace {

// Sites and counters are registered once each from function-local statics, so
// these bounds only have to cover distinct annotations in the binary, not
// invocations. Overflow degrades to "not recorded" rather than misreporting.
constexpr uint32_t kMaxSites = 512;
constexpr uint32_t kMaxCounters = 256;
// Deeper nesting than this stops accumulating child time; the scope is still
// timed, it just cannot subtract its children.
constexpr uint32_t kMaxDepth = 64;

struct ThreadBlock {
  // Despite the names, these hold NowTicks() units, not nanoseconds; the
  // report converts them (see NowTicks).
  uint64_t inclusive_ns[kMaxSites]{};
  uint64_t self_ns[kMaxSites]{};
  uint64_t calls[kMaxSites]{};

  // Nesting stack. entry_ns[d] is when the scope at depth d started;
  // child_ns[d] accumulates the inclusive time of scopes closed inside it.
  uint64_t entry_ns[kMaxDepth]{};
  uint64_t child_ns[kMaxDepth]{};
  uint32_t site_at[kMaxDepth]{};
  uint32_t depth{0};

  // Per-thread report baselines. Reporting is per thread rather than summed:
  // summing a site across threads can exceed wall-clock time, which reads as
  // nonsense. Per thread it also shows WHICH thread is the bottleneck.
  uint64_t reported_inclusive[kMaxSites]{};
  uint64_t reported_self[kMaxSites]{};
  uint64_t reported_calls[kMaxSites]{};

  std::string name;

  // For CPU utilisation: the thread's own CPU clock, sampled at each report.
  // busy% = cpu time delta / wall delta. This is what separates "this thread
  // is the frame" from "this thread is waiting on another one".
#if defined(__linux__)
  clockid_t cpu_clock{};
  bool has_cpu_clock{false};
#endif
  uint64_t reported_cpu_ns{0};
};

std::mutex g_mutex;
// Keyed on "file:line" so two functions never collapse into one row; the value
// stored for display is "category/function".
std::unordered_map<std::string, uint32_t> g_site_ids;
std::vector<std::string> g_site_names;
std::unordered_map<std::string, uint32_t> g_counter_ids;
std::vector<std::string> g_counter_names;
std::vector<bool> g_counter_is_rate;
std::vector<std::unique_ptr<ThreadBlock>> g_blocks;

std::atomic<int64_t> g_counters[kMaxCounters];

thread_local ThreadBlock* t_block = nullptr;

// Reporting state, only touched under g_mutex from Flip().
uint64_t g_frames_since_report = 0;
uint64_t g_last_report_ns = 0;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// What a scope reads on entry and exit. Not steady_clock: on a machine whose
// clocksource is HPET or ACPI-PM rather than the TSC, clock_gettime costs
// ~1 us per read even through the vDSO, and at ~14 scopes a draw the profiler
// was 40% of the frame it measured on a Radeon 780M laptop (BeginSubmission's
// two-bool early-out timed at 2.0 us there against 0.07 on a TSC desktop).
// The hardware counter is a few nanoseconds anywhere. Scopes accumulate raw
// ticks; the report converts them with the tick/ns ratio measured over its
// own interval, so no calibration is needed and no drift accumulates.
inline uint64_t NowTicks() {
#if defined(__x86_64__) || defined(_M_X64)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t ticks;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#else
  return NowNs();
#endif
}
// Ticks and nanoseconds at the last report, for the conversion ratio.
uint64_t g_last_report_ticks = 0;
double g_ns_per_tick = 1.0;

ThreadBlock* AcquireBlock() {
  if (t_block) {
    return t_block;
  }
  auto block = std::make_unique<ThreadBlock>();
  ThreadBlock* raw = block.get();
#if defined(__linux__)
  raw->has_cpu_clock = pthread_getcpuclockid(pthread_self(), &raw->cpu_clock) == 0;
#endif
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_blocks.push_back(std::move(block));
  }
  t_block = raw;
  return raw;
}

uint32_t RegisterNamed(const char* name, std::unordered_map<std::string, uint32_t>& ids,
                       std::vector<std::string>& names, uint32_t limit) {
  if (!name) {
    return UINT32_MAX;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = ids.find(name);
  if (it != ids.end()) {
    return it->second;
  }
  if (names.size() >= limit) {
    return UINT32_MAX;
  }
  const uint32_t id = uint32_t(names.size());
  names.emplace_back(name);
  ids.emplace(name, id);
  return id;
}

// Last two path components, so a row reads "vulkan/command_processor.cpp"
// rather than an absolute path.
std::string ShortFile(const char* file) {
  if (!file) {
    return {};
  }
  std::string path(file);
  size_t last = path.find_last_of('/');
  if (last == std::string::npos) {
    return path;
  }
  size_t prev = path.find_last_of('/', last - 1);
  return prev == std::string::npos ? path.substr(last + 1) : path.substr(prev + 1);
}

}  // namespace

uint32_t RegisterSite(const char* category, const char* function, const char* file, int line) {
  const std::string key = ShortFile(file) + ":" + std::to_string(line);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_site_ids.find(key);
  if (it != g_site_ids.end()) {
    return it->second;
  }
  if (g_site_names.size() >= kMaxSites) {
    return UINT32_MAX;
  }
  const uint32_t id = uint32_t(g_site_names.size());
  // Two different files can hold a scope in a same-named function ("Execute"),
  // which would otherwise print as two identical rows - carry the location.
  std::string display;
  if (category && *category) {
    display += category;
    display += '/';
  }
  display += function ? function : "?";
  display += " @";
  display += key;
  g_site_names.emplace_back(std::move(display));
  g_site_ids.emplace(key, id);
  return id;
}

uint32_t RegisterCounter(const char* name, bool is_rate) {
  const uint32_t id = RegisterNamed(name, g_counter_ids, g_counter_names, kMaxCounters);
  if (id != UINT32_MAX) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_counter_is_rate.size() <= id) {
      g_counter_is_rate.resize(id + 1, false);
    }
    g_counter_is_rate[id] = is_rate;
  }
  return id;
}

void ScopeEnter(uint32_t site) {
  if (site == UINT32_MAX) {
    return;
  }
  ThreadBlock* block = AcquireBlock();
  const uint32_t depth = block->depth;
  if (depth >= kMaxDepth) {
    ++block->depth;  // still balanced by ScopeExit, just not recorded
    return;
  }
  block->site_at[depth] = site;
  block->child_ns[depth] = 0;
  block->entry_ns[depth] = NowTicks();
  ++block->depth;
}

void ScopeExit(uint32_t site) {
  if (site == UINT32_MAX) {
    return;
  }
  ThreadBlock* block = t_block;
  if (!block || block->depth == 0) {
    return;
  }
  --block->depth;
  const uint32_t depth = block->depth;
  if (depth >= kMaxDepth) {
    return;
  }
  // A scope that was entered while profiling was off (or from a mismatched
  // nesting) would attribute someone else's time - drop it instead.
  if (block->site_at[depth] != site) {
    return;
  }
  const uint64_t elapsed = NowTicks() - block->entry_ns[depth];
  block->inclusive_ns[site] += elapsed;
  block->self_ns[site] += elapsed - block->child_ns[depth];
  ++block->calls[site];
  if (depth > 0) {
    block->child_ns[depth - 1] += elapsed;
  }
}

void SetCounter(uint32_t counter, int64_t value) {
  if (counter == UINT32_MAX) {
    return;
  }
  g_counters[counter].store(value, std::memory_order_relaxed);
}

void AddCounter(uint32_t counter, int64_t delta) {
  if (counter == UINT32_MAX) {
    return;
  }
  g_counters[counter].fetch_add(delta, std::memory_order_relaxed);
}

void NameThread(const char* name) {
  ThreadBlock* block = AcquireBlock();
  if (!name || !*name) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  block->name = name;
}

void ForgetThread() {
  // The block deliberately outlives the thread: a thread that exits mid-interval
  // still contributed time that belongs in the report. Blocks are freed at
  // Shutdown only.
}

void Flush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_report_ns = 0;
}

void Shutdown() {
  g_enabled.store(false, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_blocks.clear();
}

namespace {

// Nothing in the tree calls PROFILE_THREAD_ENTER, but rexglue does name its
// threads through the OS ("GPU Commands", "Vulkan Pipelines", ...), so take the
// name from there rather than reporting bare ids.
void EnsureThreadName(ThreadBlock* block) {
  if (!block->name.empty()) {
    return;
  }
#if defined(__linux__)
  char buf[32] = {};
  if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0 && buf[0]) {
    block->name = buf;
    return;
  }
#endif
  block->name = "thread";
}

// Snapshot one thread's deltas since the last report, resetting its baselines.
struct Row {
  uint32_t site;
  double self_ms;
  double incl_ms;
  double calls;
};

void CollectThread(ThreadBlock* block, size_t site_count, double frames, std::vector<Row>& out) {
  out.clear();
  for (size_t i = 0; i < site_count; ++i) {
    const uint64_t d_incl = block->inclusive_ns[i] - block->reported_inclusive[i];
    const uint64_t d_self = block->self_ns[i] - block->reported_self[i];
    const uint64_t d_calls = block->calls[i] - block->reported_calls[i];
    block->reported_inclusive[i] = block->inclusive_ns[i];
    block->reported_self[i] = block->self_ns[i];
    block->reported_calls[i] = block->calls[i];
    if (d_calls == 0) {
      continue;
    }
    // The accumulators hold ticks; g_ns_per_tick was measured over this
    // report interval.
    out.push_back(Row{uint32_t(i), double(d_self) * g_ns_per_tick / 1.0e6 / frames,
                      double(d_incl) * g_ns_per_tick / 1.0e6 / frames, double(d_calls) / frames});
  }
  std::sort(out.begin(), out.end(),
            [](const Row& a, const Row& b) { return a.self_ms > b.self_ms; });
}

}  // namespace

void Flip() {
  // The cvar is sampled here rather than at every scope so the hot path stays a
  // relaxed load. Flip runs once per guest frame (the XE_SWAP packet).
  const bool enabled = REXCVAR_GET(profiling);
  const bool was_enabled = g_enabled.exchange(enabled, std::memory_order_relaxed);
  if (!enabled) {
    return;
  }

  // Name the calling thread opportunistically - Flip runs on the GPU thread.
  EnsureThreadName(AcquireBlock());

  const uint64_t now = NowNs();
  const uint64_t now_ticks = NowTicks();
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frames_since_report;

  const size_t site_count = g_site_names.size();
  std::vector<Row> rows;

  if (!was_enabled || g_last_report_ns == 0) {
    // Just turned on: take a baseline instead of reporting whatever happened to
    // accumulate beforehand.
    g_last_report_ns = now;
    g_last_report_ticks = now_ticks;
    g_frames_since_report = 0;
    for (const auto& block : g_blocks) {
      CollectThread(block.get(), site_count, 1.0, rows);
    }
    return;
  }

  const double configured = REXCVAR_GET(profiling_interval_s);
  const double interval_s = configured > 0.01 ? configured : 0.01;
  const double elapsed_s = double(now - g_last_report_ns) / 1.0e9;
  if (elapsed_s < interval_s || g_frames_since_report == 0) {
    return;
  }

  const double frames = double(g_frames_since_report);
  const int32_t max_rows = REXCVAR_GET(profiling_rows);
  if (now_ticks > g_last_report_ticks) {
    g_ns_per_tick = double(now - g_last_report_ns) / double(now_ticks - g_last_report_ticks);
  }
  g_last_report_ticks = now_ticks;

  REXLOG_ERROR("[PROFILE] {} guest frames in {:.2f}s = {:.1f} fps, {:.2f} ms/frame; "
               "columns are ms per guest frame",
               g_frames_since_report, elapsed_s, frames / elapsed_s,
               elapsed_s * 1000.0 / frames);

  for (const auto& block : g_blocks) {
    CollectThread(block.get(), site_count, frames, rows);
    // A thread with no scopes this interval still gets its utilisation line -
    // the game's sim thread has almost no annotations and is exactly the one
    // whose busy% we need.
    bool has_clock = false;
#if defined(__linux__)
    has_clock = block->has_cpu_clock;
#endif
    if (rows.empty() && !has_clock) {
      continue;
    }
    double thread_self = 0.0;
    for (const Row& row : rows) {
      thread_self += row.self_ms;
    }
    double busy_pct = -1.0;
#if defined(__linux__)
    if (block->has_cpu_clock) {
      timespec ts{};
      if (clock_gettime(block->cpu_clock, &ts) == 0) {
        const uint64_t cpu_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
        if (block->reported_cpu_ns) {
          busy_pct = 100.0 * double(cpu_ns - block->reported_cpu_ns) / double(now - g_last_report_ns);
        }
        block->reported_cpu_ns = cpu_ns;
      }
    }
#endif
    if (busy_pct >= 0.0) {
      REXLOG_ERROR("[PROFILE] thread {:<18} {:7.3f} ms/frame in scopes, {:5.1f}% of a core busy",
                   block->name.empty() ? "?" : block->name, thread_self, busy_pct);
    } else {
      REXLOG_ERROR("[PROFILE] thread {:<18} {:7.3f} ms/frame in scopes",
                   block->name.empty() ? "?" : block->name, thread_self);
    }
    int32_t printed = 0;
    for (const Row& row : rows) {
      if (max_rows > 0 && printed >= max_rows) {
        break;
      }
      REXLOG_ERROR("[PROFILE]     {:<72} self {:7.3f}  incl {:7.3f}  x{:.1f}",
                   g_site_names[row.site], row.self_ms, row.incl_ms, row.calls);
      ++printed;
    }
  }

  for (size_t i = 0; i < g_counter_names.size(); ++i) {
    const bool is_rate = i < g_counter_is_rate.size() && g_counter_is_rate[i];
    if (is_rate) {
      // Reset so the next report covers only the next interval.
      const int64_t total = g_counters[i].exchange(0, std::memory_order_relaxed);
      REXLOG_ERROR("[PROFILE]   per-frame {:<38} {:.1f}", g_counter_names[i],
                   double(total) / frames);
    } else {
      REXLOG_ERROR("[PROFILE]   gauge     {:<38} {}", g_counter_names[i],
                   g_counters[i].load(std::memory_order_relaxed));
    }
  }

  g_last_report_ns = now;
  g_frames_since_report = 0;
}

}  // namespace rex::debug::profiling
