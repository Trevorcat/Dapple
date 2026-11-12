// Stage (d) tests: RenderLoop semantics (latest wins, vsync grid, idle = zero work, pause, quiesce) and the view's
// lifecycle (attach / detach / re-attach re-renders the newest snapshot, unbind, trim, destroy under load) on the
// null presenter with a real session (a synthetic JPEG). The Vulkan swapchain is covered on the emulator.
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "dapple/dapple_view.h"
#include "dapple/encode.h"
#include "dapple/image.h"
#include "render_loop.h"

using namespace dapple::present;
using namespace std::chrono_literals;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

static bool waitFor(const std::function<bool()>& f, int ms) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!f()) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

static void testLatestWins() {
  std::mutex m;
  std::vector<std::uint64_t> seen;
  RenderLoop loop(
      [&](Snapshot&& s, std::int64_t) {
        std::this_thread::sleep_for(30ms);  // a slow frame
        std::lock_guard<std::mutex> l(m);
        seen.push_back(s.seq);
      },
      false, 16666667);
  std::uint64_t last = 0;
  for (int i = 0; i < 100; ++i) {
    Snapshot s;
    s.payload = "x";
    last = loop.submit(std::move(s));
    std::this_thread::sleep_for(2ms);
  }
  CHECK(waitFor([&] {
    std::lock_guard<std::mutex> l(m);
    return !seen.empty() && seen.back() == last;
  }, 2000));
  const LoopStats st = loop.stats();
  std::printf("  latest-wins: submits %llu renders %llu dropped %llu\n", (unsigned long long)st.submits, (unsigned long long)st.renders,
              (unsigned long long)st.dropped);
  CHECK(st.submits == 100);
  CHECK(st.renders < 60);                       // far fewer frames than snapshots
  CHECK(st.renders + st.dropped == st.submits); // every snapshot is either rendered or counted as dropped
  CHECK(std::is_sorted(seen.begin(), seen.end()));
}

static void testIdleAndGrid() {
  std::mutex m;
  std::vector<std::int64_t> starts;
  const std::int64_t period = 16666667;
  RenderLoop loop(
      [&](Snapshot&&, std::int64_t) {
        std::lock_guard<std::mutex> l(m);
        starts.push_back(steadyNowNs());
      },
      false, period);
  std::this_thread::sleep_for(300ms);
  CHECK(loop.stats().wakeups == 0 && loop.stats().renders == 0 && loop.stats().vsyncs == 0);  // idle: nothing happens
  for (int i = 0; i < 20; ++i) {
    Snapshot s;
    s.payload = "x";
    loop.submit(std::move(s));
    std::this_thread::sleep_for(3ms);
  }
  std::this_thread::sleep_for(100ms);
  const LoopStats a = loop.stats();
  std::this_thread::sleep_for(400ms);
  const LoopStats b = loop.stats();
  CHECK(a.renders == b.renders && a.vsyncs == b.vsyncs && b.wakeups == 0);  // and idle again afterwards
  // never more than one frame per vsync period, however fast the submits arrive (host timers are coarse: median gap)
  std::vector<std::int64_t> gaps;
  for (std::size_t i = 1; i < starts.size(); ++i) gaps.push_back(starts[i] - starts[i - 1]);
  std::sort(gaps.begin(), gaps.end());
  const std::int64_t med = gaps.empty() ? 0 : gaps[gaps.size() / 2];
  std::printf("  grid: %zu frames, median gap %.2f ms (period %.2f)\n", starts.size(), double(med) / 1e6, double(period) / 1e6);
  CHECK(!gaps.empty() && med >= period * 9 / 10);
}

static void testPauseQuiesce() {
  std::atomic<int> n{0};
  std::atomic<bool> inFrame{false};
  RenderLoop loop(
      [&](Snapshot&&, std::int64_t) {
        inFrame = true;
        std::this_thread::sleep_for(40ms);
        ++n;
        inFrame = false;
      },
      false, 16666667);
  loop.pause(true);
  Snapshot s;
  s.payload = "a";
  loop.submit(std::move(s));
  std::this_thread::sleep_for(100ms);
  CHECK(n == 0 && loop.hasPending());  // paused: the latest snapshot waits
  loop.pause(false);
  CHECK(waitFor([&] { return n == 1; }, 1000));
  // quiesce waits for a frame in flight and (drop) clears the mailbox
  Snapshot s2;
  s2.payload = "b";
  loop.submit(std::move(s2));
  CHECK(waitFor([&] { return inFrame.load(); }, 1000));
  loop.quiesce(true);
  CHECK(!inFrame.load());
  Snapshot s3;
  s3.payload = "c";
  loop.pause(true);
  loop.submit(std::move(s3));
  loop.quiesce(true);
  CHECK(!loop.hasPending());
  loop.pause(false);
  std::this_thread::sleep_for(100ms);
  CHECK(n == 2);
}

