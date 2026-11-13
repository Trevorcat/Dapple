#pragma once
// Platform scheduling helpers for the interactive path: big.LITTLE topology, thread classes (interactive /
// background), a dedicated render thread, and Android ADPF (APerformanceHint + AThermal), all resolved at run
// time (dlsym) with safe fallbacks: on desktop and on devices without the APIs every call is a no-op.
//
// Kernels stay pure per-tile functions; none of this touches pixel code.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <dlfcn.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#define DAPPLE_SYSPERF_LINUX 1
#endif
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

namespace dapple::sys {

struct Topology {
  std::vector<int> perf;      // performance (big/prime) cpus
  std::vector<int> efficient; // little cpus
  int total = 0;
  bool hetero() const { return !perf.empty() && !efficient.empty(); }
};

#ifdef DAPPLE_SYSPERF_LINUX
inline long readLong(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return -1;
  long v = -1;
  if (std::fscanf(f, "%ld", &v) != 1) v = -1;
  std::fclose(f);
  return v;
}
inline int gettid_() { return int(::syscall(SYS_gettid)); }
#else
inline int gettid_() { return 0; }
#endif

// Classifies cpus by capacity (cpu_capacity, or cpuinfo_max_freq as a proxy; <= 0 = unknown): a cpu is
// "performance" when it reaches 60% of the best one. Unknown or homogeneous capacities -> no split (perf and
// efficient empty), i.e. no pinning anywhere. Pure, unit tested.
inline Topology classify(const std::vector<long>& cap) {
  Topology r;
  r.total = int(cap.size());
  if (cap.empty()) return r;
  const long mx = *std::max_element(cap.begin(), cap.end());
  if (mx > 0 && std::all_of(cap.begin(), cap.end(), [](long c) { return c > 0; }))
    for (int i = 0; i < r.total; ++i) (cap[std::size_t(i)] * 10 >= mx * 6 ? r.perf : r.efficient).push_back(i);
  if (r.efficient.empty()) r.perf.clear();  // homogeneous: no pinning
  return r;
}

inline const Topology& topology() {
  static const Topology t = [] {
    const int total = int(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<long> cap(std::size_t(total), -1);
#ifdef DAPPLE_SYSPERF_LINUX
    for (int i = 0; i < total; ++i) {
      const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(i);
      long c = readLong(b + "/cpu_capacity");
      if (c <= 0) c = readLong(b + "/cpufreq/cpuinfo_max_freq");
      cap[std::size_t(i)] = c;
    }
#endif
    return classify(cap);
  }();
  return t;
}

// Cpus the interactive thread pool runs on. The performance cores when there are at least 4 of them
// (Snapdragon 680 4+4, 7-series 1+3+4 / 4+4, 8 Elite 2+6...): the frame is latency bound and a little core
// would hold the last chunk of every pass. With fewer big cores (Helio G85 2+6, 2+6 Unisoc...) every cpu takes
// part: the chunks are claimed dynamically, so the slower cores just take fewer of them, and 6 little cores add
// more throughput than they cost in tail latency.
#if defined(__APPLE__)
// Performance cores of this device (A11 / A12: 2, M1: 4); the burst never uses the efficiency cores (hw.perflevel0 exists on macOS 12+ / iOS 15+, older: 2 for
// six-core phones, half of the cores otherwise).
inline int applePerfCores() {
  static const int n = [] {
    int v = 0;
    std::size_t sz = sizeof v;
    if (::sysctlbyname("hw.perflevel0.logicalcpu", &v, &sz, nullptr, 0) == 0 && v > 0) return v;
    const int total = int(std::max(1u, std::thread::hardware_concurrency()));
    return total <= 6 ? std::min(total, 2) : std::max(2, total / 2);
  }();
  return n;
}
#endif
inline std::vector<int> poolCpus(const Topology& t) {
#if defined(__APPLE__)
  return std::vector<int>(std::size_t(std::min(applePerfCores(), std::max(1, t.total))), 0);  // only the count matters (no affinity API on Darwin)
#endif
  if (t.hetero() && t.perf.size() >= 4) return t.perf;
  std::vector<int> all;
  for (int i = 0; i < t.total; ++i) all.push_back(i);
  return all;
}

inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// Interactive render threads (pool workers + the render thread) keep the normal priority (nice 0). Tried nice +4 (so
// the UI thread always wins a core): native frame time rose (27 -> 43 ms median, N5 profile, together with other
// changes) and UI-thread jank did not improve (mid profile 13.9% -> 19%); with barrier-synchronised passes a
// preempted low-priority worker holding the last chunk stalls the whole pass. Kept as a single switch point.
inline constexpr int kInteractiveNice = 0;
inline void setInteractivePriority() {
#if defined(__APPLE__)
  ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
#ifdef DAPPLE_SYSPERF_LINUX
  if (kInteractiveNice != 0) ::setpriority(PRIO_PROCESS, gettid_(), kInteractiveNice);
#endif
}

inline bool setAffinity(const std::vector<int>& cpus) {
#ifdef DAPPLE_SYSPERF_LINUX
  if (cpus.empty()) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : cpus) CPU_SET(c, &set);
  return ::sched_setaffinity(0, sizeof set, &set) == 0;  // pid 0 = calling thread
#else
  (void)cpus;
  return false;
#endif
}

// Thread classes. Interactive: performance cores. Background: efficiency cores + nice 10 (best effort).
inline thread_local int g_class = 0;  // 0 normal, 1 interactive, 2 background
inline bool inBackground() { return g_class == 2; }

inline void setBackground(bool on) {
  const int was = g_class;
  (void)was;
  g_class = on ? 2 : 0;
#if defined(__APPLE__)
  ::pthread_set_qos_class_self_np(on ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
#endif
#ifdef DAPPLE_SYSPERF_LINUX
  const Topology& t = topology();
  if (on && was != 2) {
    ::setpriority(PRIO_PROCESS, gettid_() , 10);
    if (t.hetero()) setAffinity(t.efficient);
  } else if (!on && was == 2) {
    ::setpriority(PRIO_PROCESS, gettid_(), 0);  // may be refused (RLIMIT_NICE); the thread then stays nice
    std::vector<int> all;
    for (int i = 0; i < t.total; ++i) all.push_back(i);
    if (t.hetero()) setAffinity(all);
  }
#endif
}

// ---- Android ADPF -------------------------------------------------------------------------------------
struct Adpf {
  using GetMgr = void* (*)();
  using Create = void* (*)(void*, const int32_t*, std::size_t, int64_t);
  using Report = int (*)(void*, int64_t);
  using Update = int (*)(void*, int64_t);
  using Close = void (*)(void*);
  using ThAcq = void* (*)();
  using ThStatus = int (*)(void*);
  using ThHead = float (*)(void*, int);
  GetMgr getMgr = nullptr;
  Create create = nullptr;
  Report report = nullptr;
  Update update = nullptr;
  Close close = nullptr;
  ThAcq thAcq = nullptr;
  ThStatus thStatus = nullptr;
  ThHead thHead = nullptr;
  void* mgr = nullptr;
  void* thermal = nullptr;
  void* session = nullptr;
  std::vector<int32_t> sessionTids;
  int64_t target = 0;
  bool failed = false;
  std::mutex mtx;
  bool loaded = false;

  void load() {
    if (loaded) return;
    loaded = true;
#ifdef DAPPLE_SYSPERF_LINUX
#ifdef __ANDROID__
    void* h = ::dlopen("libandroid.so", RTLD_NOW);
    if (!h) return;
    getMgr = reinterpret_cast<GetMgr>(::dlsym(h, "APerformanceHint_getManager"));
    create = reinterpret_cast<Create>(::dlsym(h, "APerformanceHint_createSession"));
    report = reinterpret_cast<Report>(::dlsym(h, "APerformanceHint_reportActualWorkDuration"));
    update = reinterpret_cast<Update>(::dlsym(h, "APerformanceHint_updateTargetWorkDuration"));
    close = reinterpret_cast<Close>(::dlsym(h, "APerformanceHint_closeSession"));
    thAcq = reinterpret_cast<ThAcq>(::dlsym(h, "AThermal_acquireManager"));
    thStatus = reinterpret_cast<ThStatus>(::dlsym(h, "AThermal_getCurrentThermalStatus"));
    thHead = reinterpret_cast<ThHead>(::dlsym(h, "AThermal_getThermalHeadroom"));
    if (getMgr) mgr = getMgr();
    if (thAcq) thermal = thAcq();
#endif
#endif
  }
  bool hintAvailable() { load(); return mgr && create && report; }
  bool sessionActive() {
    std::lock_guard<std::mutex> l(mtx);
    return session != nullptr;
  }

  // (Re)creates the session for exactly these threads when the set changed.
  void ensure(const std::vector<int32_t>& tids) {
    if (!hintAvailable() || target <= 0) return;
    std::lock_guard<std::mutex> l(mtx);
    if (tids == sessionTids && (session || failed)) return;  // a refused set is not retried every frame
    if (session && close) close(session);
    session = create(mgr, tids.data(), tids.size(), target);
    failed = session == nullptr;
    sessionTids = tids;
  }
  void reportDuration(int64_t ns) {
    std::lock_guard<std::mutex> l(mtx);
    if (session && report) report(session, ns);
  }
  void setTarget(int64_t ns) {
    std::lock_guard<std::mutex> l(mtx);
    target = ns;
    if (session && update && ns > 0) update(session, ns);
  }
  // headroom in [0..1+] where 1.0 = severe throttling; <0 when unavailable. forecast: seconds ahead (<=60).
  float headroom(int forecastSec) {
    load();
    if (!thermal || !thHead) return -1.f;
    const float h = thHead(thermal, forecastSec);
    return h != h ? -1.f : h;
  }
  int status() {
    load();
    return thermal && thStatus ? thStatus(thermal) : -1;
  }
};
inline Adpf& adpf() {
  static Adpf* a = new Adpf;
  return *a;
}

#if defined(__APPLE__)
// Starts LibRaw's OpenMP team once (omp_apple.cpp: team size = performance cores, workers created at user-initiated QoS: a pthread inherits the QoS of the thread
// that creates it, so a team first spawned from a utility prefetch would stay at utility). No-op without libomp.
void appleOmpInit();
// The open the user is waiting for runs at user-initiated QoS even when the calling (Dart worker) thread is at the default one; prefetch threads that called
// setBackground(true) keep utility.
struct OpenQos {
  qos_class_t old_ = QOS_CLASS_UNSPECIFIED;
  int rel_ = 0;
  bool raised_ = false;
  OpenQos() {
    if (g_class == 2) return;
    ::pthread_get_qos_class_np(::pthread_self(), &old_, &rel_);
    if (old_ != QOS_CLASS_USER_INTERACTIVE && old_ != QOS_CLASS_USER_INITIATED && ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0) raised_ = true;
  }
  ~OpenQos() {
    if (raised_) ::pthread_set_qos_class_self_np(old_ == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : old_, rel_);
  }
};
#endif

}  // namespace dapple::sys
