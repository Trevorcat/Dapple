// dapple_view_* (core/include/dapple/dapple_view.h): the composition of RenderLoop + Presenter + the C API session.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "dapple/dapple_view.h"
#include "presenter.h"
#include "render_loop.h"
#include "src/sysperf.h"

using namespace dapple::present;

namespace {
double msBetween(std::int64_t a, std::int64_t b) { return double(b - a) / 1e6; }
thread_local std::string tlErr;
}  // namespace

struct DappleView {
  std::unique_ptr<Presenter> presenter;
  std::string variantNote;      // why the null presenter / fallback was chosen
  std::mutex mu;                // held for a whole frame and by every state change
  DappleSession* session = nullptr;
  int surfW = 0, surfH = 0;
  Snapshot last;                // newest snapshot that could not be shown (or was shown): re-rendered on attach
  bool lastPending = false;

  std::mutex cbMu;
  DappleViewFrameFn cb = nullptr;
  void* cbUser = nullptr;

  std::mutex anMu;
  std::string analysis;
  bool hasAnalysis = false;
  std::string lastErr;          // newest failure text (anMu)
  void setErr(const std::string& e) { std::lock_guard<std::mutex> l(anMu); lastErr = e; }

  // stats (loop thread writes, info reads)
  std::atomic<std::uint64_t> shown{0}, failedRender{0}, failedPresent{0}, noTarget{0};
  std::atomic<double> lastRenderMs{0}, lastPresentMs{0}, lastLatencyMs{0}, emaRenderMs{0}, emaPresentMs{0};
  std::atomic<std::int64_t> thermalPollNs{0};
  std::atomic<float> headroom{-1.f};

  std::unique_ptr<RenderLoop> loop;  // last member: destroyed first

  void fire(std::int64_t seq, int status, int w, int h, double r, double p, double lat) {
    std::lock_guard<std::mutex> l(cbMu);
    if (cb) cb(cbUser, seq, status, w, h, r, p, lat);
  }

  void polThermal(std::int64_t now) {
    // AThermal headroom is a binder call rate limited by the platform: at most every 4 s, only while frames are rendered
    if (now - thermalPollNs.load() < 4000000000LL) return;
    thermalPollNs.store(now);
    const float h = dapple::sys::adpf().headroom(10);
    headroom.store(h);
    if (h >= 1.0f) loop->setDivisor(2);  // at the throttle threshold: cap at 30 fps
    else if (h >= 0.f && h < 0.8f && divisorFromThermal) loop->setDivisor(userDivisor);
    divisorFromThermal = h >= 1.0f;
  }
  bool divisorFromThermal = false;
  int userDivisor = 1;

  void renderFrame(Snapshot&& s, std::int64_t) {
    int status = DAPPLE_VIEW_NO_TARGET, w = 0, h = 0;
    double renderMs = 0, presentMs = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      last = s;
      lastPending = true;
      if (session && presenter && presenter->hasWindow()) {
        DappleFrameRequest rq{};
        rq.size = int32_t(sizeof rq);
        rq.long_edge = s.longEdge;
        rq.flags = s.flags;
        rq.bake_size = s.bakeSize;
        DappleImage img{};
        char* an = nullptr;
        const std::int64_t t0 = steadyNowNs();
        const int rc = dapple_session_render_frame(session, s.payload.c_str(), s.lutPath.empty() ? nullptr : s.lutPath.c_str(), &rq, &img,
                                                   (s.flags & DAPPLE_FRAME_ANALYSIS) ? &an : nullptr);
        const std::int64_t t1 = steadyNowNs();
        renderMs = msBetween(t0, t1);
        if (rc != DAPPLE_OK || !img.rgba) {
          status = DAPPLE_VIEW_RENDER_FAILED;
          setErr(std::string("render: ") + dapple_last_error());
        } else {
          w = img.width;
          h = img.height;
          std::string err;
          PresentTiming pt;
          const bool ok = presenter->present(img.rgba, w, h, &pt, err);
          presentMs = msBetween(t1, steadyNowNs());
          if (ok) {
            status = DAPPLE_VIEW_SHOWN;
            lastPending = false;
          } else {
            status = DAPPLE_VIEW_PRESENT_FAILED;
            setErr("present: " + err);
          }
        }
        dapple_free_image(&img);
        if (an) {
          std::lock_guard<std::mutex> al(anMu);
          analysis = an;
          hasAnalysis = true;
          dapple_free_string(an);
        }
      }
    }
    const double lat = msBetween(s.submitNs, steadyNowNs());
    switch (status) {
      case DAPPLE_VIEW_SHOWN: ++shown; break;
      case DAPPLE_VIEW_RENDER_FAILED: ++failedRender; break;
      case DAPPLE_VIEW_PRESENT_FAILED: ++failedPresent; break;
      default: ++noTarget; break;
    }
    lastRenderMs = renderMs;
    lastPresentMs = presentMs;
    lastLatencyMs = lat;
    if (status == DAPPLE_VIEW_SHOWN) {
      emaRenderMs = emaRenderMs.load() == 0 ? renderMs : emaRenderMs.load() * 0.8 + renderMs * 0.2;
      emaPresentMs = emaPresentMs.load() == 0 ? presentMs : emaPresentMs.load() * 0.8 + presentMs * 0.2;
    }
    polThermal(steadyNowNs());
    fire(std::int64_t(s.seq), status, w, h, renderMs, presentMs, lat);
  }
};

