#pragma once
// Process memory probes for the export memory budget (docs/engine/pipeline-architecture.md section 42, streaming export).
//
//   footprint()  what the OS charges the process for now: phys_footprint on Apple (what jetsam compares with its limit), VmRSS on
//                Linux / Android, PrivateUsage (commit) on Windows. Bytes, 0 = unknown.
//   available()  memory the process may still allocate before the OS steps in: os_proc_available_memory() on iOS (the jetsam
//                headroom of THIS process), MemAvailable of /proc/meminfo on Linux / Android, available physical memory on Windows
//                and macOS. Bytes, 0 = unknown.
//   Sampler      a background thread sampling footprint() every millisecond while it lives, with a peak per labelled stage
//                (stage("render") ...). Used by the export stage log and the memory tests; costs one thread, no allocation in the loop.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#else
#include <unistd.h>
#endif

namespace dapple::mem {

#if !defined(_WIN32) && !defined(__APPLE__)
// kB value of a "Key:   123 kB" line of a /proc file, -1 when absent
inline long procKb(const char* file, const char* key) {
  std::FILE* f = std::fopen(file, "r");
  if (!f) return -1;
  char line[256];
  long v = -1;
  const std::size_t n = std::strlen(key);
  while (std::fgets(line, sizeof line, f))
    if (std::strncmp(line, key, n) == 0 && line[n] == ':') {
      v = std::strtol(line + n + 1, nullptr, 10);
      break;
    }
  std::fclose(f);
  return v;
}
#endif

inline std::size_t footprint() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return std::size_t(pmc.PrivateUsage);
  return 0;
#elif defined(__APPLE__)
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return std::size_t(info.phys_footprint);
  return 0;
#else
  // VmRSS reads a seq file: ~10 us. statm is cheaper (pages): resident * page size
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long size = 0, resident = 0;
  const int got = std::fscanf(f, "%lu %lu", &size, &resident);
  std::fclose(f);
  return got == 2 ? std::size_t(resident) * std::size_t(sysconf(_SC_PAGESIZE)) : 0;
#endif
}

// Resident set (working set on Windows): also reported, the footprint is the number that matters on phones.
inline std::size_t resident() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return std::size_t(pmc.WorkingSetSize);
  return 0;
#elif defined(__APPLE__)
  return footprint();
#else
  return footprint();
#endif
}

inline std::size_t available() {
#if defined(_WIN32)
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) return std::size_t(ms.ullAvailPhys);
  return 0;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
  if (__builtin_available(iOS 13.0, *)) return std::size_t(os_proc_available_memory());
  return 0;
#else
  std::uint64_t mem = 0;
  std::size_t len = sizeof mem;
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) != 0) return 0;
  return std::size_t(mem / 2);  // macOS has no per-process limit: half the machine, conservatively
#endif
#else
  const long kb = procKb("/proc/meminfo", "MemAvailable");
  return kb > 0 ? std::size_t(kb) * 1024u : 0;
#endif
}

// Samples footprint() every millisecond on its own thread while alive. stage() labels what follows; peaks are kept per label (in order of
// first use). Thread-safe; the sampling loop allocates nothing.
class Sampler {
 public:
  Sampler() : base_(footprint()) {
    peak_.store(base_);
    stagePeak_.store(base_);
    th_ = std::thread([this] {
      while (!stop_.load(std::memory_order_relaxed)) {
        sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~Sampler() { finish(); }
  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  void stage(const std::string& name) {
    sample();
    std::lock_guard<std::mutex> lk(mu_);
    closeStage();
    cur_ = name;
    stagePeak_.store(footprint());
  }
  void finish() {
    if (th_.joinable()) {
      stop_.store(true);
      th_.join();
      sample();
      std::lock_guard<std::mutex> lk(mu_);
      closeStage();
    }
  }
  std::size_t base() const { return base_; }
  std::size_t peak() const { return peak_.load(); }
  // (stage, peak bytes) in order
  std::vector<std::pair<std::string, std::size_t>> stages() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stages_;
  }
  // "decode 812 / render 1290 / encode 640" (MB, peaks per stage)
  std::string summary() const {
    std::string s;
    char b[96];
    for (const auto& kv : stages()) {
      std::snprintf(b, sizeof b, "%s%s %.0f", s.empty() ? "" : " / ", kv.first.c_str(), double(kv.second) / 1048576.0);
      s += b;
    }
    return s;
  }

 private:
  void sample() {
    const std::size_t v = footprint();
    std::size_t p = peak_.load(std::memory_order_relaxed);
    while (v > p && !peak_.compare_exchange_weak(p, v)) {
    }
    p = stagePeak_.load(std::memory_order_relaxed);
    while (v > p && !stagePeak_.compare_exchange_weak(p, v)) {
    }
  }
  void closeStage() {
    if (cur_.empty()) return;
    for (auto& kv : stages_)
      if (kv.first == cur_) {
        kv.second = std::max(kv.second, stagePeak_.load());
        return;
      }
    stages_.emplace_back(cur_, stagePeak_.load());
  }
  std::size_t base_;
  std::atomic<std::size_t> peak_{0}, stagePeak_{0};
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;
  std::string cur_;
  std::vector<std::pair<std::string, std::size_t>> stages_;
  std::thread th_;
};

// The sampler of the export in flight on this thread (null = none): engine stages label themselves through mark().
inline Sampler*& current() {
  static thread_local Sampler* s = nullptr;
  return s;
}
inline void mark(const char* stage) {
  if (Sampler* s = current()) s->stage(stage);
}

}  // namespace dapple::mem
