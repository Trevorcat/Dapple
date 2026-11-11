#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "dapple/result.h"

namespace dapple {

// 3D colour lookup table (the .cube format used by Photoshop, Resolve,
// Premiere and most filter packs). Input/output are display-encoded sRGB.
class Lut3D {
 public:
  static Result<Lut3D> loadCube(const std::filesystem::path& path);
  static Result<Lut3D> parseCube(const std::string& text);
  static Lut3D identity(int size);

  int size() const { return size_; }
  const std::string& title() const { return title_; }

  // Tetrahedral interpolation; rgb in/out, values clamped to the domain.
  void apply(const float in[3], float out[3]) const;

 private:
  int size_ = 0;
  std::string title_;
  float domainMin_[3] = {0, 0, 0};
  float domainMax_[3] = {1, 1, 1};
  std::vector<float> table_;  // r fastest, then g, then b (cube convention)
  const float* at(int r, int g, int b) const {
    return &table_[3 * (std::size_t(b) * size_ * size_ + std::size_t(g) * size_ + r)];
  }
};

}  // namespace dapple
