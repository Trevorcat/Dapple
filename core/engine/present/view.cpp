// dapple_view_* (core/include/dapple/dapple_view.h): the composition of RenderLoop + Presenter + the C API session.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "dapple/dapple_view.h"
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"
#endif
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
  std::string lastActive;       // active_param of the newest rendered snapshot (anMu; info json, tests)
  void setErr(const std::string& e) { std::lock_guard<std::mutex> l(anMu); lastErr = e; }

  // stats (loop thread writes, info reads)
  std::atomic<std::uint64_t> shown{0}, failedRender{0}, failedPresent{0}, noTarget{0};
  // zero readback accounting (loop thread writes): frames the engine wrote straight into the presenter's image vs frames that came
  // back as host pixels (CPU executor, GLES, film, image too small, device change) and were uploaded
  std::atomic<std::uint64_t> extFrames{0}, hostFrames{0}, hostBytes{0}, extTooSmall{0}, extRefused{0};
  std::atomic<double> emaFenceMs{0};
  // size history of the last frame (mu): capacity of the next external image is estimated from it
  int histReqLong = 0, histW = 0, histH = 0;
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
        if (!s.activeParam.empty()) rq.active_param = s.activeParam.c_str();  // grain stays on while a grain slider is dragged
        {
          std::lock_guard<std::mutex> al(anMu);
          lastActive = s.activeParam;
        }
        DappleImage img{};
        char* an = nullptr;
        // zero readback: ask the presenter for an image on the engine's device when the size of the frame can be estimated
        ExternalTarget et;
        DappleExternalImage ei{};
        bool ext = false;
        if (histW > 0 && histH > 0) {
          const double k = (s.longEdge > 0 && histReqLong > 0) ? double(s.longEdge) / double(histReqLong) : 1.0;
          const int cw = int(histW * k * 1.03) + 8, ch = int(histH * k * 1.03) + 8;  // headroom: crops / straighten change the size a little
          std::string berr;
          ext = presenter->beginExternal(cw, ch, &et, berr);
          if (ext) {
            ei.image = et.image;
            ei.width = et.w;
            ei.height = et.h;
            ei.layout_in = et.layoutIn;
            ei.layout_out = et.layoutOut;
            rq.external = &ei;
          } else {
            ++extRefused;
          }
        }
        const std::int64_t t0 = steadyNowNs();
        const int rc = dapple_session_render_frame(session, s.payload.c_str(), s.lutPath.empty() ? nullptr : s.lutPath.c_str(), &rq, &img,
                                                   (s.flags & DAPPLE_FRAME_ANALYSIS) ? &an : nullptr);
        const std::int64_t t1 = steadyNowNs();
        renderMs = msBetween(t0, t1);
        const bool onGpu = rc == DAPPLE_OK && !img.rgba && img.width > 0 && img.height > 0;  // the picture is in the external image
        if (rc != DAPPLE_OK || (!img.rgba && !onGpu)) {
          status = DAPPLE_VIEW_RENDER_FAILED;
          setErr(std::string("render: ") + dapple_last_error());
        } else {
          w = img.width;
          h = img.height;
          std::string err;
          PresentTiming pt;
          bool ok;
          if (onGpu) {
            ok = ext && presenter->presentExternal(w, h, &pt, err);
            if (!ext) err = "engine returned an external frame that was not asked for";
          } else {
            if (ext) ++extTooSmall;  // CPU frame / GLES / image too small: pixels came back
            ok = presenter->present(img.rgba, w, h, &pt, err);
          }
          presentMs = msBetween(t1, steadyNowNs());
          if (ok) {
            status = DAPPLE_VIEW_SHOWN;
            lastPending = false;
            histReqLong = s.longEdge;
            histW = w;
            histH = h;
            if (onGpu) {
              ++extFrames;
              emaFenceMs = emaFenceMs.load() == 0 ? pt.fenceMs : emaFenceMs.load() * 0.8 + pt.fenceMs * 0.2;
            } else {
              ++hostFrames;
              hostBytes += std::uint64_t(w) * std::uint64_t(h) * 4;
            }
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
#if defined(__ANDROID__)
    // diagnostics: one line per 30 shown frames (zero-readback counters + the split of the last frame), logcat tag dapple_view
    if (status == DAPPLE_VIEW_SHOWN && (shown.load() % 30) == 0)
      __android_log_print(ANDROID_LOG_INFO, "dapple_view",
                          "shown %llu ext %llu host %llu hostBytes %llu tooSmall %llu refused %llu | last %dx%d render %.2f ms present %.2f ms "
                          "latency %.2f ms | ema render %.2f present %.2f fenceWait %.2f",
                          (unsigned long long)shown.load(), (unsigned long long)extFrames.load(), (unsigned long long)hostFrames.load(),
                          (unsigned long long)hostBytes.load(), (unsigned long long)extTooSmall.load(), (unsigned long long)extRefused.load(), w, h,
                          renderMs, presentMs, lat, emaRenderMs.load(), emaPresentMs.load(), emaFenceMs.load());
#endif
    fire(std::int64_t(s.seq), status, w, h, renderMs, presentMs, lat);
  }
};

