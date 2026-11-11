#pragma once
// Shared helpers of `dapple eval` and `dapple fit-profile`: sample selection,
// exact-size area resampling, and a disk cache of decoded RAW + camera JPEG
// pairs (so the 0.8 s/file RAW decode is paid once).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dapple/decoder.h"
#include "dapple/developer.h"
#include "dapple/image.h"

namespace dapple::evaltool {

namespace fs = std::filesystem;

inline std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// All ARW files under `dir`, sorted by UTF-8 path (stable sample selection).
inline std::vector<fs::path> listArw(const fs::path& dir) {
  std::vector<fs::path> v;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    std::string e = u8(it->path().extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (e == ".arw") v.push_back(it->path());
  }
  std::sort(v.begin(), v.end(), [](const fs::path& a, const fs::path& b) { return u8(a) < u8(b); });
  return v;
}

// Area-average resample to exactly dw x dh (fractional edge weights).
ImageF resizeExact(const ImageF& src, int dw, int dh);

// Geometric registration of the camera JPEG to the RAW render. The in-camera JPEG engine applies
// lens corrections (distortion / shading) and the preview is not framed exactly like the decoded
// RAW, so the two differ by a small, lens dependent zoom (up to ~3%), a radial term and a shift.
// Model: a pixel p of our render (relative to the centre c, u = |p-c| / half-diagonal) is found in
// the camera image at c + (p-c) * (1 + k u^2) / scale + t.
struct Alignment {
  float scale = 1.f, k = 0.f, tx = 0.f, ty = 0.f;  // t in pixels of the case grid
  float ncc0 = 0.f;   // normalised cross-correlation of high-passed luma, identity mapping
  float ncc = 0.f;    // after registration
  int margin = 0;     // border (px of the case grid) where the camera image has no data
};

struct Case {
  std::string name;
  SourceImage src;  // linear ProPhoto at `edge` long edge, camera resolved via src.meta
  ImageF cam;       // camera JPEG warped onto our framing, sRGB-encoded 0..1, same size as src.linear
  Alignment align;
};

// Finds the registration of `cam` to `ours` (both sRGB-encoded, same size).
Alignment alignCamera(const ImageF& ours, const ImageF& cam);
ImageF warpCamera(const ImageF& cam, const Alignment& a);

// Decodes (or loads from cache) one RAW + its embedded JPEG at `edge` long edge.
bool loadCase(const Developer& dev, const fs::path& raw, int edge, const fs::path& cacheDir, Case& out,
              std::string& err);

// Loads many cases with a few worker threads. Failed files are skipped (reported on stderr).
std::vector<Case> loadCases(const Developer& dev, const std::vector<fs::path>& files, int edge,
                            const fs::path& cacheDir, int workers = 4);

}  // namespace dapple::evaltool