// ---- view ------------------------------------------------------------------------------------------------------------
struct Events {
  std::mutex m;
  std::vector<int> status;
  std::vector<std::int64_t> seqs;
  int w = 0, h = 0;
};
static void onFrame(void* u, int64_t seq, int32_t st, int32_t w, int32_t h, double, double, double) {
  auto* e = static_cast<Events*>(u);
  std::lock_guard<std::mutex> l(e->m);
  e->status.push_back(st);
  e->seqs.push_back(seq);
  if (st == DAPPLE_VIEW_SHOWN) e->w = w, e->h = h;
}
static int count(Events& e, int st) {
  std::lock_guard<std::mutex> l(e.m);
  return int(std::count(e.status.begin(), e.status.end(), st));
}

static std::string makeJpeg() {
  dapple::Image8 im;
  im.width = 320;
  im.height = 240;
  im.data.resize(std::size_t(im.width) * im.height * 3);
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) {
      std::uint8_t* p = &im.data[(std::size_t(y) * im.width + x) * 3];
      p[0] = std::uint8_t(x * 255 / im.width);
      p[1] = std::uint8_t(y * 255 / im.height);
      p[2] = std::uint8_t(128 + 60 * ((x / 32 + y / 32) & 1));
    }
  const auto path = std::filesystem::temp_directory_path() / "dapple_present_test.jpg";
  if (!dapple::writeJpeg(im, path)) return {};
  return path.string();
}

static void testView() {
  const std::string jpg = makeJpeg();
  CHECK(!jpg.empty());
  DappleSession* sess = dapple_session_open(jpg.c_str(), DAPPLE_QUALITY_PREVIEW, 512);
  CHECK(sess != nullptr);
  if (!sess) return;
  DappleViewOptions o{int32_t(sizeof o), DAPPLE_VIEW_TIMER_LOOP | DAPPLE_VIEW_NULL_PRESENTER};
  DappleView* v = dapple_view_create(&o);
  CHECK(v != nullptr);
  Events ev;
  dapple_view_set_callback(v, onFrame, &ev);
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = 256;
  rq.flags = DAPPLE_FRAME_INTERACTIVE;

  // no target yet: the snapshot is kept, not shown
  dapple_view_submit(v, "{\"exposure\":0.2}", nullptr, &rq);
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_NO_TARGET) >= 1; }, 1000));
  dapple_view_bind_session(v, sess);
  int fakeWindow = 0;
  CHECK(dapple_view_attach_window(v, &fakeWindow, 640, 480) == DAPPLE_OK);  // attaching re-renders the newest snapshot
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) >= 1; }, 3000));
  CHECK(ev.w == 256 && ev.h == 192);

  // frames with analysis deliver the JSON once
  rq.flags = DAPPLE_FRAME_ANALYSIS;
  rq.long_edge = 320;
  const int before = count(ev, DAPPLE_VIEW_SHOWN);
  dapple_view_submit(v, "{}", nullptr, &rq);
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > before; }, 3000));
  char* an = dapple_view_take_analysis(v);
  CHECK(an != nullptr);
  dapple_free_string(an);
  CHECK(dapple_view_take_analysis(v) == nullptr);

  // background: detach, submit (kept), foreground: attach shows it
  dapple_view_detach_window(v);
  const int nt = count(ev, DAPPLE_VIEW_NO_TARGET);
  rq.flags = 0;
  rq.long_edge = 200;
  dapple_view_submit(v, "{\"exposure\":0.5}", nullptr, &rq);
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_NO_TARGET) > nt; }, 1000));
  const int sh = count(ev, DAPPLE_VIEW_SHOWN);
  CHECK(dapple_view_attach_window(v, &fakeWindow, 800, 600) == DAPPLE_OK);
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > sh; }, 3000));
  CHECK(ev.w == 200);

  // active_param (the slider being dragged) reaches the frame request; a caller whose struct is too short to carry it is ignored
  {
    auto activeOf = [&](const char* expect) {
      char* info = dapple_view_info_json(v);
      const std::string j = info ? info : "";
      dapple_free_string(info);
      return j.find(std::string("\"activeParam\":\"") + expect + "\"") != std::string::npos;
    };
    DappleFrameRequest ar{};
    ar.size = sizeof ar;
    ar.long_edge = 200;
    ar.flags = DAPPLE_FRAME_INTERACTIVE;
    ar.active_param = "film_grain";
    int sh3 = count(ev, DAPPLE_VIEW_SHOWN);
    dapple_view_submit(v, "{}", nullptr, &ar);
    CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > sh3; }, 3000));
    CHECK(activeOf("film_grain"));
    ar.size = int32_t(offsetof(DappleFrameRequest, active_param));  // an older caller: the field is not part of its struct
    sh3 = count(ev, DAPPLE_VIEW_SHOWN);
    dapple_view_submit(v, "{}", nullptr, &ar);
    CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > sh3; }, 3000));
    CHECK(activeOf(""));
  }

  // trim keeps working afterwards
  dapple_view_trim(v, 0);
  dapple_view_trim(v, 1);
  dapple_view_trim(v, 2);
  const int sh2 = count(ev, DAPPLE_VIEW_SHOWN);
  dapple_view_submit(v, "{}", nullptr, &rq);
  CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > sh2; }, 3000));

  // stress: submit / detach / attach / trim / pause from several threads, then unbind and close the session
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  th.emplace_back([&] {
    std::mt19937 r(1);
    while (!stop) {
      rq.long_edge = 128 + int(r() % 128);
      dapple_view_submit(v, "{\"contrast\":0.1}", nullptr, &rq);
      std::this_thread::sleep_for(std::chrono::microseconds(500 + r() % 3000));
    }
  });
  th.emplace_back([&] {
    std::mt19937 r(2);
    while (!stop) {
      dapple_view_detach_window(v);
      std::this_thread::sleep_for(std::chrono::milliseconds(r() % 8));
      dapple_view_attach_window(v, &fakeWindow, 640 + int(r() % 100), 480);
      dapple_view_trim(v, int32_t(r() % 3));
      std::this_thread::sleep_for(std::chrono::milliseconds(r() % 8));
    }
  });
  th.emplace_back([&] {
    std::mt19937 r(3);
    while (!stop) {
      dapple_view_set_paused(v, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(r() % 6));
      dapple_view_set_paused(v, 0);
      dapple_view_set_rate_divisor(v, 1 + int(r() % 2));
      std::this_thread::sleep_for(std::chrono::milliseconds(r() % 10));
    }
  });
  std::this_thread::sleep_for(1500ms);
  stop = true;
  for (auto& t : th) t.join();
  dapple_view_set_paused(v, 0);
  dapple_view_set_rate_divisor(v, 1);
  char* info = dapple_view_info_json(v);
  CHECK(info != nullptr);
  if (info) std::printf("  info: %s\n", info);
  dapple_free_string(info);

  // unbind before closing: nothing may touch the session afterwards
  dapple_view_bind_session(v, nullptr);
  const int n0 = count(ev, DAPPLE_VIEW_SHOWN);
  dapple_view_submit(v, "{}", nullptr, &rq);
  std::this_thread::sleep_for(100ms);
  CHECK(count(ev, DAPPLE_VIEW_SHOWN) == n0);
  dapple_session_close(sess);
  // destroy with a pending snapshot
  dapple_view_submit(v, "{}", nullptr, &rq);
  dapple_view_destroy(v);
  std::filesystem::remove(jpg);
}

