#include "render_loop.h"

#include "engine/exec/big_core.h"

#include <chrono>

#if defined(__ANDROID__)
#include <android/choreographer.h>
#include <android/looper.h>
#include <sys/eventfd.h>
#include <unistd.h>
#define DAPPLE_PRESENT_CHOREO 1
#endif

namespace dapple::present {

std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef DAPPLE_PRESENT_CHOREO
struct RenderLoop::Impl {
  ALooper* looper = nullptr;
  AChoreographer* choreo = nullptr;
  int efd = -1;
  bool armed = false;  // loop thread only
  unsigned tick = 0;
};
#else
struct RenderLoop::Impl {};
#endif

RenderLoop::RenderLoop(RenderFn fn, bool useVsync, std::int64_t periodNs)
    : fn_(std::move(fn)), useVsync_(useVsync), period_(periodNs) {
  impl_ = new Impl();
  th_ = std::thread([this] { threadMain(); });
}

RenderLoop::~RenderLoop() {
  {
    std::lock_guard<std::mutex> l(m_);
    stop_ = true;
  }
  cv_.notify_all();
#ifdef DAPPLE_PRESENT_CHOREO
  if (impl_->efd >= 0) {
    std::uint64_t one = 1;
    (void)!::write(impl_->efd, &one, sizeof one);
  }
#endif
  if (th_.joinable()) th_.join();
  delete impl_;
}

std::uint64_t RenderLoop::submit(Snapshot&& s) {
  std::uint64_t id;
  bool wake;
  {
    std::lock_guard<std::mutex> l(m_);
    id = ++seq_;
    s.seq = id;
    s.submitNs = steadyNowNs();
    ++st_.submits;
    if (full_) ++st_.dropped;
    wake = !full_;  // the loop is already armed (or will re-check the mailbox) when it was full
    box_ = std::move(s);
    full_ = true;
  }
  if (wake) {
#ifdef DAPPLE_PRESENT_CHOREO
    if (useVsync_ && impl_->efd >= 0) {
      std::uint64_t one = 1;
      (void)!::write(impl_->efd, &one, sizeof one);
      return id;
    }
#endif
    cv_.notify_one();
  }
  return id;
}

void RenderLoop::quiesce(bool drop) {
  std::unique_lock<std::mutex> l(m_);
  if (drop) full_ = false;
  idleCv_.wait(l, [this] { return !rendering_; });
}

void RenderLoop::pause(bool p) {
  paused_.store(p);
  if (!p) {
    bool pend;
    {
      std::lock_guard<std::mutex> l(m_);
      pend = full_;
    }
    if (pend) {
#ifdef DAPPLE_PRESENT_CHOREO
      if (useVsync_ && impl_->efd >= 0) {
        std::uint64_t one = 1;
        (void)!::write(impl_->efd, &one, sizeof one);
        return;
      }
#endif
      cv_.notify_one();
    }
  }
}

bool RenderLoop::hasPending() const {
  std::lock_guard<std::mutex> l(m_);
  return full_;
}

LoopStats RenderLoop::stats() const {
  std::lock_guard<std::mutex> l(m_);
  return st_;
}

namespace {
struct Taken {
  bool ok = false;
  Snapshot s;
};
}  // namespace

// Takes the mailbox (marks the loop as rendering under the same lock, so quiesce() cannot slip in between).
static Taken take(std::mutex& m, bool& full, Snapshot& box, bool& rendering, std::atomic<bool>& paused) {
  Taken t;
  std::lock_guard<std::mutex> l(m);
  if (full && !paused.load()) {
    t.s = std::move(box);
    full = false;
    rendering = true;
    t.ok = true;
  }
  return t;
}

#ifdef DAPPLE_PRESENT_CHOREO
static void vsyncCb(std::int64_t frameTimeNanos, void* data);
static int wakeCb(int fd, int, void* data);

static void arm(RenderLoop::Impl* im, void* self) {
  if (im->armed) return;
  im->armed = true;
  AChoreographer_postFrameCallback64(im->choreo, vsyncCb, self);
}

// Both callbacks need the loop's private state; they are friends through this accessor.
struct LoopAccess {
  static void onWake(RenderLoop* l);
  static void onVsync(RenderLoop* l, std::int64_t t);
};
#endif

void RenderLoop::threadMain() {
  dapple::engine::gpu::detail::pinCurrentThreadToBigCores();  // this thread renders and presents every frame
#ifdef DAPPLE_PRESENT_CHOREO
  if (useVsync_) {
    impl_->looper = ALooper_prepare(0);
    impl_->choreo = AChoreographer_getInstance();
    impl_->efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl_->choreo && impl_->efd >= 0) {
      ALooper_addFd(impl_->looper, impl_->efd, 1, ALOOPER_EVENT_INPUT, wakeCb, this);
      for (;;) {
        {
          std::lock_guard<std::mutex> l(m_);
          if (stop_) break;
        }
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);  // blocks: zero cost while idle
      }
      ALooper_removeFd(impl_->looper, impl_->efd);
      ::close(impl_->efd);
      impl_->efd = -1;
      return;
    }
    // no Choreographer on this thread: fall through to the timer grid
    if (impl_->efd >= 0) {
      ::close(impl_->efd);
      impl_->efd = -1;
    }
  }
