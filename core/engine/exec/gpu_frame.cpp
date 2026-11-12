// Pass graph of one frame (or one tile of an export). The structure follows core/src/spatial.cpp applySpatial():
//   point (fused) -> luma NR -> chroma NR -> dehaze -> clarity + texture + sharpen -> grain
// with the large-scale analysis (dehaze airlight / dark channel, clarity base) on a 1/4 grid that, for a tiled export,
// is assembled from all tiles first (GlobalRes) so every tile applies the same global numbers.
#include "engine/exec/gpu_frame.h"

namespace dapple::engine::gpu {

bool buildFrame(GpuRecorder& r, const FrameJob& j, H* out8) {
  const FramePlan& P = *j.plan;
  const SpatialRadii R = spatialRadii(P, j.resizeSharpen);
  r.W = j.work.w();
  r.H_ = j.work.h();
  const int W = r.W, Hh = r.H_;
  const bool finalMode = j.mode == Mode::Whole || j.mode == Mode::Final;
  const bool whole = j.mode == Mode::Whole;

  const bool nrL = P.nrLuma > 0, nrC = P.nrColor > 0, dz = P.dehaze != 0;
  const bool clar = P.clarity != 0, tex = P.texture != 0, shp = R.sharpAmount > 0;
  const bool detail = clar || tex || shp;
  const bool grain = P.grain > 0 && finalMode;
  Epilogue ep;
  grainArgs(P, ep.amp, ep.period);
  ep.grain = grain;

  // which stage ends the chain
  enum { SPoint, SNrL, SNrC, SDz, SDetail } last = SPoint;
  if (nrL) last = SNrL;
  if (nrC) last = SNrC;
  if (dz) last = SDz;
  if (detail) last = SDetail;
  auto epFor = [&](int s, H o8) {
    Epilogue e = ep;
    e.last = finalMode && last == s;
    e.out8 = e.last ? o8 : -1;
    return e;
  };

  const H D = r.tr(kF16, W, Hh);
  H o8 = -1;
  if (finalMode) o8 = r.tr(kU8, W, Hh);
  *out8 = o8;
  const int ox = j.work.x0, oy = j.work.y0;

  // ---- point-wise (fused) ----
  if (!recordPoint(r, PointNode{&P, j.src, j.lut, W, Hh, ox, oy, epFor(SPoint, o8)}, D)) return false;
  if (nrL && !recordNrLuma(r, NrLumaNode{&P, &R, W, Hh, ox, oy, epFor(SNrL, o8)}, D)) return false;
  if (nrC && !recordNrChroma(r, NrChromaNode{&P, &R, W, Hh, ox, oy, epFor(SNrC, o8)}, D)) return false;

  const bool tileGlobals = !whole && j.glob;
  auto interiorWin = [&]() {
    return WRect{j.interior.x0 - j.work.x0, j.interior.y0 - j.work.y0, j.interior.x1 - j.work.x0, j.interior.y1 - j.work.y0};
  };
  // dimensions of the 1/4 grid the dehaze / clarity analyses live on
  const int gridW = tileGlobals ? j.glob->lw : cdiv(W, 4), gridH = tileGlobals ? j.glob->lh : cdiv(Hh, 4);

  // ---- dehaze ----
  if (dz) {
    H dcH = -1;
    if (P.dehaze > 0) {
      if (j.mode == Mode::AnalyzeDehaze) {
        // sweep 1: only collect the interior's low-resolution colour
        r.down(D, 4, 0, j.glob->color, interiorWin(), j.interior.x0 / 4, j.interior.y0 / 4, cdiv(j.interior.w(), 4), cdiv(j.interior.h(), 4));
        return true;
      }
      if (whole) {
        const H col = r.tr(kF32, gridW, gridH);
        r.down(D, 4, 0, col, WRect{0, 0, W, Hh}, 0, 0, gridW, gridH);
        dcH = recordDehazeAnalysis(r, R, col, gridW, gridH);
        if (dcH < 0) return false;
      } else {
        dcH = j.glob->dc;
      }
    }
    if (!recordDehazeApply(r, DehazeNode{&P, W, Hh, ox, oy, gridW, gridH, dcH, epFor(SDz, o8)}, D)) return false;
  }

  // ---- clarity / texture / sharpen ----
  if (detail || j.mode == Mode::AnalyzeClarity) {
    H abH = -1;
    if (clar) {
      if (j.mode == Mode::AnalyzeClarity) {
        r.down(D, 4, 1, j.glob->yy, interiorWin(), j.interior.x0 / 4, j.interior.y0 / 4, cdiv(j.interior.w(), 4), cdiv(j.interior.h(), 4));
        return true;
      }
      if (whole) {
        const H yy = r.tr(kF32, gridW, gridH);
        r.down(D, 4, 1, yy, WRect{0, 0, W, Hh}, 0, 0, gridW, gridH);
        abH = recordClarityAnalysis(r, R, yy, gridW, gridH);
        if (abH < 0) return false;
      } else {
        abH = j.glob->ab;
      }
    }
    if (j.mode != Mode::AnalyzeClarity && !recordDetail(r, DetailNode{&P, &R, W, Hh, ox, oy, gridW, gridH, abH, epFor(SDetail, o8)}, D)) return false;
  }
  return true;
}

bool buildGlobalAnalysis(GpuRecorder& r, const FrameJob& j, bool dehaze) {
  const SpatialRadii R = spatialRadii(*j.plan, j.resizeSharpen);
  const GlobalRes& G = *j.glob;
  r.W = G.lw;
  r.H_ = G.lh;
  if (dehaze) {
    const H dc = recordDehazeAnalysis(r, R, G.color, G.lw, G.lh);
    if (dc < 0) return false;
    r.g.copyImage(dc, G.dc, std::uint32_t(G.lw), std::uint32_t(G.lh));
  } else {
    const H ab = recordClarityAnalysis(r, R, G.yy, G.lw, G.lh);
    if (ab < 0) return false;
    r.g.copyImage(ab, G.ab, std::uint32_t(G.lw), std::uint32_t(G.lh));
  }
  return true;
}

}  // namespace dapple::engine::gpu