// The zero-readback path with a session that cannot use it (CPU executor): the presenter offers an image, the pixels come back, the
// view must show them through the host path and count the refusal.
static void testExternalFallback() {
  const std::string jpg = makeJpeg();
  DappleSession* sess = dapple_session_open(jpg.c_str(), DAPPLE_QUALITY_PREVIEW, 512);
  CHECK(sess != nullptr);
  if (!sess) return;
  DappleViewOptions o{int32_t(sizeof o), DAPPLE_VIEW_TIMER_LOOP | DAPPLE_VIEW_NULL_PRESENTER | DAPPLE_VIEW_NULL_EXTERNAL};
  DappleView* v = dapple_view_create(&o);
  CHECK(v != nullptr);
  Events ev;
  dapple_view_set_callback(v, onFrame, &ev);
  dapple_view_bind_session(v, sess);
  int fakeWindow = 0;
  CHECK(dapple_view_attach_window(v, &fakeWindow, 640, 480) == DAPPLE_OK);
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = 256;
  rq.flags = DAPPLE_FRAME_INTERACTIVE;
  for (int i = 0; i < 6; ++i) {
    const int before = count(ev, DAPPLE_VIEW_SHOWN);
    dapple_view_submit(v, i & 1 ? "{\"exposure\":0.3}" : "{\"exposure\":0.1}", nullptr, &rq);
    CHECK(waitFor([&] { return count(ev, DAPPLE_VIEW_SHOWN) > before; }, 3000));
  }
  CHECK(count(ev, DAPPLE_VIEW_RENDER_FAILED) == 0 && count(ev, DAPPLE_VIEW_PRESENT_FAILED) == 0);
  char* info = dapple_view_info_json(v);
  CHECK(info != nullptr);
  if (info) {
    const std::string j = info;
    std::printf("  external fallback info: %s\n", j.c_str());
    CHECK(j.find("\"externalFrames\":0") != std::string::npos);  // the CPU executor never writes the image
    CHECK(j.find("\"hostFrames\":6") != std::string::npos);
    CHECK(j.find("\"tooSmall\":5") != std::string::npos);        // the first frame has no size history: no image was offered
  }
  dapple_free_string(info);
  dapple_view_bind_session(v, nullptr);
  dapple_session_close(sess);
  dapple_view_destroy(v);
  std::filesystem::remove(jpg);
}

int main() {
  std::printf("render loop\n");
  testLatestWins();
  testIdleAndGrid();
  testPauseQuiesce();
  std::printf("view\n");
  testView();
  std::printf("zero-readback fallback\n");
  testExternalFallback();
  std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
  return g_fail ? 1 : 0;
}