#endif
  unsigned tick = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> l(m_);
      cv_.wait(l, [this] { return stop_ || (full_ && !paused_.load()); });
      if (stop_) return;
    }
    // align the frame start to the next tick of the grid
    const std::int64_t p = period_.load(), now = steadyNowNs();
    const std::int64_t next = (now / p + 1) * p;
    std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
    {
      std::lock_guard<std::mutex> l(m_);
      ++st_.vsyncs;
    }
    if (tick++ % unsigned(divisor_.load()) != 0) {
      std::lock_guard<std::mutex> l(m_);
      ++st_.skippedByCap;
      continue;
    }
    Taken t = take(m_, full_, box_, rendering_, paused_);
    if (!t.ok) {
      std::lock_guard<std::mutex> l(m_);
      if (!stop_ && !full_) ++st_.wakeups;
      continue;
    }
    fn_(std::move(t.s), next);
    {
      std::lock_guard<std::mutex> l(m_);
      ++st_.renders;
      rendering_ = false;
    }
    idleCv_.notify_all();
  }
}

#ifdef DAPPLE_PRESENT_CHOREO
void LoopAccess::onWake(RenderLoop* l) {
  std::uint64_t v;
  while (::read(l->impl_->efd, &v, sizeof v) > 0) {
  }
  bool pend;
  {
    std::lock_guard<std::mutex> g(l->m_);
    pend = l->full_ && !l->stop_;
  }
  if (pend && !l->paused_.load()) arm(l->impl_, l);
  else if (pend) {
    // paused: stay unarmed until pause(false) writes the eventfd again
  } else {
    std::lock_guard<std::mutex> g(l->m_);
    if (!l->stop_ && !l->full_) ++l->st_.wakeups;  // spurious (e.g. the mailbox was dropped by quiesce)
  }
}

void LoopAccess::onVsync(RenderLoop* l, std::int64_t t) {
  RenderLoop::Impl* im = l->impl_;
  im->armed = false;
  {
    std::lock_guard<std::mutex> g(l->m_);
    ++l->st_.vsyncs;
  }
  if (im->tick++ % unsigned(l->divisor_.load()) != 0) {
    {
      std::lock_guard<std::mutex> g(l->m_);
      ++l->st_.skippedByCap;
    }
    arm(im, l);
    return;
  }
  Taken tk = take(l->m_, l->full_, l->box_, l->rendering_, l->paused_);
  if (!tk.ok) return;
  l->fn_(std::move(tk.s), t);
  bool again;
  {
    std::lock_guard<std::mutex> g(l->m_);
    ++l->st_.renders;
    l->rendering_ = false;
    again = l->full_ && !l->stop_ && !l->paused_.load();
  }
  l->idleCv_.notify_all();
  if (again) arm(im, l);
}

static void vsyncCb(std::int64_t t, void* data) { LoopAccess::onVsync(static_cast<RenderLoop*>(data), t); }
static int wakeCb(int, int, void* data) {
  LoopAccess::onWake(static_cast<RenderLoop*>(data));
  return 1;
}
#endif

}  // namespace dapple::present
