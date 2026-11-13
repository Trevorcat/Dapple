#pragma once
// Demosaic node: portable CPU implementation (docs/engine/pipeline-architecture.md section 32). The same arithmetic runs in the
// compute shaders core/engine/shaders/demosaic_*.comp, so a GPU device and a CPU device agree within the usual parity budget.
//
//   raw 16-bit mosaic --scale (black, white, WB, clip)--> float plane --green plane--> R, G, B --3x3--> linear working RGB
//
// Bayer:  gradient-weighted Hamilton-Adams green (two directions, discriminator over a 3 x 3 neighbourhood), colour-difference chroma
//         (diagonal gradient-weighted at the opposite colour, plain average at green). Halo 4.
// X-Trans: green from the four directions (H, V, two diagonals) through the nearest greens of the 6 x 6 pattern, red / blue as the
//         green-guided weighted average of the colour differences of the 5 x 5 neighbourhood. Halo 6.
// Both are written from published descriptions (Hamilton and Adams 1997, the colour-difference / gradient-weighting family; Markesteijn's
// idea of directional green + colour difference) and share no code with RawTherapee / darktable (GPL) or LibRaw's demosaic.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dapple::demosaic {

enum class Cfa : std::uint8_t { Bayer = 0, XTrans = 1 };

struct Rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Everything the node reads besides the samples. Pointers are borrowed.
struct Desc {
  Cfa cfa = Cfa::Bayer;
  int period = 2;                 // 2 (Bayer) or 6 (X-Trans)
  std::uint8_t cfa3[36] = {};     // colour of cell (y % period) * period + (x % period): 0 R, 1 G, 2 B
  std::uint8_t cfa4[36] = {};     // as in the file (0 R, 1 G, 2 B, 3 second green): selects the multiplier and the black level
  int width = 0, height = 0;      // the mosaic
  const std::uint16_t* data = nullptr;  // width * height
  int blackRows = 1, blackCols = 1;     // black table (total black level per cell)
  const float* black = nullptr;
  float invSpan = 1.f / 65535.f;  // 1 / (white - common black)
  float wb[4] = {1.f, 1.f, 1.f, 1.f};
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // white-balanced camera RGB -> output RGB, row major
  int xtMode = 1;                 // X-Trans: 0 = fast (green + guided differences + median), 1 = candidate directions with homogeneity selection
  bool hot = false;               // isolated hot-sample filter on the mosaic (same-colour neighbours, clip aware); needs 3 more samples of window on each side
  const std::atomic<std::uint64_t>* cancelCur = nullptr;  // latest-wins cancellation (engine CancelToken pattern): the work stops between tiles when *cancelCur != cancelMine
  std::uint64_t cancelMine = 0;
  bool clampOut = true;           // clip the output to [0, 1] like LibRaw's 16-bit output
};

constexpr int kHaloBayer = 4, kHaloXTrans = 6, kHaloXTransCand = 9;
inline int halo(Cfa c) { return c == Cfa::Bayer ? kHaloBayer : kHaloXTrans; }
constexpr int kHotMargin = 3;
inline int hotExtra(const Desc& d) { return d.hot ? kHotMargin : 0; }
inline int haloOf(const Desc& d) { return d.cfa == Cfa::Bayer ? kHaloBayer : (d.xtMode >= 1 ? kHaloXTransCand : kHaloXTrans); }

// Coordinate mapping that keeps the CFA phase: Bayer reflects (i -> -i, 2n - 2 - i), X-Trans repeats with the period.
int mapCoord(int i, int n, Cfa c);

// A finger is on a control (the app sets this from pointer-down to the end of the drag, dapple_set_interaction): background tile loops (CPU and GPU) wait
// between tiles, up to 2 s per tile, so a drag never competes with a region / preview decode that is in flight. Cancelled work stops waiting at once.
void setInteraction(bool on);
void yieldWhileInteracting(const std::atomic<std::uint64_t>* cancelCur, std::uint64_t cancelMine);

// The scaled value of a raw sample at (x, y) (inside the mosaic): max(raw - black, 0) * wb / span, clipped to 1.
float scaleSample(const Desc& d, int x, int y);

// Demosaic `roi` (inside the mosaic) into `out`: interleaved RGB float, row stride `strideFloats` (>= 3 * roi.w()). `threads` = 0: all cores.
void demosaic(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads = 0);

// One tile on prepared data (shared by the threads of demosaic() and by the tests). `S` is the scaled window of
// (roi.w() + 2 halo) x (roi.h() + 2 halo) floats whose (0,0) is absolute (roi.x0 - halo, roi.y0 - halo); the result is roi.w() x roi.h().
void demosaicWindow(const Desc& d, const float* S, Rect roi, float* out, std::size_t strideFloats);

// Plain bilinear (the reference the tests compare the structure of the node against; also the "fast" tier of a first picture).
void bilinear(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads = 0);

}  // namespace dapple::demosaic