extern "C" {

DappleView* dapple_view_create(const DappleViewOptions* o) {
  auto v = std::make_unique<DappleView>();
  const int flags = o ? o->flags : 0;
  std::string err;
  if (flags & DAPPLE_VIEW_NULL_PRESENTER) {
    v->presenter = makeNullPresenter((flags & DAPPLE_VIEW_NULL_EXTERNAL) != 0);
  } else {
    v->presenter = makeVulkanPresenter(err);
    if (!v->presenter) {
      tlErr = "display: " + err;
      return nullptr;
    }
  }
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
#endif
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
  v->histW = v->histH = v->histReqLong = 0;  // another photo: sizes unknown
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
    // active_param is additive: honoured only when the caller's struct covers it (size-versioned like dapple_session_render_frame)
    const std::size_t covers = offsetof(DappleFrameRequest, active_param) + sizeof(rq->active_param);
    if (rq->size > 0 && std::size_t(rq->size) >= covers && rq->active_param) s.activeParam = rq->active_param;
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
  std::string activeCopy;
  {
    std::lock_guard<std::mutex> l(v->anMu);
    activeCopy = v->lastActive;
  }
  for (char& c : activeCopy)
    if (c == '"' || c == '\\' || c < 32) c = ' ';
  if (activeCopy.size() > 40) activeCopy.resize(40);
  char buf[1500];
  std::snprintf(buf, sizeof buf,
                "{\"presenter\":%s,\"loop\":{\"submits\":%llu,\"renders\":%llu,\"dropped\":%llu,\"wakeups\":%llu,\"vsyncs\":%llu,"
                "\"skippedByCap\":%llu},\"shown\":%llu,\"renderFailed\":%llu,\"presentFailed\":%llu,\"noTarget\":%llu,"
                "\"renderMs\":%.2f,\"presentMs\":%.2f,\"latencyMs\":%.2f,\"emaRenderMs\":%.2f,\"emaPresentMs\":%.2f,"
                "\"headroom\":%.3f,\"divisor\":%d,\"adpf\":%s,\"zeroReadback\":{\"externalFrames\":%llu,\"hostFrames\":%llu,"
                "\"hostBytes\":%llu,\"tooSmall\":%llu,\"refused\":%llu,\"emaFenceMs\":%.2f},\"lastError\":\"%s\",\"activeParam\":\"%s\"}",
                pi.c_str(), (unsigned long long)st.submits, (unsigned long long)st.renders, (unsigned long long)st.dropped,
                (unsigned long long)st.wakeups, (unsigned long long)st.vsyncs, (unsigned long long)st.skippedByCap,
                (unsigned long long)v->shown.load(), (unsigned long long)v->failedRender.load(),
                (unsigned long long)v->failedPresent.load(), (unsigned long long)v->noTarget.load(), v->lastRenderMs.load(),
                v->lastPresentMs.load(), v->lastLatencyMs.load(), v->emaRenderMs.load(), v->emaPresentMs.load(), double(v->headroom.load()),
                v->userDivisor, dapple::sys::adpf().hintAvailable() ? "true" : "false", (unsigned long long)v->extFrames.load(),
                (unsigned long long)v->hostFrames.load(), (unsigned long long)v->hostBytes.load(), (unsigned long long)v->extTooSmall.load(),
                (unsigned long long)v->extRefused.load(), v->emaFenceMs.load(), errCopy.c_str(), activeCopy.c_str());
  std::string js(buf);
  if (char* es = dapple_engine_stats_json()) {  // which executor rendered the frames (GPU / CPU, reasons), process-wide
    if (!js.empty() && js.back() == '}') js.pop_back();
    js += ",\"engine\":";
    js += es;
    js += "}";
    dapple_free_string(es);
  }
  char* out = static_cast<char*>(std::malloc(js.size() + 1));
  if (out) std::memcpy(out, js.c_str(), js.size() + 1);
  return out;
}

}  // extern "C"
