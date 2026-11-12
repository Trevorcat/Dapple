#pragma once
// While a GPU frame is in flight the calling thread sleeps in the fence wait and wakes on whatever core the scheduler picks:
// usually a little core at a low clock, where reading the mapped readback buffer took 49 ms instead of 7-13 ms (Pixel 5,
// 1.7 MP). On Android the thread is pinned to the fastest cores for the duration of a run and restored afterwards.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <sched.h>
namespace dapple::engine::gpu::detail {
inline cpu_set_t bigCoreSet(bool& ok) {
  static cpu_set_t set;
  static bool have = false, valid = false;
  if (!have) {
    have = true;
    CPU_ZERO(&set);
    long maxf[64] = {0}, top = 0;
    for (int c = 0; c < 64; ++c) {
      char path[96];
      std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
      if (FILE* f = std::fopen(path, "r")) {
        if (std::fscanf(f, "%ld", &maxf[c]) != 1) maxf[c] = 0;
        std::fclose(f);
        top = std::max(top, maxf[c]);
      }
    }
    int n = 0;
    for (int c = 0; c < 64; ++c)
      if (maxf[c] > 0 && maxf[c] * 10 >= top * 8) {
        CPU_SET(c, &set);
        ++n;
      }
    valid = n > 0;
  }
  ok = valid;
  return set;
}
struct BigCoreScope {
  cpu_set_t prev;
  bool active = false;
  BigCoreScope() {
    if (std::getenv("DAPPLE_GPU_NO_PIN")) return;
    bool ok = false;
    const cpu_set_t big = bigCoreSet(ok);
    if (!ok || sched_getaffinity(0, sizeof prev, &prev) != 0) return;
    cpu_set_t both;
    CPU_AND(&both, &prev, &big);
    if (CPU_COUNT(&both) == 0) return;
    active = sched_setaffinity(0, sizeof both, &both) == 0;
  }
  ~BigCoreScope() {
    if (active) sched_setaffinity(0, sizeof prev, &prev);
  }
};
}  // namespace dapple::engine::gpu::detail
#else
namespace dapple::engine::gpu::detail {
struct BigCoreScope {};
}  // namespace dapple::engine::gpu::detail
#endif
