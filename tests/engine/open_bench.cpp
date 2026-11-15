// Tap-to-first-frame and full-size export through the C API, in the order the app uses (wall ms, one line per step):
//   dapple_open_bench <raw file> open            dapple_session_open(PREVIEW, 2048) + the first frame (render_frame, 1600 px, tier full)
//   dapple_open_bench <raw file> export out.jpg  the editor session as above, then dapple_export_intent + dapple_session_open(FULL, 0) +
//                                                dapple_session_export_jpeg (original size, quality 92), peak RSS of the process at the end
// Env: DAPPLE_BENCH_PROBE=<dir> configures the GPU (verdict cache in <dir>) and warms it up first, as the app does at start-up; DAPPLE_GPU=1 for the GPU
// executor; DAPPLE_BENCH_PARAMS=<json> (default "{}"); DAPPLE_BENCH_DUMP=<file> writes the first frame (width, height, RGBA bytes). The engine's stage lines go to logcat on Android and to stderr elsewhere.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "dapple/dapple_c.h"

namespace {
using Clk = std::chrono::steady_clock;
double msSince(Clk::time_point t) { return std::chrono::duration<double, std::milli>(Clk::now() - t).count(); }
long statusKb(const char* key) {  // Linux / Android: VmHWM / VmRSS of this process, -1 elsewhere
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind(key, 0) == 0) return std::atol(line.c_str() + std::strlen(key) + 1);
  return -1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: %s raw open|export [out.jpg]\n", argv[0]), 2;
  const bool exportMode = std::strcmp(argv[2], "export") == 0;
  if (exportMode && argc < 4) return std::fprintf(stderr, "export needs out.jpg\n"), 2;
  const char* params = std::getenv("DAPPLE_BENCH_PARAMS") ? std::getenv("DAPPLE_BENCH_PARAMS") : "{}";
  if (const char* pdir = std::getenv("DAPPLE_BENCH_PROBE")) {
    dapple_gpu_configure(pdir, "bench-1", "bench-fp");
    dapple_gpu_warmup();
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
  }
  if (const char* prime = std::getenv("DAPPLE_BENCH_PRIME")) {  // another photo opened and rendered first: the app's state after the first photo (pipelines built)
    if (DappleSession* p = dapple_session_open(prime, DAPPLE_QUALITY_PREVIEW, 2048)) {
      DappleFrameRequest pq{};
      pq.size = sizeof pq;
      pq.long_edge = 1600;
      DappleImage pi{};
      if (dapple_session_render_frame(p, params, nullptr, &pq, &pi, nullptr) == DAPPLE_OK) dapple_free_image(&pi);
      dapple_session_close(p);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  }
  const auto t0 = Clk::now();
  DappleSession* s = dapple_session_open(argv[1], DAPPLE_QUALITY_PREVIEW, 2048);
  if (!s) return std::fprintf(stderr, "open: %s\n", dapple_last_error()), 1;
  const double openMs = msSince(t0);
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = 1600;
  DappleImage img{};
  const auto t1 = Clk::now();
  if (dapple_session_render_frame(s, params, nullptr, &rq, &img, nullptr) != DAPPLE_OK) return std::fprintf(stderr, "frame: %s\n", dapple_last_error()), 1;
  const double frameMs = msSince(t1);
  if (const char* dump = std::getenv("DAPPLE_BENCH_DUMP")) {  // the first frame's pixels (before / after comparisons)
    if (FILE* f = std::fopen(dump, "wb")) {
      std::fwrite(&img.width, sizeof img.width, 1, f), std::fwrite(&img.height, sizeof img.height, 1, f);
      if (img.rgba) std::fwrite(img.rgba, 1, std::size_t(img.width) * img.height * 4, f);
      std::fclose(f);
    }
  }
  dapple_free_image(&img);
  std::printf("open (preview, 2048) %7.0f ms, first frame %6.0f ms, tap to first frame %7.0f ms\n", openMs, frameMs, msSince(t0));
  if (!exportMode) {
    dapple_session_close(s);
    std::printf("peak RSS %ld kB\n", statusKb("VmHWM:"));
    return 0;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const long rssBefore = statusKb("VmRSS:");
  const auto t2 = Clk::now();
  dapple_export_intent(params);
  DappleSession* f = dapple_session_open(argv[1], DAPPLE_QUALITY_FULL, 0);
  if (!f) return std::fprintf(stderr, "full: %s\n", dapple_last_error()), 1;
  const double fullMs = msSince(t2);
  const auto t3 = Clk::now();
  const int rc = dapple_session_export_jpeg(f, params, nullptr, 0, 92, argv[3]);
  const double expMs = msSince(t3);
  dapple_session_close(f);
  dapple_session_close(s);
  std::printf("export: full open %6.0f ms, render + encode %6.0f ms, total %6.0f ms (rc %d)\n", fullMs, expMs, msSince(t2), rc);
  std::printf("RSS before export %ld kB, peak RSS %ld kB\n", rssBefore, statusKb("VmHWM:"));
  return rc == 0 ? 0 : 1;
}
