#pragma once
// Thread placement of inference, consistent with core/src/sysperf.h: the pool lives on the performance cores (poolCpus), the calling thread
// joins them for the duration of a run (XNNPACK / ORT use the caller as one of the workers). Worker threads of a backend inherit the
// affinity of the thread that creates them, so sessions are created inside a ScopedAffinity.
#include <algorithm>
#include <vector>

#include "sysperf.h"

namespace dapple::ai {

// Number of threads used when RunOptions::threads == 0. Measured on a Snapdragon 7s Gen 2 class phone (4 x A78 + 4 x A55): 2 threads on the
// big cores were the fastest for every model (4 threads were up to 1.8x slower for ONNX Runtime and not faster for LiteRT), and 2 leaves
// two big cores to the UI / render threads.
inline int defaultThreads() {
  const auto cpus = sys::poolCpus(sys::topology());
  return std::max(1, std::min<int>(2, int(cpus.size())));
}

inline std::vector<int> inferenceCpus() {
  const auto& t = sys::topology();
  if (!t.hetero()) return {};  // homogeneous / unknown: no pinning (same convention as sysperf)
  return sys::poolCpus(t);
}

class ScopedAffinity {
 public:
  explicit ScopedAffinity(const std::vector<int>& cpus) {
#ifdef DAPPLE_SYSPERF_LINUX
    if (cpus.empty()) return;
    saved_ = new cpu_set_t;
    if (::sched_getaffinity(0, sizeof(cpu_set_t), static_cast<cpu_set_t*>(saved_)) != 0) { delete static_cast<cpu_set_t*>(saved_); saved_ = nullptr; return; }
    sys::setAffinity(cpus);
#else
    (void)cpus;
#endif
  }
  ~ScopedAffinity() {
#ifdef DAPPLE_SYSPERF_LINUX
    if (saved_) {
      ::sched_setaffinity(0, sizeof(cpu_set_t), static_cast<cpu_set_t*>(saved_));
      delete static_cast<cpu_set_t*>(saved_);
    }
#endif
  }
  ScopedAffinity(const ScopedAffinity&) = delete;
  ScopedAffinity& operator=(const ScopedAffinity&) = delete;

 private:
  void* saved_ = nullptr;
};

}  // namespace dapple::ai
