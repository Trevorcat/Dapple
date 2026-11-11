#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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
    if (threads_ == 0 || n <= chunk || inJob_) {
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
      pending_ = threads_;
      ++generation_;
    }
    cvWork_.notify_all();
    work();
    std::unique_lock<std::mutex> lock(mtx_);
    cvDone_.wait(lock, [&] { return pending_ == 0; });
    job_ = nullptr;
  }

 private:
  ThreadPool() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    threads_ = int(hw) - 1;
    for (int i = 0; i < threads_; ++i) std::thread([this] { loop(); }).detach();
  }

  void work() {
    const bool was = inJob_;
    inJob_ = true;
    for (;;) {
      const int start = next_.fetch_add(chunk_);
      if (start >= total_) break;
      (*job_)(start, std::min(total_, start + chunk_));
    }
    inJob_ = was;
  }

  void loop() {
    unsigned seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mtx_);
        cvWork_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
      }
      work();
      std::lock_guard<std::mutex> lock(mtx_);
      if (--pending_ == 0) cvDone_.notify_one();
    }
  }

  static inline thread_local bool inJob_ = false;
  std::mutex submit_, mtx_;
  std::condition_variable cvWork_, cvDone_;
  const std::function<void(int, int)>* job_ = nullptr;
  std::atomic<int> next_{0};
  int total_ = 0, chunk_ = 1, pending_ = 0, threads_ = 0;
  unsigned generation_ = 0;
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
