#include "dapple/lut.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "file_util.h"

namespace dapple {

Result<Lut3D> Lut3D::loadCube(const std::filesystem::path& path) {
  auto text = readTextFile(path);
  if (!text) return std::unexpected(text.error());
  auto lut = parseCube(*text);
  if (!lut) return fail(pathToUtf8(path.filename()) + ": " + lut.error().message);
  if (lut->title_.empty()) lut->title_ = pathToUtf8(path.stem());
  return lut;
}

Result<Lut3D> Lut3D::parseCube(const std::string& text) {
  Lut3D lut;
  std::istringstream in(text);
  std::string line;
  std::size_t expected = 0;
  lut.table_.clear();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    std::istringstream ls(line.substr(first));
    std::string key;
    ls >> key;
    if (key == "TITLE") {
      const auto q0 = line.find('"'), q1 = line.rfind('"');
      if (q0 != std::string::npos && q1 > q0) lut.title_ = line.substr(q0 + 1, q1 - q0 - 1);
    } else if (key == "LUT_3D_SIZE") {
      ls >> lut.size_;
      if (lut.size_ < 2 || lut.size_ > 256) return fail("invalid LUT_3D_SIZE");
      expected = std::size_t(lut.size_) * lut.size_ * lut.size_;
      lut.table_.reserve(expected * 3);
    } else if (key == "LUT_1D_SIZE") {
      return fail("1D LUTs are not supported");
    } else if (key == "DOMAIN_MIN") {
      ls >> lut.domainMin_[0] >> lut.domainMin_[1] >> lut.domainMin_[2];
    } else if (key == "DOMAIN_MAX") {
      ls >> lut.domainMax_[0] >> lut.domainMax_[1] >> lut.domainMax_[2];
    } else if (key == "LUT_3D_INPUT_RANGE") {
      float lo = 0, hi = 1;
      ls >> lo >> hi;
      std::fill(std::begin(lut.domainMin_), std::end(lut.domainMin_), lo);
      std::fill(std::begin(lut.domainMax_), std::end(lut.domainMax_), hi);
    } else if ((key[0] >= '0' && key[0] <= '9') || key[0] == '-' || key[0] == '.' || key[0] == '+') {
      std::istringstream vs(line.substr(first));
      float r, g, b;
      if (!(vs >> r >> g >> b)) return fail("malformed data line: " + line);
      lut.table_.insert(lut.table_.end(), {r, g, b});
    }
    // Unknown keywords are ignored, as most readers do.
  }
  if (lut.size_ == 0) return fail("missing LUT_3D_SIZE");
  if (lut.table_.size() != expected * 3)
    return fail("expected " + std::to_string(expected) + " entries, got " + std::to_string(lut.table_.size() / 3));
  lut.finalize();
  return lut;
}

void Lut3D::finalize() {
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the bytes of size, domain and table
  auto mix = [&](const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(&size_, sizeof size_);
  mix(domainMin_, sizeof domainMin_);
  mix(domainMax_, sizeof domainMax_);
  mix(table_.data(), table_.size() * sizeof(float));
  contentId_ = h ? h : 1;  // 0 is "no LUT"
}

Lut3D Lut3D::identity(int size) {
  Lut3D lut;
  lut.size_ = size;
  lut.title_ = "identity";
  lut.table_.reserve(std::size_t(size) * size * size * 3);
  for (int b = 0; b < size; ++b)
    for (int g = 0; g < size; ++g)
      for (int r = 0; r < size; ++r)
        lut.table_.insert(lut.table_.end(), {r / float(size - 1), g / float(size - 1), b / float(size - 1)});
  lut.finalize();
  return lut;
}

void Lut3D::apply(const float in[3], float out[3]) const {
  const float n = float(size_ - 1);
  float f[3];
  int i0[3], i1[3];
  for (int c = 0; c < 3; ++c) {
    float v = (in[c] - domainMin_[c]) / (domainMax_[c] - domainMin_[c]);
    v = std::clamp(v, 0.f, 1.f) * n;
    i0[c] = std::min(int(v), size_ - 2);
    f[c] = v - i0[c];
    i1[c] = i0[c] + 1;
  }
  const float fr = f[0], fg = f[1], fb = f[2];
  const float* c000 = at(i0[0], i0[1], i0[2]);
  const float* c111 = at(i1[0], i1[1], i1[2]);
  // Tetrahedral interpolation: pick the tetrahedron containing the point.
  const float *a, *b;
  float wa, wb, w0, w1;
  if (fr > fg) {
    if (fg > fb) {        // r > g > b
      a = at(i1[0], i0[1], i0[2]); b = at(i1[0], i1[1], i0[2]);
      w0 = 1 - fr; wa = fr - fg; wb = fg - fb; w1 = fb;
    } else if (fr > fb) { // r > b >= g
      a = at(i1[0], i0[1], i0[2]); b = at(i1[0], i0[1], i1[2]);
      w0 = 1 - fr; wa = fr - fb; wb = fb - fg; w1 = fg;
    } else {              // b >= r > g
      a = at(i0[0], i0[1], i1[2]); b = at(i1[0], i0[1], i1[2]);
      w0 = 1 - fb; wa = fb - fr; wb = fr - fg; w1 = fg;
    }
  } else {
    if (fb > fg) {        // b > g >= r
      a = at(i0[0], i0[1], i1[2]); b = at(i0[0], i1[1], i1[2]);
      w0 = 1 - fb; wa = fb - fg; wb = fg - fr; w1 = fr;
    } else if (fb > fr) { // g >= b > r
      a = at(i0[0], i1[1], i0[2]); b = at(i0[0], i1[1], i1[2]);
      w0 = 1 - fg; wa = fg - fb; wb = fb - fr; w1 = fr;
    } else {              // g >= r >= b
      a = at(i0[0], i1[1], i0[2]); b = at(i1[0], i1[1], i0[2]);
      w0 = 1 - fg; wa = fg - fr; wb = fr - fb; w1 = fb;
    }
  }
  for (int c = 0; c < 3; ++c) out[c] = w0 * c000[c] + wa * a[c] + wb * b[c] + w1 * c111[c];
}

}  // namespace dapple