extern "C" {

DappleView* dapple_view_create(const DappleViewOptions* o) {
  auto v = std::make_unique<DappleView>();
  const int flags = o ? o->flags : 0;
  std::string err;
  if (flags & DAPPLE_VIEW_NULL_PRESENTER) {
    v->presenter = makeNullPresenter();
  } else {
    v->presenter = makeVulkanPresenter(err);
    if (!v->presenter) {
      tlErr = "display: " + err;
      return nullptr;
    }
  }
  DappleView* raw = v.get();
  v->loop = std::make_unique<RenderLoop>([raw](Snapshot&& s, std::int64_t t) { raw->renderFrame(std::move(s), t); },
                                          (flags & DAPPLE_VIEW_TIMER_LOOP) == 0);
  return v.release();
}

void dapple_view_destroy(DappleView* v) {
  if (!v) return;
  v->loop.reset();  // joins the thread (a frame in flight finishes first)
  {
    std::lock_guard<std::mutex> l(v->cbMu);
    v->cb = nullptr;
  }
  delete v;
}

int32_t dapple_view_attach_window(DappleView* v, void* win, int32_t w, int32_t h) {
  if (!v) return DAPPLE_ERROR;
  bool resubmit = false;
  {
    std::lock_guard<std::mutex> l(v->mu);
    std::string err;
    if (!v->presenter->setWindow(win, w, h, err)) {
      tlErr = "display: " + err;
      return DAPPLE_ERROR;
    }
    v->surfW = w;
    v->surfH = h;
    resubmit = true;  // a new surface starts empty: show the newest snapshot again
  }
  if (resubmit) {
    Snapshot s;
    bool have;
    {
      std::lock_guard<std::mutex> l(v->mu);
      s = v->last;
      have = !s.payload.empty();
    }
    if (have && !v->loop->hasPending()) v->loop->submit(std::move(s));
  }
  return DAPPLE_OK;
}

void dapple_view_detach_window(DappleView* v) {
  if (!v) return;
  std::lock_guard<std::mutex> l(v->mu);
  v->presenter->clearWindow();
}

int32_t dapple_view_resize(DappleView* v, int32_t w, int32_t h) {
  if (!v) return DAPPLE_ERROR;
  // the swapchain follows the surface extent by itself (VK_ERROR_OUT_OF_DATE); only the request size is updated
  std::lock_guard<std::mutex> l(v->mu);
  v->surfW = w;
  v->surfH = h;
  return DAPPLE_OK;
}

void dapple_view_bind_session(DappleView* v, DappleSession* s) {
  if (!v) return;
  if (!s) v->loop->quiesce(true);
  std::lock_guard<std::mutex> l(v->mu);  // waits for the frame in flight
  v->session = s;
  if (!s) {
    v->last = Snapshot{};
    v->lastPending = false;
  }
}

int64_t dapple_view_submit(DappleView* v, const char* params, const char* lut, const DappleFrameRequest* rq) {
  if (!v) return 0;
  Snapshot s;
  s.payload = params ? params : "";
  if (s.payload.empty()) s.payload = "{}";
  s.lutPath = lut ? lut : "";
  if (rq) {
    s.longEdge = rq->long_edge;
    s.flags = rq->flags;
    s.bakeSize = rq->bake_size;
  }
  return std::int64_t(v->loop->submit(std::move(s)));
}

void dapple_view_set_callback(DappleView* v, DappleViewFrameFn fn, void* user) {
  if (!v) return;
  std::lock_guard<std::mutex> l(v->cbMu);
  v->cb = fn;
  v->cbUser = user;
}

void dapple_view_set_rate_divisor(DappleView* v, int32_t d) {
  if (!v) return;
  v->userDivisor = d < 1 ? 1 : d;
  v->loop->setDivisor(v->userDivisor);
}

void dapple_view_set_paused(DappleView* v, int32_t p) {
  if (v) v->loop->pause(p != 0);
}

void dapple_view_trim(DappleView* v, int32_t level) {
  if (!v) return;
  std::lock_guard<std::mutex> l(v->mu);
  v->presenter->trim(level);
}

const char* dapple_view_last_error(void) { return tlErr.c_str(); }

char* dapple_view_take_analysis(DappleView* v) {
  if (!v) return nullptr;
  std::lock_guard<std::mutex> l(v->anMu);
  if (!v->hasAnalysis) return nullptr;
  v->hasAnalysis = false;
  char* out = static_cast<char*>(std::malloc(v->analysis.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, v->analysis.c_str(), v->analysis.size() + 1);
  return out;
}

char* dapple_view_info_json(DappleView* v) {
  if (!v) return nullptr;
  const LoopStats st = v->loop->stats();
  std::string pi;
  {
    std::lock_guard<std::mutex> l(v->mu);
    pi = v->presenter->info();
  }
  std::string errCopy;
  {
    std::lock_guard<std::mutex> l(v->anMu);
    errCopy = v->lastErr;
  }
  for (char& c : errCopy)
    if (c == '"' || c == '\\' || c < 32) c = ' ';
  char buf[960];
  std::snprintf(buf, sizeof buf,
                "{\"presenter\":%s,\"loop\":{\"submits\":%llu,\"renders\":%llu,\"dropped\":%llu,\"wakeups\":%llu,\"vsyncs\":%llu,"
                "\"skippedByCap\":%llu},\"shown\":%llu,\"renderFailed\":%llu,\"presentFailed\":%llu,\"noTarget\":%llu,"
                "\"renderMs\":%.2f,\"presentMs\":%.2f,\"latencyMs\":%.2f,\"emaRenderMs\":%.2f,\"emaPresentMs\":%.2f,"
                "\"headroom\":%.3f,\"divisor\":%d,\"adpf\":%s,\"lastError\":\"%s\"}",
                pi.c_str(), (unsigned long long)st.submits, (unsigned long long)st.renders, (unsigned long long)st.dropped,
                (unsigned long long)st.wakeups, (unsigned long long)st.vsyncs, (unsigned long long)st.skippedByCap,
                (unsigned long long)v->shown.load(), (unsigned long long)v->failedRender.load(),
                (unsigned long long)v->failedPresent.load(), (unsigned long long)v->noTarget.load(), v->lastRenderMs.load(),
                v->lastPresentMs.load(), v->lastLatencyMs.load(), v->emaRenderMs.load(), v->emaPresentMs.load(), double(v->headroom.load()),
                v->userDivisor, dapple::sys::adpf().hintAvailable() ? "true" : "false", errCopy.c_str());
  char* out = static_cast<char*>(std::malloc(std::strlen(buf) + 1));
  if (out) std::memcpy(out, buf, std::strlen(buf) + 1);
  return out;
}

}  // extern "C"
