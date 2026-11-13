#pragma once
// Runtime backends behind ai::Model. Internal: the engine never includes this.
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ai/ai.h"

namespace dapple::ai {

struct SessionArgs {
  std::string path;            // file path ("" when memory)
  const void* data = nullptr;  // memory model
  std::size_t size = 0;
  int threads = 1;
  std::vector<int> cpus;       // cpus the pool should live on (inherited by threads created during session creation); empty = no pinning
};

class Session {
 public:
  virtual ~Session() = default;
  virtual const char* runtime() const = 0;
  virtual std::vector<TensorSpec> inputs() const = 0;
  virtual std::vector<TensorSpec> outputs() const = 0;
  virtual Status run(const std::vector<TensorView>& in, std::vector<Tensor>& out, const std::atomic<bool>* cancel, const std::vector<int>& wantOutputs) = 0;
};

// Return nullptr + status when the runtime is not compiled in or the model is refused.
std::unique_ptr<Session> makeLiteRtSession(const SessionArgs&, Status& st);
std::unique_ptr<Session> makeOrtSession(const SessionArgs&, Status& st);

// Polls a cancel flag on a helper thread and calls `fire` once when it is set. Costs one sleeping thread per run, only when a flag is given.
class CancelWatcher {
 public:
  template <class F>
  CancelWatcher(const std::atomic<bool>* flag, F fire) {
    if (!flag) return;
    th_ = std::thread([this, flag, fire]() {
      while (!stop_.load(std::memory_order_acquire)) {
        if (flag->load(std::memory_order_relaxed)) { fire(); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }
  ~CancelWatcher() {
    stop_.store(true, std::memory_order_release);
    if (th_.joinable()) th_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread th_;
};

}  // namespace dapple::ai
