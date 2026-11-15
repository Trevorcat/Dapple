#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "sysperf.h"

namespace dapple {

// A tiny persistent thread pool (spawning threads per pass costs ~1 ms on Windows, and the
// renderer runs a dozen passes). The pool is leaked on purpose: joining threads from a static
// destructor deadlocks when the library is unloaded on Windows. Jobs are serialised; a job started
// from inside a job runs inline on the calling thread.
class ThreadPool {
 public:
  // Latest-wins cancellation (docs/engine/pipeline-architecture.md 5.8): while a CancelScope is alive on the submitting
  // thread, a job it submits stops handing out chunks as soon as *cur != mine. The caller must treat the output of such a
  // job as invalid (it checks the same token afterwards).
  struct CancelRef {
    const std::atomic<std::uint64_t>* cur = nullptr;
    std::uint64_t mine = 0;
  };
  static CancelRef& tlCancel() {
    static thread_local CancelRef c;
    return c;
  }
  static ThreadPool& instance() {
    static ThreadPool* pool = new ThreadPool;
    return *pool;
  }
  // A second, smaller pool for background table baking (wave 8): jobs of the frame thread never queue behind a bake and the other way round.
  // Half of the pool cpus (at least 2 threads in total), same capability-chosen cores, normal scheduler priority.
  static ThreadPool& background() {
    static ThreadPool* pool = new ThreadPool(true);
    return *pool;
  }
  // The pool parallelFor uses on this thread (null = the main pool); set by background workers.
  static ThreadPool*& tlUse() {
    static thread_local ThreadPool* p = nullptr;
    return p;
  }

  // Calls fn(begin, end) over disjoint ranges covering [0, n), in chunks of `chunk` items.
  void run(int n, int chunk, const std::function<void(int, int)>& fn) {
    if (n <= 0) return;
    chunk = std::max(1, chunk);
    // background work (prefetch, the zoom detail session, previews) never takes the frame pool, so an interactive frame is never queued
    // behind it: while no finger is on a control it runs on the background pool (the table bakes' pool, half the cpus, its own submit
    // lock), else on the caller's own low priority thread. A settled zoomed frame on a phone without the GPU path took 4 s on one
    // efficiency core (docs/engine/pipeline-architecture.md 43.11).
    if (sys::inBackground()) {
      static const bool serial = std::getenv("DAPPLE_BG_SERIAL") != nullptr;  // the old policy (benchmarks: one thread)
      if (!bg_ && !inJob_ && n > chunk && !serial && !sys::interacting()) {
        background().runJob(n, chunk, fn, true);
        return;
      }
      fn(0, n);
      return;
    }
    runJob(n, chunk, fn);
  }

