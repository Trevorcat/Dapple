#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "presenter.h"

namespace dapple::present {
namespace {
class NullPresenter final : public Presenter {
 public:
  bool setWindow(void* window, int w, int h, std::string&) override {
    std::lock_guard<std::mutex> l(m_);
    win_ = window;
    w_ = w;
    h_ = h;
    return true;
  }
  void clearWindow() override {
    std::lock_guard<std::mutex> l(m_);
    win_ = nullptr;
  }
  bool hasWindow() const override {
    std::lock_guard<std::mutex> l(m_);
    return win_ != nullptr;
  }
  bool present(const std::uint8_t* rgba, int w, int h, PresentTiming*, std::string& err) override {
    std::lock_guard<std::mutex> l(m_);
    if (!win_) return err = "no window", false;
    last_.assign(rgba, rgba + std::size_t(w) * std::size_t(h) * 4);
    lw_ = w;
    lh_ = h;
    ++n_;
    return true;
  }
  void trim(int) override {
    std::lock_guard<std::mutex> l(m_);
    last_.clear();
    last_.shrink_to_fit();
  }
  std::string info() const override { return "{\"variant\":\"null\"}"; }
  std::uint64_t presented() const override { return n_.load(); }

 private:
  mutable std::mutex m_;
  void* win_ = nullptr;
  int w_ = 0, h_ = 0, lw_ = 0, lh_ = 0;
  std::vector<std::uint8_t> last_;
  std::atomic<std::uint64_t> n_{0};
};
}  // namespace

std::unique_ptr<Presenter> makeNullPresenter() { return std::make_unique<NullPresenter>(); }

#if !defined(__ANDROID__)
std::unique_ptr<Presenter> makeVulkanPresenter(std::string& err) {
  err = "Vulkan swapchain presenter is Android only";
  return nullptr;
}
#endif
}  // namespace dapple::present
