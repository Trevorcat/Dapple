// Concurrency stress for the RAW decode path through the C API: several threads open / close sessions of RAW and DNG files at the same time (preview opens, which
// read the embedded preview for the exposure anchor on a std::async thread while the opening thread unpacks; full opens; region opens), the way the editor, its
// prefetch and an export overlap. LibRaw may run OpenMP regions (static libomp on Apple, the NDK's on Android) and its memory manager takes an OpenMP critical
// section on every allocation: this must neither crash nor change pixels.
//   dapple_omp_stress_tests [seconds] [threads] [files...]    default 20 s, 6 threads, the public samples under build/rawsamples that exist
// Exit 77 when no sample is present.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "dapple/dapple_c.h"

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
  const int nThreads = argc > 2 ? std::atoi(argv[2]) : 6;
  std::vector<std::string> files;
  for (int i = 3; i < argc; ++i)
    if (std::filesystem::exists(argv[i])) files.push_back(argv[i]);
  if (files.empty())
    for (const char* f : {"build/rawsamples/Apple/iPhone-XS/IMG_1105.dng", "build/rawsamples/Fujifilm/X-S10/Fujifilm-X-S10-compressed.RAF", "build/rawsamples/Sony/ILCE-7RM4/DSC00396.ARW",
                          "build/rawsamples/Apple/iPhone-12-Pro/IMG_1361.DNG", "build/rawsamples/Fujifilm/X-T50/DSCF0119.RAF"})
      if (std::filesystem::exists(f)) files.push_back(f);
  if (files.empty()) return std::printf("no sample files\n"), 77;
  std::atomic<long> opens{0}, fails{0};
  std::atomic<bool> stop{false};
  const auto t0 = std::chrono::steady_clock::now();
  auto worker = [&](int id) {
    unsigned n = unsigned(id) * 7919u;
    while (!stop.load()) {
      const std::string& f = files[(n + unsigned(id)) % files.size()];
      const bool small = f.size() > 4 && (f.compare(f.size() - 4, 4, ".dng") == 0 || f.compare(f.size() - 4, 4, ".DNG") == 0);  // full opens only of the 12 MP files
      unsigned kind = (n / 3 + unsigned(id)) % 7;
      if (kind == 2 && !small) kind = 0;
      if (kind > 4) kind = 4;
      DappleSession* s = nullptr;
      if (kind == 0 || kind == 1) s = dapple_session_open(f.c_str(), DAPPLE_QUALITY_PREVIEW, kind == 0 ? 2048 : 4096);
      else if (kind == 2) s = dapple_session_open(f.c_str(), DAPPLE_QUALITY_FULL, 0);
      else if (kind == 3) s = dapple_session_open_region(f.c_str(), 100, 100, 1300, 900);
      else {  // open and close at once: teardown while the other threads are inside LibRaw
        s = dapple_session_open(f.c_str(), DAPPLE_QUALITY_PREVIEW, 1024);
        if (s) dapple_session_close(s);
        s = nullptr;
        ++opens;
        ++n;
        continue;
      }
      if (s) dapple_session_close(s);
      else if (kind != 3) {  // region opens may legitimately fail (superseded by a newer region request, DNG lens warp needs the whole frame)
        ++fails;
        std::printf("open failed (kind %u) %s: %s\n", kind, f.c_str(), dapple_last_error());
      }
      ++opens;
      ++n;
    }
  };
  std::vector<std::thread> th;
  for (int i = 0; i < nThreads; ++i) th.emplace_back(worker, i);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop = true;
  for (auto& t : th) t.join();
  std::printf("omp_stress: %d threads, %ld opens in %.0f s over %zu files, %ld failed opens\n", nThreads, opens.load(),
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), files.size(), fails.load());
  return fails.load() == 0 ? 0 : 1;
}
