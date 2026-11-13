// Apple only: LibRaw's OpenMP team (static libomp, scripts/ios/build-libomp.sh). Everything else compiles to nothing.
#if defined(__APPLE__)
#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "sysperf.h"
#if defined(DAPPLE_LIBOMP)
#include <omp.h>
#endif

namespace dapple::sys {
void appleOmpInit() {
#if defined(DAPPLE_LIBOMP)
  static std::once_flag once;
  std::call_once(once, [] {
    OpenQos qos;  // the workers inherit this thread's QoS
    const int n = applePerfCores();
    const char* env = std::getenv("OMP_NUM_THREADS");  // an explicit setting (tests, comparisons) wins
    const int team = env && *env ? std::max(1, std::atoi(env)) : n;
    omp_set_num_threads(team);
    omp_set_dynamic(0);
    kmp_set_blocktime(0);  // idle workers sleep at once: no spinning against the other sessions' threads
#pragma omp parallel num_threads(team)
    {
      volatile int spawn = 0;  // forces the team into existence now
      (void)spawn;
    }
  });
#endif
}
}  // namespace dapple::sys
#endif
