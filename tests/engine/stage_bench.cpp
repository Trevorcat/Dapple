// The editor's decode stages through the C API, in the order and with the arguments the app uses (wall ms):
//   dapple_stage_bench <raw file> <out.jpg> [params json]
//   1. open     dapple_session_open(PREVIEW, 2048)             tap -> picture
//   2. detail   dapple_session_open(PREVIEW, 4096)             2x double-tap (half-size session)
//   3. region   dapple_session_open_region: a tile of the middle (first 1:1), a pan (second tile), 256 x 256 (the idle warm-up)
//   4. export   dapple_session_open(FULL, 0) + dapple_session_export_jpeg (original size, quality 92)
// Env: DAPPLE_GPU=1 for the GPU executor; DAPPLE_BENCH_PROBE=<dir> runs the app's start-up probe first; DAPPLE_BENCH_ORDER=warmup runs the 256 x 256
// warm-up region before the first 1:1 (what the app does after the picture settles). The engine's own stage lines (tag dapple_engine / dapple_demosaic) go
// to logcat on Android and to stderr elsewhere.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "dapple/dapple_c.h"

namespace {
using Clk = std::chrono::steady_clock;
double msSince(Clk::time_point t) { return std::chrono::duration<double, std::milli>(Clk::now() - t).count(); }
int intField(const char* json, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  const char* p = json ? std::strstr(json, k.c_str()) : nullptr;
  if (!p) return 0;
  p = std::strchr(p, ':');
  return p ? std::atoi(p + 1) : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: %s raw out.jpg [params]\n", argv[0]), 2;
  const char* params = argc > 3 ? argv[3] : "{}";
  if (const char* pdir = std::getenv("DAPPLE_BENCH_PROBE")) {
    dapple_gpu_configure(pdir, "bench-1", "bench-fp");
    dapple_gpu_warmup();
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
  }
  auto t = Clk::now();
  DappleSession* s = dapple_session_open(argv[1], DAPPLE_QUALITY_PREVIEW, 2048);
  if (!s) return std::fprintf(stderr, "open: %s\n", dapple_last_error()), 1;
  std::printf("open (preview, 2048)        %8.0f ms\n", msSince(t));
  char* info = dapple_session_info_json(s);
  const int W = intField(info, "width"), H = intField(info, "height");
  dapple_free_string(info);
  dapple_session_close(s);
  std::printf("  picture %d x %d\n", W, H);

  t = Clk::now();
  s = dapple_session_open(argv[1], DAPPLE_QUALITY_PREVIEW, 4096);
  if (!s) return std::fprintf(stderr, "detail: %s\n", dapple_last_error()), 1;
  std::printf("detail (preview, 4096)      %8.0f ms\n", msSince(t));
  dapple_session_close(s);

  auto region = [&](const char* name, int x0, int y0, int w, int h) {
    const auto t0 = Clk::now();
    DappleSession* r = dapple_session_open_region(argv[1], x0, y0, x0 + w, y0 + h);
    if (!r) {
      std::printf("%-27s refused: %s\n", name, dapple_last_error());
      return;
    }
    std::printf("%-27s %8.0f ms\n", name, msSince(t0));
    dapple_session_close(r);
  };
  std::thread finger;
  if (const char* e = std::getenv("DAPPLE_BENCH_INTERACT_MS")) {  // a finger is down for N ms from the start of the first region: its tile loops must wait
    const int ms = std::atoi(e);
    dapple_set_interaction(1);
    finger = std::thread([ms] {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      dapple_set_interaction(0);
    });
  }
  const bool warmFirst = std::getenv("DAPPLE_BENCH_ORDER") && std::strcmp(std::getenv("DAPPLE_BENCH_ORDER"), "warmup") == 0;
  if (warmFirst) region("warm-up region 256 x 256", W / 2 - 128, H / 2 - 128, 256, 256);
  if (W >= 3400 && H >= 3400) {
    region("first 1:1 region 1604 x 3382", W * 0.4, H * 0.12, 1604, 3382);
    region("pan, second region", W * 0.58, H * 0.12, 1604, 3382);
  } else {
    region("first 1:1 region", W / 4, H / 4, W / 2, H / 2);
  }

  if (finger.joinable()) finger.join();
  t = Clk::now();
  s = dapple_session_open(argv[1], DAPPLE_QUALITY_FULL, 0);
  if (!s) return std::fprintf(stderr, "full: %s\n", dapple_last_error()), 1;
  const double openMs = msSince(t);
  const auto t1 = Clk::now();
  const int rc = dapple_session_export_jpeg(s, params, nullptr, 0, 92, argv[2]);
  std::printf("export: full open %6.0f ms, render + encode %6.0f ms (rc %d)\n", openMs, msSince(t1), rc);
  dapple_session_close(s);
  return rc == 0 ? 0 : 1;
}
