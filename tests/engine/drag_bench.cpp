// Slider-drag timing through the C API (what the app does): dapple_session_render_frame with INTERACTIVE frames, one parameter moving.
//   dapple_drag_bench <image> <film pack .dfp> [long edge=360] [frames=14] [case substring]
// Env: DAPPLE_GPU=1 (GPU executor), DAPPLE_ENGINE_LOG=1. Prints per case: first frame, median and max of the following frames (wall ms).
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dapple/dapple_c.h"

namespace {
int settleMsEnv() { return std::getenv("DAPPLE_BENCH_SETTLE_MS") ? std::atoi(std::getenv("DAPPLE_BENCH_SETTLE_MS")) : 0; }
struct Case {
  const char* name;
  const char* key;
  float lo, hi;
  const char* extra;  // params JSON fragment present in every frame
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: %s image pack.dfp [long_edge] [frames] [filter]\n", argv[0]), 2;
  const int le = argc > 3 ? std::atoi(argv[3]) : 360;
  const int frames = argc > 4 ? std::atoi(argv[4]) : 14;
  const std::string filter = argc > 5 ? argv[5] : "";
  if (const char* ap = std::getenv("DAPPLE_BENCH_ADPF")) {  // wave 8: Android Performance Hint target in ms, like the app's configurePerf
    dapple_perf_configure(0, std::atoi(ap));
    if (char* j = dapple_perf_info_json()) {
      std::fprintf(stderr, "perfinfo %s\n", j);
      dapple_free_string(j);
    }
  }
  if (dapple_film_load_pack(argv[2]) != DAPPLE_OK) return std::fprintf(stderr, "pack: %s\n", dapple_last_error()), 1;
  if (const char* pdir = std::getenv("DAPPLE_BENCH_PROBE")) {  // the app's start-up: configure + background probe (+ warm-up), then wait
    dapple_gpu_configure(pdir, "bench-1", "bench-fp");
    dapple_gpu_warmup();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::getenv("DAPPLE_BENCH_PROBE_WAIT_MS") ? std::atoi(std::getenv("DAPPLE_BENCH_PROBE_WAIT_MS")) : 4000));
  }
  DappleSession* s = dapple_session_open(argv[1], 0, 2048);
  if (!s) return std::fprintf(stderr, "open: %s\n", dapple_last_error()), 1;
  const std::vector<Case> cases = {
      {"still", "film_strength", 100, 100, ""},
      {"film_strength", "film_strength", 100, 60, ""},
      {"film_grain", "film_grain", 20, 80, ""},
      {"film_shoot", "film_shoot", -1, 1, ""},
      {"film_print", "film_print", -1, 1, ""},
      {"film_black", "film_black", -1, 1, ""},
      {"film_latitude", "film_latitude", -1, 1, ""},
      {"film_wb", "film_wb", 0, 1, ""},
      {"film_warmth", "film_warmth", -30, 30, ""},
      {"film_tint", "film_tint", -30, 30, ""},
      {"film_exp", "film_exp", -1, 1, ""},
      {"exposure (film on)", "exposure", -1, 1, ""},
  };
  using Clk = std::chrono::steady_clock;
  if (filter == "black") {  // visible range of film_black on this photo: mean / max change against film_black 0 (exact tables, final tier)
    auto render = [&](float v, DappleImage* out) {
      char js[200];
      std::snprintf(js, sizeof js, "{\"film\":\"kodak_portra_400\",\"film_black\":%.3f}", double(v));
      DappleFrameRequest rq{};
      rq.size = sizeof rq;
      rq.long_edge = le;
      return dapple_session_render_frame(s, js, nullptr, &rq, out, nullptr) == DAPPLE_OK;
    };
    DappleImage ref{};
    if (!render(0.f, &ref)) return 1;
    const size_t n = size_t(ref.width) * ref.height;
    double dark = 0;  // share of pixels below 20/255 (the part black can move)
    for (size_t i = 0; i < n; ++i) dark += ref.rgba[4 * i + 1] < 20;
    std::printf("  film_black against 0 (%dx%d, %.1f %% of the pixels below 20/255):\n", ref.width, ref.height, 100.0 * dark / double(n));
    for (float v : {-1.f, -0.34f, 0.34f, 1.f}) {
      DappleImage b{};
      if (!render(v, &b)) return 1;
      double sum = 0, sumDark = 0;
      int mx = 0;
      for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
          const int d = std::abs(int(b.rgba[4 * i + c]) - int(ref.rgba[4 * i + c]));
          sum += d, mx = std::max(mx, d);
          if (ref.rgba[4 * i + 1] < 20) sumDark += d;
        }
      std::printf("    film_black %+.2f: mean |d| %.2f /255 (dark pixels %.2f), max %d\n", double(v), sum / double(n * 3), dark > 0 ? sumDark / (dark * 3) : 0.0, mx);
      dapple_free_image(&b);
    }
    dapple_free_image(&ref);
    return 0;
  }
  if (filter == "converge") {  // a drag that stops on a value converges to the table of that value without further input (wave 8, async print tables)
    static std::atomic<std::int64_t> fired{0};
    static std::atomic<int> count{0};
    dapple_set_repaint_callback(
        [](void*) {
          fired.store(std::chrono::duration_cast<std::chrono::microseconds>(Clk::now().time_since_epoch()).count());
          count.fetch_add(1);
        },
        nullptr);
    auto frame = [&](float v, DappleImage* out) {
      char js[200];
      std::snprintf(js, sizeof js, "{\"film\":\"kodak_portra_400\",\"film_black\":%.4f}", double(v));
      DappleFrameRequest rq{};
      rq.size = sizeof rq;
      rq.long_edge = le;
      rq.flags = DAPPLE_FRAME_INTERACTIVE;
      rq.active_param = "film_black";
      return dapple_session_render_frame(s, js, nullptr, &rq, out, nullptr) == DAPPLE_OK;
    };
    auto diff = [](const DappleImage& a, const DappleImage& b) {
      double sum = 0;
      const size_t n = size_t(a.width) * a.height * 4;
      for (size_t i = 0; i < n; ++i) sum += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
      return sum / double(n);
    };
    DappleImage warm{};
    if (!frame(0.f, &warm)) return 1;
    dapple_free_image(&warm);
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    for (int run = 0; run < 3; ++run) {
      const float target = 0.25f + 0.2f * float(run);
      DappleImage last{};
      auto tStop = Clk::now();
      int c0 = count.load();
      for (int i = 0; i <= 10; ++i) {
        dapple_free_image(&last);
        tStop = Clk::now();  // the landing is timed from the start of the last drag frame (it can land while that frame is still rendering)
        c0 = count.load();
        if (!frame(target * float(i) / 10.f, &last)) return 1;
      }
      while (count.load() == c0 && std::chrono::duration<double>(Clk::now() - tStop).count() < 3.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const double landMs = count.load() > c0 ? double(fired.load() - std::chrono::duration_cast<std::chrono::microseconds>(tStop.time_since_epoch()).count()) / 1000.0 : -1.0;
      DappleImage a{}, b{};
      if (!frame(target, &a)) return 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (!frame(target, &b)) return 1;
      std::printf("  run %d: stopped on %.2f, table landed %.1f ms after the start of the last drag frame (-1 = never); last drag frame vs converged: mean |d| %.3f /255; converged vs 500 ms later: %.3f (0 = already exact)\n",
                  run, double(target), landMs, diff(last, a), diff(a, b));
      dapple_free_image(&last);
      dapple_free_image(&a);
      dapple_free_image(&b);
    }
    return 0;
  }
  if (filter == "stockslide") {  // the film map selects stocks live while the finger slides (wave 8): 10 stocks in 3 s, frame times and stock tap -> first frame
    const char* ids[] = {"kodak_portra_400", "kodak_ektar_100", "fujifilm_velvia_100", "kodak_vision3_500t", "kodak_portra_160",
                         "kodak_portra_800", "fujifilm_pro_400h", "kodak_gold_200", "kodak_ultramax_400", "fujifilm_provia_100f"};
    auto frame = [&](const char* stock, double* ms) {
      char js[200];
      std::snprintf(js, sizeof js, "{\"film\":\"%s\"}", stock);
      DappleFrameRequest rq{};
      rq.size = sizeof rq;
      rq.long_edge = le;
      rq.flags = DAPPLE_FRAME_INTERACTIVE;
      rq.active_param = "film";
      DappleImage img{};
      const auto t0 = Clk::now();
      const bool ok = dapple_session_render_frame(s, js, nullptr, &rq, &img, nullptr) == DAPPLE_OK;
      *ms = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
      dapple_free_image(&img);
      return ok;
    };
    static std::atomic<std::int64_t> landedUs{0};
    dapple_set_repaint_callback([](void*) { landedUs.store(std::chrono::duration_cast<std::chrono::microseconds>(Clk::now().time_since_epoch()).count()); }, nullptr);
    double ms = 0;
    if (!frame(ids[0], &ms)) return 1;  // the photo is open on the default stock
    std::this_thread::sleep_for(std::chrono::milliseconds(settleMsEnv()));
    for (int pass = 0; pass < 2; ++pass) {  // pass 0: every stock for the first time in this process; pass 1: warm in process
      std::vector<double> all, firsts, shownAfter;
      const auto tStart = Clk::now();
      for (int i = 1; i <= 10; ++i) {
        const char* st = ids[i % 10];
        const auto tSel = Clk::now();
        const std::int64_t selUs = std::chrono::duration_cast<std::chrono::microseconds>(tSel.time_since_epoch()).count();
        landedUs.store(0);
        double first = 0;
        if (!frame(st, &first)) return 1;
        firsts.push_back(first);
        all.push_back(first);
        // frames until the next selection 300 ms later (3 s for 10 stocks)
        while (std::chrono::duration<double, std::milli>(Clk::now() - tSel).count() < 300.0) {
          double m = 0;
          if (!frame(st, &m)) return 1;
          all.push_back(m);
        }
        const std::int64_t lu = landedUs.load();
        shownAfter.push_back(lu > selUs ? double(lu - selUs) / 1000.0 : 0.0);  // 0: nothing was stale (cached)
      }
      std::vector<double> s2 = all;
      std::sort(s2.begin(), s2.end());
      int over = 0;
      for (double v : all) over += v > 33.0;
      double fmax = 0, fsum = 0;
      for (double v : firsts) fmax = std::max(fmax, v), fsum += v;
      std::printf("  pass %d (%s): 10 stocks in %.1f s, %zu frames, p50 %.1f p95 %.1f max %.1f ms, over 33 ms: %d; stock tap -> first frame: mean %.1f ms, max %.1f ms; correct stock on screen after: mean %.0f ms, max %.0f ms (0 = was cached)\n", pass,
                  pass == 0 ? "cold in process" : "warm in process", std::chrono::duration<double>(Clk::now() - tStart).count(), all.size(), s2[s2.size() / 2],
                  s2[std::size_t(double(s2.size()) * 0.95)], s2.back(), over, fsum / double(firsts.size()), fmax, std::accumulate(shownAfter.begin(), shownAfter.end(), 0.0) / double(shownAfter.size()), *std::max_element(shownAfter.begin(), shownAfter.end()));
    }
    return 0;
  }
  if (filter == "compare") {  // coarse (interactive drag) tables against the exact ones, same parameters
    const char* sets[] = {"{\"film\":\"kodak_portra_400\",\"film_black\":0.37}", "{\"film\":\"kodak_ektar_100\",\"film_warmth\":23}",
                          "{\"film\":\"fujifilm_pro_400h\",\"film_print\":0.4}", "{\"film\":\"kodak_portra_800\",\"film_wb\":0.7,\"film_tint\":-17}",
                          "{\"film\":\"kodak_vision3_500t\",\"film_black\":-0.6}"};
    for (const char* js : sets) {
      DappleImage a{}, b{};
      for (int pass = 0; pass < 2; ++pass) {
        DappleFrameRequest rq{};
        rq.size = sizeof rq;
        rq.long_edge = le;
        rq.flags = DAPPLE_FRAME_INTERACTIVE;
        rq.active_param = pass == 0 ? "film_black" : "film_strength";
        if (dapple_session_render_frame(s, js, nullptr, &rq, pass == 0 ? &a : &b, nullptr) != DAPPLE_OK) return 1;
      }
      double sum = 0;
      int mx = 0;
      const size_t n = size_t(a.width) * a.height;
      for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
          const int d = std::abs(int(a.rgba[4 * i + c]) - int(b.rgba[4 * i + c]));
          sum += d, mx = std::max(mx, d);
        }
      std::printf("  coarse vs exact %-60s mean |d| %.3f /255, max %d\n", js, sum / double(n * 3), mx);
      dapple_free_image(&a);
      dapple_free_image(&b);
    }
    return 0;
  }
  const int settleMs = std::getenv("DAPPLE_BENCH_SETTLE_MS") ? std::atoi(std::getenv("DAPPLE_BENCH_SETTLE_MS")) : 0;  // lets the background calibration finish
  std::printf("== drag bench: %s, long edge %d, %d frames/case, GPU env %s ==\n", argv[1], le, frames, std::getenv("DAPPLE_GPU") ? std::getenv("DAPPLE_GPU") : "-");
  std::printf("  %-20s %9s %9s %9s %9s\n", "case", "first", "median", "max", "min");
  for (const Case& c : cases) {
    if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
    if (settleMs > 0 && std::string(c.name) == "still") {
      DappleFrameRequest rq{};
      rq.size = sizeof rq;
      rq.long_edge = le;
      rq.flags = DAPPLE_FRAME_INTERACTIVE;
      DappleImage img{};
      dapple_session_render_frame(s, "{\"film\":\"kodak_portra_400\"}", nullptr, &rq, &img, nullptr);
      dapple_free_image(&img);
      std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    }
    const double periodMs = std::getenv("DAPPLE_BENCH_PERIOD_MS") ? std::atof(std::getenv("DAPPLE_BENCH_PERIOD_MS")) : 0.0;  // vsync-paced frames (wave 8): one frame start per period
    auto nextStart = Clk::now();
    std::vector<double> t;
    for (int i = 0; i < frames; ++i) {
      if (periodMs > 0) {
        std::this_thread::sleep_until(nextStart);
        nextStart = std::max(nextStart + std::chrono::duration_cast<Clk::duration>(std::chrono::duration<double, std::milli>(periodMs)), Clk::now());
      }
      const float v = c.lo + (c.hi - c.lo) * float(i) / float(std::max(1, frames - 1));
      char json[512];
      std::snprintf(json, sizeof json, "{\"film\":\"kodak_portra_400\",\"%s\":%.4f}", c.key, double(v));
      DappleFrameRequest rq{};
      rq.size = sizeof rq;
      rq.long_edge = le;
      rq.flags = DAPPLE_FRAME_INTERACTIVE;
      rq.active_param = c.key;
      DappleImage img{};
      const auto t0 = Clk::now();
      if (dapple_session_render_frame(s, json, nullptr, &rq, &img, nullptr) != DAPPLE_OK) return std::fprintf(stderr, "frame: %s\n", dapple_last_error()), 1;
      t.push_back(std::chrono::duration<double, std::milli>(Clk::now() - t0).count());
      dapple_free_image(&img);
    }
    std::vector<double> r(t.begin() + 1, t.end());
    std::sort(r.begin(), r.end());
    std::printf("  %-20s %9.1f %9.1f %9.1f %9.1f\n", c.name, t[0], r[r.size() / 2], r.back(), r.front());
  }
  dapple_session_close(s);
  return 0;
}
