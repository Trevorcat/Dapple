#pragma once
// Thread placement of the calling process (docs/engine/pipeline-architecture.md section 56): what a thread runs on (nice, policy, allowed cpus,
// cpuset), a fixed CPU workload to compare a process with a native binary, and the scope a one-off speed probe runs in.
//
// Diagnostics are off by default: on Android `setprop debug.dapple.diag_sched 1` / `debug.dapple.diag_upload 1`, elsewhere the env variables
// DAPPLE_DIAG_SCHED / DAPPLE_DIAG_UPLOAD. Header only (the engine, the demosaic node and the C layer use it).
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#define DAPPLE_SCHED_LINUX 1
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace dapple::engine::sched {

// A debug switch: the Android property (read on every call, a cheap lookup) or the environment variable.
inline bool diagOn(const char* prop, const char* env) {
#if defined(__ANDROID__)
  char v[PROP_VALUE_MAX] = {0};
  if (__system_property_get(prop, v) > 0 && v[0] == '1') return true;
#else
  (void)prop;
#endif
  const char* e = std::getenv(env);
  return e && e[0] == '1';
}
inline bool diagSched() { return diagOn("debug.dapple.diag_sched", "DAPPLE_DIAG_SCHED"); }
inline bool diagUpload() { return diagOn("debug.dapple.diag_upload", "DAPPLE_DIAG_UPLOAD"); }

inline int tid() {
#ifdef DAPPLE_SCHED_LINUX
  return int(::syscall(SYS_gettid));
#else
  return 0;
#endif
}

inline int onlineCpus() { return int(std::max(1u, std::thread::hardware_concurrency())); }

struct Placement {
  int tid = 0;
  int nice = 0;
  int policy = 0;        // SCHED_OTHER 0, SCHED_BATCH 3, SCHED_IDLE 5 ...
  int cpus = 0;          // allowed cpus (affinity mask, already limited by the cpuset)
  std::uint64_t mask = 0;
  std::string cpuset;    // /proc/self/task/<tid>/cpuset (Android: /top-app, /foreground, /background ...)
  bool known = false;
};

inline Placement placementOf(int t = 0) {
  Placement p;
  p.tid = t ? t : tid();
#ifdef DAPPLE_SCHED_LINUX
  p.known = true;
  errno = 0;
  p.nice = ::getpriority(PRIO_PROCESS, id_t(p.tid));
  p.policy = ::sched_getscheduler(p.tid);
  cpu_set_t s;
  CPU_ZERO(&s);
  if (::sched_getaffinity(p.tid, sizeof s, &s) == 0) {
    p.cpus = CPU_COUNT(&s);
    for (int c = 0; c < 64; ++c)
      if (CPU_ISSET(c, &s)) p.mask |= std::uint64_t(1) << c;
  }
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%d/cpuset", p.tid);
  if (std::FILE* f = std::fopen(path, "r")) {
    char b[96] = {0};
    if (std::fgets(b, sizeof b, f)) p.cpuset = b;
    std::fclose(f);
    while (!p.cpuset.empty() && (p.cpuset.back() == '\n' || p.cpuset.back() == '\r')) p.cpuset.pop_back();
  }
#endif
  return p;
}

inline std::string describe(const Placement& p) {
  if (!p.known) return "n/a";
  char b[160];
  std::snprintf(b, sizeof b, "tid %d nice %d policy %d cpus %d mask %llx cpuset %s", p.tid, p.nice, p.policy, p.cpus, (unsigned long long)p.mask,
                p.cpuset.empty() ? "?" : p.cpuset.c_str());
  return b;
}

// A fixed CPU workload: `chunks` items of dependent float math, claimed dynamically by `threads` threads the caller spawns (as the CPU demosaic
// node does: they inherit the caller's priority and cpus), 0 = every cpu. Returns the wall time in ms.
inline double fixedWorkMs(int threads, int chunks = 256) {
  if (threads <= 0) threads = onlineCpus();
  std::atomic<int> next{0};
  std::atomic<std::uint32_t> sink{0};
  auto worker = [&] {
    for (;;) {
      const int i = next.fetch_add(1);
      if (i >= chunks) break;
      float a = 1.0f + float(i) * 1e-6f, b = 0.999f;
      for (int k = 0; k < 200000; ++k) a = a * b + 1e-4f, b = b * 0.99999f + 1e-5f;
      sink.fetch_add(std::uint32_t(a * 1000.f), std::memory_order_relaxed);
    }
  };
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (int i = 1; i < threads; ++i) th.emplace_back(worker);
  worker();
  for (auto& t : th) t.join();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Runs a one-off speed probe at the placement real work gets: nice 0 and every online cpu for the calling thread (threads it spawns inherit
// both), restored afterwards. A caller marked background (dapple_thread_background: nice 10, efficiency cores) would otherwise time the CPU on
// the efficiency cores at low priority (Note10+ in the app: X-Trans CPU probe 1022 ms against 213 ms native). ok(): the thread really got
// nice <= 0 and the mask of every cpu, outside a background cpuset (an app not in front); a probe that ran without it is not cached.
class FullSpeedScope {
 public:
  FullSpeedScope() {
#ifdef DAPPLE_SCHED_LINUX
    before_ = placementOf();
    CPU_ZERO(&prev_);
    havePrev_ = ::sched_getaffinity(0, sizeof prev_, &prev_) == 0;
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < onlineCpus() && c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
    const bool widened = ::sched_setaffinity(0, sizeof all, &all) == 0;
    if (before_.nice > 0) ::setpriority(PRIO_PROCESS, id_t(before_.tid), 0);
    after_ = placementOf();
    // the mask read back can lack cpus the kernel isolates for a while (Qualcomm core control: the Note10+ showed 6 or 7 of 8 when idle, natively too):
    // that is the same for real work, so it does not count; a background cpuset (the app not in front) does
    ok_ = after_.nice <= 0 && widened && after_.cpuset.find("background") == std::string::npos && after_.cpuset.find("restricted") == std::string::npos;
#else
    ok_ = true;
#endif
  }
  ~FullSpeedScope() {
#ifdef DAPPLE_SCHED_LINUX
    if (havePrev_) ::sched_setaffinity(0, sizeof prev_, &prev_);
    if (before_.nice != after_.nice) ::setpriority(PRIO_PROCESS, id_t(before_.tid), before_.nice);
#endif
  }
  FullSpeedScope(const FullSpeedScope&) = delete;
  FullSpeedScope& operator=(const FullSpeedScope&) = delete;
  bool ok() const { return ok_; }
  const Placement& before() const { return before_; }
  const Placement& after() const { return after_; }

 private:
  Placement before_, after_;
#ifdef DAPPLE_SCHED_LINUX
  cpu_set_t prev_;
  bool havePrev_ = false;
#endif
  bool ok_ = false;
};

}  // namespace dapple::engine::sched
