#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "presenter.h"

namespace dapple::present {
namespace {
class NullPresenter final : public Presenter {
 public:
  explicit NullPresenter(bool external) : external_(external) {}
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
  // test double of the zero-readback input: hands out a dummy image so the view takes the external path (the frame has to come
  // back as pixels from a CPU session, which is exactly the fallback the tests look at)
  bool beginExternal(int w, int h, ExternalTarget* out, std::string& err) override {
    if (!external_) return err = "not supported", false;
    std::lock_guard<std::mutex> l(m_);
    if (!win_) return err = "no window", false;
    out->image = &dummy_;
    out->w = std::uint32_t(w);
    out->h = std::uint32_t(h);
    return true;
  }
  bool presentExternal(int w, int h, PresentTiming*, std::string& err) override {
    std::lock_guard<std::mutex> l(m_);
    if (!win_) return err = "no window", false;
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
  const bool external_;
  int dummy_ = 0;
  int w_ = 0, h_ = 0, lw_ = 0, lh_ = 0;
  std::vector<std::uint8_t> last_;
  std::atomic<std::uint64_t> n_{0};
};
}  // namespace

std::unique_ptr<Presenter> makeNullPresenter(bool external) { return std::make_unique<NullPresenter>(external); }

#if !defined(__ANDROID__)
std::unique_ptr<Presenter> makeVulkanPresenter(std::string& err) {
  err = "Vulkan swapchain presenter is Android only";
  return nullptr;
}
#endif
}  // namespace dapple::present