 private:
  // yieldOnTouch: a background frame's job: the workers stop taking chunks as soon as a finger is on a control (the caller finishes it)
  void runJob(int n, int chunk, const std::function<void(int, int)>& fn, bool yieldOnTouch = false) {
    const int active = std::min(threads_, limit_.load(std::memory_order_relaxed) - 1);
    if (active <= 0 || n <= chunk || inJob_) {
      fn(0, n);
      return;
    }
    std::lock_guard<std::mutex> submit(submit_);
    cancelCur_.store(tlCancel().cur, std::memory_order_relaxed);
    cancelMine_.store(tlCancel().mine, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mtx_);
      job_ = &fn;
      yield_ = yieldOnTouch;
      total_ = n;
      chunk_ = chunk;
      next_.store(0);
      pending_ = active;
      pendingA_.store(active, std::memory_order_relaxed);
      active_ = active;
      ++generation_;
      gen_.store(generation_, std::memory_order_release);
    }
    cvWork_.notify_all();
    work(false);
    // the other threads are finishing their last chunks: spin briefly before sleeping on the condition variable
    const auto spinEnd = std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs_);
    for (int k = 0; pendingA_.load(std::memory_order_acquire) != 0; ) {
      if ((++k & 63) == 0 && std::chrono::steady_clock::now() > spinEnd) break;
      sys::cpuRelax();
    }
    std::unique_lock<std::mutex> lock(mtx_);
    cvDone_.wait(lock, [&] { return pending_ == 0; });
    job_ = nullptr;
    // an exception thrown by fn on any thread is rethrown here, on the caller, once every worker is done
    // (never out of a worker: that would terminate the process)
    if (err_) {
      std::exception_ptr e = std::move(err_);
      err_ = nullptr;
      lock.unlock();
      std::rethrow_exception(e);
    }
  }

 private:
  explicit ThreadPool(bool background = false) : bg_(background) {
    // big.LITTLE: the performance cores take part (workers pinned to them) when there are enough of them,
    // otherwise all cpus (see sys::poolCpus)
    const sys::Topology& topo = sys::topology();
    int n = int(sys::poolCpus(topo).size());
    if (n <= 0) n = int(std::max(1u, std::thread::hardware_concurrency()));
    if (const char* env = std::getenv("DAPPLE_THREADS")) n = std::max(1, std::atoi(env));
    if (const char* env = std::getenv("DAPPLE_SPIN_US")) spinUs_ = std::clamp(std::atoi(env), 0, 1000);
    if (bg_) n = std::max(2, n / 2);
    threads_ = n - 1;
    limit_.store(n);
    for (int i = 0; i < threads_; ++i)
      std::thread([this, i] {
        if (bg_) sys::utilityQos();  // the background pool (table bakes) works on the efficiency cores under load on Apple
        loop(i);
      }).detach();
  }

  void work(bool worker) {
    const bool was = inJob_;
    inJob_ = true;
    for (;;) {
      if (worker && yield_ && sys::interacting()) break;  // the caller takes the remaining chunks
      const int start = next_.fetch_add(chunk_);
      if (start >= total_) break;
      if (const auto* cc = cancelCur_.load(std::memory_order_relaxed)) {
        if (cc->load(std::memory_order_relaxed) != cancelMine_.load(std::memory_order_relaxed)) {
          next_.store(total_);  // cancelled: abandon the remaining chunks
          break;
        }
      }
      try {
        (*job_)(start, std::min(total_, start + chunk_));
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(mtx_);
          if (!err_) err_ = std::current_exception();
        }
        next_.store(total_);  // abandon the remaining chunks
        break;
      }
    }
    inJob_ = was;
  }

  void loop(int idx) {
    unsigned seen = 0, warmSeen = 0;
    if (!std::getenv("DAPPLE_THREADS")) sys::setAffinity(sys::poolCpus(sys::topology()));
    if (!bg_) sys::setInteractivePriority();
    {
      std::lock_guard<std::mutex> lock(mtx_);
      tids_.push_back(sys::gettid_());
    }
    for (;;) {
      bool mine;
      {
        // A frame is a dozen passes back to back: spin briefly (bounded, spinUs_) for the next pass so the
        // workers are not put to sleep and woken (a futex round trip per pass, far slower in a VM or when the
        // core drops to a low clock), then block on the condition variable: an idle pool costs no CPU.
        // While a WarmScope is open the spin lasts up to kWarmSpinUs (the next job of the scope starts on a running worker); past the normal
        // spin the worker yields its cpu at every check, so a thread that is ready to run there (the frame thread, a driver thread) is not
        // kept waiting by a spinning worker when the cpus are oversubscribed.
        const auto spin0 = std::chrono::steady_clock::now();
        const auto spinEnd = spin0 + std::chrono::microseconds(spinUs_), warmEnd = spin0 + std::chrono::microseconds(kWarmSpinUs);
        int k = 0;
        while (gen_.load(std::memory_order_acquire) == seen) {
          if ((++k & 63) == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now > spinEnd) {
              if (warm_.load(std::memory_order_relaxed) <= 0 || now > warmEnd) break;
              std::this_thread::yield();
            }
          }
          sys::cpuRelax();
        }
        std::unique_lock<std::mutex> lock(mtx_);
        cvWork_.wait(lock, [&] { return generation_ != seen || warmGen_ != warmSeen; });
        warmSeen = warmGen_;
        if (generation_ == seen) continue;  // woken by a WarmScope: spin until its job comes (or the scope closes)
        seen = generation_;
        mine = idx < active_;
      }
      if (!mine) continue;  // this job runs with fewer threads than the pool has
      work(true);
      std::lock_guard<std::mutex> lock(mtx_);
      pendingA_.fetch_sub(1, std::memory_order_release);
      if (--pending_ == 0) cvDone_.notify_one();
    }
  }

  static inline thread_local bool inJob_ = false;
  std::mutex submit_, mtx_;
  std::condition_variable cvWork_, cvDone_;
  const std::function<void(int, int)>* job_ = nullptr;
  std::exception_ptr err_;  // first exception of the running job (guarded by mtx_)
  std::atomic<int> next_{0};
  std::atomic<const std::atomic<std::uint64_t>*> cancelCur_{nullptr};
  std::atomic<std::uint64_t> cancelMine_{0};
  int total_ = 0, chunk_ = 1, pending_ = 0, threads_ = 0;
  bool yield_ = false;  // set under mtx_ before the generation bump; workers read it after waking (mtx_ / gen_ acquire)
  unsigned generation_ = 0;
  std::atomic<unsigned> gen_{0};      // mirror of generation_ for the lock-free spin
  std::atomic<int> pendingA_{0};      // mirror of pending_ for the caller's spin
  int spinUs_ = 60;  // bounded spin before blocking (one pass gap is typically 5-50 us); env DAPPLE_SPIN_US
  std::atomic<int> warm_{0};  // open WarmScopes: the workers keep spinning between jobs
  static constexpr int kWarmSpinUs = 2000;  // longest idle spin inside a WarmScope (a table stage's serial gaps are shorter)
  unsigned warmGen_ = 0;      // bumped (under mtx_) when a WarmScope opens: wakes sleeping workers into the spin
  int active_ = 0;
  bool bg_ = false;
  std::atomic<int> limit_{1};
  std::vector<int> tids_;

 public:
  // A stretch of the calling thread with several short jobs separated by serial work (an interactive frame's table stage): the sleeping
  // workers are woken now, while the caller still works alone, and keep spinning between jobs (up to kWarmSpinUs idle) until the scope closes. On a phone a worker
  // woken from an idle core takes milliseconds to start and the job waits for it; a warm worker joins at once. Nests; jobs from inside a
  // job and background threads are unaffected.
  class WarmScope {
   public:
    explicit WarmScope(ThreadPool& p) : p_(p) { p_.warmBegin(); }
    ~WarmScope() { p_.warm_.fetch_sub(1, std::memory_order_relaxed); }
    WarmScope(const WarmScope&) = delete;
    WarmScope& operator=(const WarmScope&) = delete;

   private:
    ThreadPool& p_;
  };
  void warmBegin() {
    if (threads_ <= 0) return void(warm_.fetch_add(1, std::memory_order_relaxed));
    {
      std::lock_guard<std::mutex> lock(mtx_);
      warm_.fetch_add(1, std::memory_order_relaxed);
      ++warmGen_;
    }
    cvWork_.notify_all();
  }
  // Total threads (caller included) a job may use; 0 / out of range = all.
  void setLimit(int n) { limit_.store(n <= 0 ? threads_ + 1 : std::min(n, threads_ + 1)); }
  int size() const { return threads_ + 1; }
  int limit() const { return limit_.load(std::memory_order_relaxed); }  // threads (caller included) a job may use now
  std::vector<int> workerTids() {
    std::lock_guard<std::mutex> lock(mtx_);
    return tids_;
  }
};

// Scopes the cancellation token of the jobs this thread submits (see ThreadPool::CancelRef).
class CancelScope {
 public:
  CancelScope(const std::atomic<std::uint64_t>* cur, std::uint64_t mine) : prev_(ThreadPool::tlCancel()) {
    ThreadPool::tlCancel() = {cur, mine};
  }
  ~CancelScope() { ThreadPool::tlCancel() = prev_; }
  CancelScope(const CancelScope&) = delete;
  CancelScope& operator=(const CancelScope&) = delete;

 private:
  ThreadPool::CancelRef prev_;
};

// Runs fn(i) for every i in [0, n) across all hardware threads.
template <class Fn>
void parallelFor(int n, Fn&& fn, int chunk = 1) {
  (ThreadPool::tlUse() ? *ThreadPool::tlUse() : ThreadPool::instance()).run(n, chunk, [&](int a, int b) {
    for (int i = a; i < b; ++i) fn(i);
  });
}

// Runs fn(y) for every row in [0, rows) across all hardware threads.
template <class Fn>
void parallelRows(int rows, Fn&& fn) {
  parallelFor(rows, fn, 8);
}

}  // namespace dapple
