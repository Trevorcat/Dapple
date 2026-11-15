#pragma once
// Recycled float frames of the point family's CPU binding and the film stage: a fresh 20 MB buffer costs ~5 ms of page faults.
#include <algorithm>
#include <mutex>
#include <vector>

#include "dapple/image.h"

namespace dapple::engine::point {

// Working frames (film input / float output of a frame) are recycled between renders: a fresh 20 MB buffer costs
// ~5 ms of page faults, more than the whole film run of a preview. Big frames (> 8 MP) are not kept.
class ScratchImage {
 public:
  ScratchImage() = default;
  ScratchImage(const ScratchImage&) = delete;
  ScratchImage& operator=(const ScratchImage&) = delete;
  ~ScratchImage() {
    if (img_.data.capacity() == 0 || img_.data.capacity() > kKeepFloats) return;
    std::lock_guard<std::mutex> lock(mtx());
    auto& s = store();
    s.push_back(std::move(img_));
    if (s.size() > 3) {
      auto smallest = std::min_element(s.begin(), s.end(), [](const ImageF& a, const ImageF& b) {
        return a.data.capacity() < b.data.capacity();
      });
      s.erase(smallest);
    }
  }
  // Frame of w x h; the pixel values are undefined (callers overwrite every pixel).
  ImageF& get(int w, int h) {
    const std::size_t need = std::size_t(w) * h * 3;
    {
      std::lock_guard<std::mutex> lock(mtx());
      auto& s = store();
      int best = -1;
      for (int i = 0; i < int(s.size()); ++i)
        if (s[i].data.capacity() >= need && (best < 0 || s[i].data.capacity() < s[best].data.capacity())) best = i;
      if (best >= 0) {
        img_ = std::move(s[best]);
        s.erase(s.begin() + best);
      }
    }
    img_.width = w;
    img_.height = h;
    img_.data.resize(need);
    return img_;
  }

 private:
  static constexpr std::size_t kKeepFloats = std::size_t(8'000'000) * 3;
  static std::mutex& mtx() {
    static std::mutex* m = new std::mutex;
    return *m;
  }
  static std::vector<ImageF>& store() {
    static std::vector<ImageF>* s = new std::vector<ImageF>;
    return *s;
  }
  ImageF img_;
};

}  // namespace dapple::engine::point
