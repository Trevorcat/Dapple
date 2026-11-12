// Decode benchmark for the video core: dapple_video_bench <file> [startSec=10] [durSec=3] [maxEdge=1920] [maxFps=30] [hw=0|1] [fmt=p010|i420]
// Prints open time, single-still time (full size), thumbnail time, and the clip decode+convert time (decode only; no encode).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../core/video/dapple_video.h"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  double start = argc > 2 ? std::atof(argv[2]) : 10, dur = argc > 3 ? std::atof(argv[3]) : 3;
  int edge = argc > 4 ? std::atoi(argv[4]) : 1920;
  double fps = argc > 5 ? std::atof(argv[5]) : 30;
  int hw = argc > 6 ? std::atoi(argv[6]) : 0;
  int fmt = argc > 7 && !std::strcmp(argv[7], "i420") ? DV_I420 : DV_P010;
  DvInfo info{};
  char err[256];
  DvOpenOptions o{hw, DV_SRC_AUTO};
  auto t = Clock::now();
  DvVideo* v = dv_open(argv[1], &o, &info, err, sizeof err);
  if (!v) { std::printf("open failed: %s\n", err); return 1; }
  std::printf("open: %.0f ms  %s %dx%d %d-bit chroma=%d %.2f fps profile=%d decoder=%s kind=%d\n", ms(t), info.codec, info.width, info.height,
              info.bitDepth, info.chroma, info.fps, info.profile, info.decoder, info.colorKind);
  int w, h;
  std::vector<uint8_t> buf;
  for (int round = 0; round < 2; round++) {
    t = Clock::now();
    dv_begin(v, (int64_t)(start * 1e6), (int64_t)(start * 1e6), 0, 0, DV_RGBA, 0, &w, &h);
    buf.resize((size_t)dv_frame_bytes(v));
    dv_next(v, buf.data(), (int64_t)buf.size());
    std::printf("still %dx%d RGBA (full size): %.0f ms%s\n", w, h, ms(t), round ? " (warm)" : "");
    t = Clock::now();
    dv_begin(v, (int64_t)(start * 1e6), (int64_t)(start * 1e6), 200, 0, DV_RGBA, 1, &w, &h);
    buf.resize((size_t)dv_frame_bytes(v));
    dv_next(v, buf.data(), (int64_t)buf.size());
    std::printf("thumb %dx%d fast: %.0f ms\n", w, h, ms(t));
  }
  t = Clock::now();
  dv_begin(v, (int64_t)(start * 1e6), (int64_t)((start + dur) * 1e6), edge, fps, fmt, 0, &w, &h);
  buf.resize((size_t)dv_frame_bytes(v));
  int n = 0;
  while (dv_next(v, buf.data(), (int64_t)buf.size()) >= 0) n++;
  double tot = ms(t);
  char name[32];
  int isHw = dv_decoder(v, name, sizeof name);
  std::printf("clip %.1f s -> %dx%d %s, %d frames kept: %.0f ms (%.1f ms per kept frame), decoder %s hw=%d\n", dur, w, h, fmt == DV_P010 ? "P010" : "I420", n, tot, n ? tot / n : 0, name, isHw);
  dv_close(v);
  return 0;
}
