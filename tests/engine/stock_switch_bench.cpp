// Time from choosing a never-seen film stock to its exact frame, through the C API (docs/engine/pipeline-architecture.md section 39):
//   dapple_stock_switch_bench <raw> <pack.dfp> <cache dir> <stocks A csv> <stocks B csv> [long_edge 1024]
// Fresh process, `cache dir` = the app's cache (empty = a cold install). Phase 1: the stocks of list A on a new photo with nothing prepared (no prebake yet).
// Phase 2: the first-launch prebake (dapple_film_prebake_start) runs to the end, then list B (other stocks, also never seen) is chosen. Each stock: the first final frame
// (match, tables, render) and a second identical frame (what is left when nothing is new). DAPPLE_FILM_CALIB_BAKED=1 = the table-bake calibration of before.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dapple/dapple_c.h"

namespace {
using Clk = std::chrono::steady_clock;
double msSince(Clk::time_point t) { return std::chrono::duration<double, std::milli>(Clk::now() - t).count(); }
std::vector<std::string> split(const char* s) {
  std::vector<std::string> r;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');) r.push_back(t);
  return r;
}
double frame(DappleSession* s, const std::string& id, int le) {
  const std::string pj = "{\"film\":\"" + id + "\"}";
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = le;
  DappleImage img{0, 0, nullptr};
  const auto t = Clk::now();
  const int rc = dapple_session_render_frame(s, pj.c_str(), nullptr, &rq, &img, nullptr);
  const double ms = msSince(t);
  if (rc != DAPPLE_OK) std::fprintf(stderr, "render %s: %s\n", id.c_str(), dapple_last_error());
  dapple_free_image(&img);
  return ms;
}
void phase(DappleSession* s, const char* label, const std::vector<std::string>& ids, int le) {
  for (const auto& id : ids) {
    const double first = frame(s, id, le), second = frame(s, id, le);
    std::printf("%-9s %-24s first %8.0f ms   second %6.0f ms   first-use cost %8.0f ms\n", label, id.c_str(), first, second, first - second);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) return std::fprintf(stderr, "usage: %s raw pack cachedir listA listB [long_edge]\n", argv[0]), 2;
  const int le = argc > 6 ? std::atoi(argv[6]) : 1024;
  dapple_gpu_configure(argv[3], "stock-bench", "stock-bench-fp");
  if (dapple_film_load_pack(argv[2]) != DAPPLE_OK) return std::fprintf(stderr, "pack: %s\n", dapple_last_error()), 1;
  DappleSession* s = dapple_session_open(argv[1], DAPPLE_QUALITY_PREVIEW, 2048);
  if (!s) return std::fprintf(stderr, "open: %s\n", dapple_last_error()), 1;
  frame(s, "", le);  // the picture itself (no film): decode-side warm-up, not part of the measurement
  phase(s, "no-prebake", split(argv[4]), le);
  const auto t = Clk::now();
  dapple_film_prebake_start();
  for (;;) {
    char* st = dapple_film_prebake_status();
    const bool fin = st && std::strstr(st, "\"finished\":true");
    if (fin) std::printf("prebake status: %s\n", st);
    dapple_free_string(st);
    if (fin || msSince(t) > 900000) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::printf("prebake of all stocks: %.0f ms\n", msSince(t));
  phase(s, "prebaked", split(argv[5]), le);
  dapple_session_close(s);
  return 0;
}
