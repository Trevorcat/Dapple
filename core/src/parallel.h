#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
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
  static ThreadPool& instance() {
    static ThreadPool* pool = new ThreadPool;
    return *pool;
  }

  // Calls fn(begin, end) over disjoint ranges covering [0, n), in chunks of `chunk` items.
  void run(int n, int chunk, const std::function<void(int, int)>& fn) {
    if (n <= 0) return;
    chunk = std::max(1, chunk);
    // background work (prefetch, detail session, previews) never takes the pool: it runs on the caller's own
    // low priority thread so an interactive frame is never queued behind it
    const int active = std::min(threads_, limit_.load(std::memory_order_relaxed) - 1);
    if (active <= 0 || n <= chunk || inJob_ || sys::inBackground()) {
      fn(0, n);
      return;
    }
    std::lock_guard<std::mutex> submit(submit_);
    {
      std::lock_guard<std::mutex> lock(mtx_);
      job_ = &fn;
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
    work();
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
  ThreadPool() {
    // big.LITTLE: the performance cores take part (workers pinned to them) when there are enough of them,
    // otherwise all cpus (see sys::poolCpus)
    const sys::Topology& topo = sys::topology();
    int n = int(sys::poolCpus(topo).size());
    if (n <= 0) n = int(std::max(1u, std::thread::hardware_concurrency()));
    if (const char* env = std::getenv("DAPPLE_THREADS")) n = std::max(1, std::atoi(env));
    if (const char* env = std::getenv("DAPPLE_SPIN_US")) spinUs_ = std::clamp(std::atoi(env), 0, 1000);
    threads_ = n - 1;
    limit_.store(n);
    for (int i = 0; i < threads_; ++i) std::thread([this, i] { loop(i); }).detach();
  }

  void work() {
    const bool was = inJob_;
    inJob_ = true;
    for (;;) {
      const int start = next_.fetch_add(chunk_);
      if (start >= total_) break;
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
    unsigned seen = 0;
    if (!std::getenv("DAPPLE_THREADS")) sys::setAffinity(sys::poolCpus(sys::topology()));
    sys::setInteractivePriority();
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
        const auto spinEnd = std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs_);
        int k = 0;
        while (gen_.load(std::memory_order_acquire) == seen) {
          if ((++k & 63) == 0 && std::chrono::steady_clock::now() > spinEnd) break;
          sys::cpuRelax();
        }
        std::unique_lock<std::mutex> lock(mtx_);
        cvWork_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        mine = idx < active_;
      }
      if (!mine) continue;  // this job runs with fewer threads than the pool has
      work();
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
  int total_ = 0, chunk_ = 1, pending_ = 0, threads_ = 0;
  unsigned generation_ = 0;
  std::atomic<unsigned> gen_{0};      // mirror of generation_ for the lock-free spin
  std::atomic<int> pendingA_{0};      // mirror of pending_ for the caller's spin
  int spinUs_ = 60;  // bounded spin before blocking (one pass gap is typically 5-50 us); env DAPPLE_SPIN_US
  int active_ = 0;
  std::atomic<int> limit_{1};
  std::vector<int> tids_;

 public:
  // Total threads (caller included) a job may use; 0 / out of range = all.
  void setLimit(int n) { limit_.store(n <= 0 ? threads_ + 1 : std::min(n, threads_ + 1)); }
  int size() const { return threads_ + 1; }
  std::vector<int> workerTids() {
    std::lock_guard<std::mutex> lock(mtx_);
    return tids_;
  }
};

// Runs fn(i) for every i in [0, n) across all hardware threads.
template <class Fn>
void parallelFor(int n, Fn&& fn, int chunk = 1) {
  ThreadPool::instance().run(n, chunk, [&](int a, int b) {
    for (int i = a; i < b; ++i) fn(i);
  });
}

// Runs fn(y) for every row in [0, rows) across all hardware threads.
template <class Fn>
void parallelRows(int rows, Fn&& fn) {
  parallelFor(rows, fn, 8);
}

}  // namespace dapple
