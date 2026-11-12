#pragma once
// Stage (d): the per-view render loop. One thread, a one-slot mailbox (latest wins), vsync-aligned frame starts,
// zero work when idle (the thread blocks; no timers, no polling).
//
//   submit(snapshot)   any thread, wait-free for the caller: overwrites the mailbox (the overwritten snapshot counts as dropped)
//   the loop thread    wakes on the first submit, waits for the next vsync (AChoreographer on Android; a fixed-rate
//                      grid elsewhere), takes the mailbox, calls the render function once, and re-arms only when the
//                      mailbox filled up again during the frame.
//
// The render function owns what a frame is (render + present). The loop owns when.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dapple::present {

struct LoopAccess;
struct Snapshot {
  std::uint64_t seq = 0;        // assigned by submit()
  std::string payload;          // params JSON (opaque to the loop)
  std::string lutPath;
  std::string activeParam;      // key of the slider being dragged (DappleFrameRequest::active_param); empty = none
  std::int32_t longEdge = 0, flags = 0, bakeSize = 0;
  std::int64_t submitNs = 0;    // steady clock, for end-to-end latency
};

struct LoopStats {
  std::uint64_t submits = 0, renders = 0, dropped = 0;  // dropped = snapshots overwritten before being rendered
  std::uint64_t wakeups = 0;    // thread wakeups that found nothing to do (must stay 0 in steady state)
  std::uint64_t vsyncs = 0;     // vsync callbacks taken
  std::uint64_t skippedByCap = 0;
};

class RenderLoop {
 public:
  using RenderFn = std::function<void(Snapshot&&, std::int64_t vsyncNs)>;
  // useVsync: Choreographer on Android; false (or elsewhere) = timer grid of `periodNs` (tests / desktop).
  RenderLoop(RenderFn fn, bool useVsync = true, std::int64_t periodNs = 16666667);
  ~RenderLoop();
  RenderLoop(const RenderLoop&) = delete;
  RenderLoop& operator=(const RenderLoop&) = delete;

  std::uint64_t submit(Snapshot&& s);
  // Block until no frame is being rendered and none will start (mailbox cleared when `drop`). Safe to call from any
  // thread except the loop thread; used before releasing resources the render function touches.
  void quiesce(bool drop);
  void pause(bool paused);           // while paused the mailbox keeps the latest snapshot but nothing renders
  bool paused() const { return paused_.load(); }
  // Frame-rate cap: render every n-th vsync (1 = every vsync). Thermal / low-battery policy.
  void setDivisor(int n) { divisor_.store(n < 1 ? 1 : n); }
  bool hasPending() const;
  LoopStats stats() const;
  std::int64_t periodNs() const { return period_.load(); }

 private:
  void threadMain();
 public:
  struct Impl;  // platform state, opaque

 private:
  Impl* impl_ = nullptr;

  RenderFn fn_;
  const bool useVsync_;
  std::atomic<std::int64_t> period_;
  mutable std::mutex m_;
  std::condition_variable cv_, idleCv_;
  Snapshot box_;
  bool full_ = false, rendering_ = false, stop_ = false;
  std::atomic<bool> paused_{false};
  std::atomic<int> divisor_{1};
  std::uint64_t seq_ = 0;
  LoopStats st_;
  std::thread th_;
  friend struct LoopAccess;
};

std::int64_t steadyNowNs();

}  // namespace dapple::present
