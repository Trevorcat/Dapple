// Keep-warm pump of the touch hint (gpu_runtime.h keepWarm, ctest "engine.gpu_keepwarm"): the pump never changes a picture, never submits while a real
// frame is pending, queues units behind real frames inside a window, and its window is bounded. Exit code 77 = no GPU device.
// The C API library carries its own copy of the GPU runtime: the frame cases go through the C API (stats from dapple_gpu_status_json "keepWarm"), the
// bracket / hold cases drive this executable's copy directly (gpu_runtime.h).
// Env: DAPPLE_GPU_SOFTWARE=1 accepts SwiftShader.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <stb_image_write.h>

#include "dapple/dapple_c.h"
#include "engine/exec/gpu_executor.h"
#include "engine/exec/gpu_runtime.h"
#include "../../test_tmp.h"

namespace eg = dapple::engine::gpu;
namespace fs = std::filesystem;

namespace {
int g_fail = 0;
#define CHECK(cond, ...)                                           \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                    \
      std::printf("\n");                                           \
      ++g_fail;                                                    \
    }                                                              \
  } while (0)

double num(const std::string& j, const char* key) {  // {"key":value,...}
  const std::string k = std::string("\"") + key + "\":";
  const std::size_t p = j.find(k);
  return p == std::string::npos ? -1e9 : std::atof(j.c_str() + p + k.size());
}
std::string apiKw() {  // the "keepWarm" object of the C API library's GPU status
  char* c = dapple_gpu_status_json();
  std::string j = c ? c : "";
  dapple_free_string(c);
  const std::size_t p = j.find("\"keepWarm\":");
  return p == std::string::npos ? std::string() : j.substr(p + 11);
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

fs::path syntheticJpeg() {
  const int w = 640, h = 480;
  std::vector<unsigned char> px(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      unsigned char* p = &px[(std::size_t(y) * w + x) * 3];
      p[0] = (unsigned char)(255 * x / w), p[1] = (unsigned char)(255 * y / h), p[2] = (unsigned char)((x * y) % 251);
    }
  const fs::path f = fs::temp_directory_path() / ("dapple_keep_warm_test_" + testUniqueTag() + ".jpg");
  stbi_write_jpg(f.string().c_str(), w, h, 3, px.data(), 95);
  return f;
}

bool frame(DappleSession* s, std::vector<unsigned char>& out) {
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = 320;
  rq.flags = DAPPLE_FRAME_INTERACTIVE;
  rq.active_param = "exposure";
  DappleImage img{};
  if (dapple_session_render_frame(s, "{\"exposure\":0.3,\"contrast\":20}", nullptr, &rq, &img, nullptr) != DAPPLE_OK) return false;
  out.assign(img.rgba, img.rgba + std::size_t(img.width) * img.height * 4);
  dapple_free_image(&img);
  return true;
}
}  // namespace

int main() {
  eg::installProvider();
  eg::setRuntimeEnabled(1);
  std::string why;
  std::shared_ptr<eg::Context> ctx = eg::sharedContext(&why);
  if (!ctx) {
    std::printf("  SKIP no GPU device: %s\n", why.c_str());
    return 77;
  }
  std::printf("  device: %s\n", ctx->caps().name.c_str());
  dapple_gpu_set_enabled(1);  // the C API library's runtime: frames on the GPU (its device is created on the first frame)
  const fs::path img = syntheticJpeg();
  DappleSession* s = dapple_session_open(img.string().c_str(), DAPPLE_QUALITY_PREVIEW, 640);
  if (!s) return std::printf("  FAIL open: %s\n", dapple_last_error()), 1;
  std::vector<unsigned char> ref, got;
  CHECK(frame(s, ref) && !ref.empty(), "reference frame");

  // prepare: the working set is allocated and one unit runs
  dapple_session_touch_hint(s, -1);
  sleepMs(300);
  std::string j = apiKw();
  std::printf("  prepare: %s\n", j.c_str());
  CHECK(num(j, "units") == 1, "prepare runs one unit");

  // a touch with a drag right away: every frame inside the window is bit-identical to the reference, units were queued behind the frames
  dapple_session_touch_hint(s, 80);
  int same = 0;
  for (int i = 0; i < 8; ++i) {
    CHECK(frame(s, got), "frame %d", i);
    same += got == ref;
  }
  CHECK(same == 8, "frames inside the window equal the reference: %d of 8", same);
  sleepMs(700);  // window: first real frame + dragMs (300), at most maxMs (400)
  j = apiKw();
  std::printf("  drag: %s\n", j.c_str());
  CHECK(num(j, "piggyback") >= 1, "units queued behind real frames");
  CHECK(num(j, "realFrames") >= 4, "the window saw the frames");  // counted from the moment the pump thread started
  CHECK(num(j, "wallMs") <= 400 + 60, "window bounded by maxMs");

  // a finger that does not move: the window ends at holdMs (250) and the GPU got units (this executable's runtime from here on)
  eg::keepWarm(-1);
  sleepMs(300);
  eg::keepWarm(80);
  sleepMs(600);
  j = eg::keepWarmJson();
  std::printf("  hold: %s\n", j.c_str());
  CHECK(num(j, "units") >= 2, "units ran");
  CHECK(num(j, "wallMs") >= 240 && num(j, "wallMs") <= 320, "hold window ~250 ms");
  CHECK(num(j, "piggyback") == 0, "no real frame, no piggyback");

  // while a real frame is pending (bracket held) the pump submits nothing
  eg::keepWarmFrame(1);
  eg::keepWarm(80);
  sleepMs(600);
  j = eg::keepWarmJson();
  eg::keepWarmFrame(-1);
  std::printf("  pending frame: %s\n", j.c_str());
  CHECK(num(j, "units") == 0, "nothing submitted while a real frame is pending");

  // off switch: no window
  const double w0 = num(apiKw(), "wallMs");
#if defined(_WIN32)
  _putenv_s("DAPPLE_HINT_MODE", "off");
#else
  setenv("DAPPLE_HINT_MODE", "off", 1);
#endif
  dapple_session_touch_hint(s, 80);
  sleepMs(400);
  CHECK(num(apiKw(), "wallMs") == w0, "DAPPLE_HINT_MODE=off: no window");

  CHECK(frame(s, got) && got == ref, "frame after the windows equals the reference");
  dapple_session_close(s);
  std::error_code ec;
  fs::remove(img, ec);
  if (g_fail) {
    std::printf("\nFAILED (%d failures)\n", g_fail);
    return 1;
  }
  std::printf("\nPASSED (0 failures)\n");
  return 0;
}
