#pragma once
// The CPU binding of one frame (CpuFrame::Impl), shared by the point family's CPU nodes (cpu.cpp) and the film stage's glue
// (nodes/film/film_glue.cpp). Internal header.
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "engine/nodes/film/film_nodes.h"
#include "engine/nodes/local/local.h"
#include "engine/nodes/point/cpu.h"
#include "engine/nodes/point/kernels.h"
#include "hdr_field.h"
#include "local_eval.h"
#include "warp_field.h"

namespace dapple::engine::point {

std::uint64_t nextFrameId();  // cpu.cpp: identity of a binding (the thread's row memo)

struct CpuFrame::Impl {
  const std::uint64_t id = nextFrameId();
  const Params& r;
  const SourceImage& source;
  const ImageF& in;
  const BakedLut* lutp;
  const bool exact;
  FastOptions opt;
  engine::point::Blocks b;      // geometry, warp, gain, HDR, look grain, radii scale of this frame (point::resolveFrame + hdrBind)
  FilmRun fr;
  HdrParams hdrP;
  std::optional<LutSampler> sample;
  mutable std::once_flag opOnce;
  mutable std::optional<PointOp> overflowOp;
  std::optional<PointOp> exactOp;
  bool filmRan = false, filmOkV = false, valid = false;
  std::shared_ptr<const local::Frame> local;  // local adjustments (docs/engine/pipeline-architecture.md section 40); null for film frames in v1
  const ImageF* win = nullptr;  // streaming export (section 42): the source pixels of the rect in flight; `in` then carries the size only
  int winX = 0, winY = 0;
  std::function<bool(int, int, int, int)> sourceFn;              // streaming export: asks for the source window of a rect (setSourceProvider)
  std::unique_ptr<engine::filmnode::FilmBands> filmBands;       // streaming export: the film in bands (filmStreamBegin)
  const warp::Field* wf = nullptr;  // portrait reshaping (section 45): the backward field of b.warp, null = none
  warp::Map wm;

  Impl(const Params& rr, const SourceImage& src, const BakedLut* lut, const FastOptions& o)
      : r(rr), source(src), in(src.linear), lutp(lut), exact(lut == nullptr), opt(o) {
    if (in.empty() || (lut && lut->n < 2)) return;
    fr = resolveFilm(r.p);
    b = resolveFrame(r, in.width, in.height, source.sceneReferred, opt, &source, exact);
    if (!exact) {
      sample.emplace(*lut);
    } else {
      exactOp.emplace(makePointOp(r, source, nullptr));
    }
    wf = b.warp.field.get();
    if (wf) wm.ax = b.warp.ax, wm.bx = b.warp.bx, wm.ay = b.warp.ay, wm.by = b.warp.by, wm.lpx = b.warp.lpx;
    if (effectiveHdr(r.p) > 0.f) {
      std::vector<float> exactTone;
      if (exact) fillToneTable(exactTone, *exactOp);
      hdrBind(r, source, b.hdr, exact ? &exactTone : &lut->tone);
    }
    if (b.hdr.active()) {
      const engine::point::Hdr& hdr = b.hdr;
      hdrP.s = hdr.s, hdrP.ev = hdr.ev, hdrP.key = hdr.key, hdrP.yn = hdr.yn, hdrP.yclip = hdr.yclip;
      hdrP.lo = hdr.lo, hdrP.hi = hdr.hi, hdrP.ls = hdr.ls, hdrP.hp = hdr.hp;
    }
    local = engine::localadj::bind(r.p, source, opt.spatial == SpatialQuality::Interactive);
    valid = true;
  }

  // source position (continuous, pixel centres at +0.5) of output pixel (x, y), normalised by the source size
  void srcUV(int x, int y, float& u, float& v) const {
    if (!b.geo.useGeo) {
      u = (float(x) + 0.5f) / float(in.width);
      v = (float(y) + 0.5f) / float(in.height);
      return;
    }
    float sx, sy;
    mapPixel(b.geo, x, y, sx, sy);
    if (wf) warp::displace(*wf, wm, sx, sy);
    u = sx / float(in.width);
    v = sy / float(in.height);
  }
  void hdrPixel(int x, int y, float* v3) const {
    float u, v;
    srcUV(x, y, u, v);
    hdr::apply(*b.hdr.field, hdrP, u, v, v3);
  }

  void fetchPixel(int x, int y, float v[3]) const {
    float sx, sy;
    mapPixel(b.geo, x, y, sx, sy);
    const bool moved = wf && warp::displace(*wf, wm, sx, sy);  // reshaping: resampled where the field moves the read, exact elsewhere
    if (b.geo.exact && !moved) {
      const int iy = std::clamp(int(std::floor(sy)), 0, in.height - 1), ix = std::clamp(int(std::floor(sx)), 0, in.width - 1);
      const float* px = win ? win->row(iy - winY) + 3 * (ix - winX) : in.row(iy) + 3 * ix;
      v[0] = px[0], v[1] = px[1], v[2] = px[2];
    } else if (moved) {
      // a read moved by the reshaping field: Lanczos-3 (sharper than the geometry's Catmull-Rom, which softens a sub-pixel shift; section 45)
      if (win) sampleLanczos3(*win, winX, winY, in.width, in.height, sx, sy, v);
      else sampleLanczos3(in, 0, 0, in.width, in.height, sx, sy, v);
    } else if (win) {
      sampleCubicWin(*win, winX, winY, in.width, in.height, sx, sy, v);
    } else {
      sampleCubic(in, sx, sy, v);
    }
  }
};

}  // namespace dapple::engine::point
