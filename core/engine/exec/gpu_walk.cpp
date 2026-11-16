// The recording of one GPU job (a whole frame, or one tile of one sweep of a tiled export): the plan's groups through the nodes' GpuImpls.
// A head group is recorded once, by the impl of highest rank among its nodes (the point recipe, or the film finish when the group holds the
// display stages); a window group by its node's impl, with its block (NodeDesc::block) and its solve; epilogue nodes ride on the last pass.
#include <algorithm>

#include "engine/exec/gpu_executor_internal.h"
#include "local_eval.h"

namespace dapple::engine::gpu {

Walk::Walk(GpuRecorder& rec, const GpuRun& run, bool wholeFrame) : r(rec), in(run), whole(wholeFrame) {
  view.point = &in.frame;
  view.blocks = &in.blocks;
  job.frame = &in.frame;
  job.blocks = &in.blocks;
  ep.amp = in.blocks.grain.amp;
  ep.period = in.blocks.grain.period;
  ep.grain = in.blocks.grain.active();
}

SolveRes* Walk::solveOf(NodeId id) {
  for (SolveRes& s : solves)
    if (s.node == id) return &s;
  return nullptr;
}

filmnode::FilmSrc Walk::filmInputRect(int wx0, int wy0, int ww, int wh) {
  const point::Blocks& P = in.frame;
  H src = job.src.img;
  if (in.blocks.local.active() && job.ldesc >= 0 && job.lplanes >= 0) {  // local adjustments, stage L, on the film's scene input (40.7)
    if (needAux && job.aux0 < 0) {
      job.aux0 = r.tr(kF16, P.geo.outW, P.geo.outH);
      job.aux1 = r.tr(kF16, P.geo.outW, P.geo.outH);
    }
    const H g = r.g.transient(hal::Format::RGBA32F, std::uint32_t(ww), std::uint32_t(wh));
    const LocalFilmInNode ln{&P, job.src, ww, wh, wx0, wy0, job.ldesc, job.lplanes, needAux ? job.aux0 : -1, needAux ? job.aux1 : -1};
    if (!recordLocalFilmIn(r, ln, g)) return {};
    return {g, 0, 0};
  }
  if (P.geo.useGeo) {
    struct U {
      std::int32_t a[4], b[4];
      float geoA[4], geoB[4], geoC[4];
      std::int32_t c[4];
      WarpU w;
    } u{};
    u.a[0] = P.geo.srcW;
    u.a[1] = P.geo.srcH;
    u.a[2] = ww;
    u.a[3] = wh;
    u.b[0] = job.src.ox;  // origin of the bound source image (0 for a resident source, the window's for a streamed one)
    u.b[1] = job.src.oy;
    u.b[2] = wx0;  // origin of this image in the picture
    u.b[3] = wy0;
    u.geoA[0] = P.geo.cs;
    u.geoA[1] = P.geo.sn;
    u.geoA[2] = P.geo.fw;
    u.geoA[3] = P.geo.fh;
    u.geoB[0] = P.geo.ox0;
    u.geoB[1] = P.geo.oy0;
    u.geoB[2] = P.geo.stepX;
    u.geoB[3] = P.geo.stepY;
    u.geoC[0] = P.geo.fullW;
    u.geoC[1] = P.geo.fullH;
    u.c[0] = P.geo.rot;
    u.c[1] = P.geo.flip ? 1 : 0;
    const bool warpOn = P.warp.active() && job.src.warp >= 0;
    if (warpOn) warpUniform(P.warp, job.src, u.w);
    const hal::PipelineH p = r.pl("film_src", {{0, P.geo.exact ? 1 : 2}, {4, warpOn ? 1 : 0}});
    if (!p) return {};
    const H g = r.g.transient(hal::Format::RGBA32F, std::uint32_t(ww), std::uint32_t(wh));
    r.g.dispatch("film_src", p, {GpuGraph::R(job.src.img), GpuGraph::W(g), GpuGraph::R(warpOn ? job.src.warp : r.g.externalBuf(r.gBuf))}, r.gx(ww),
                 r.gy(wh), &u, sizeof u);
    return {g, 0, 0};
  }
  return {src, wx0 - job.src.ox, wy0 - job.src.oy};  // the bound source (resident: ox = 0; a streamed window starts at src.ox): window pixel (0, 0) is the picture's (x0, y0)
}

H Walk::filmInput() {
  const point::Blocks& P = in.frame;
  const filmnode::FilmSrc s = filmInputRect(0, 0, P.geo.outW, P.geo.outH);
  return s.img;
}

bool Walk::filmStage() {
  const H src = filmInput();
  if (src < 0) return false;
  job.film = filmnode::recordFilmFrame(r, *fres, *ftab, *fplan, src, in.frame.geo.outW, in.frame.geo.outH);
  job.filmClamped = job.film >= 0 && filmnode::filmEndsClamped(*fres);
  job.filmOx = job.filmOy = 0;
  return job.film >= 0;
}

filmnode::FilmSrcFn Walk::filmSrcFn() {
  return [this](const filmnode::FilmWin& w) { return filmInputRect(w.x0, w.y0, w.w, w.h); };
}

bool Walk::filmLowPass(filmnode::FilmPre& out) {
  return filmnode::recordFilmLow(r, *fres, *ftab, filmSrcFn(), in.frame.geo.outW, in.frame.geo.outH, out);
}

bool Walk::filmWindowStage() {
  const point::Blocks& P = in.frame;
  const int halo = filmnode::filmWindowHalo(*fres);
  filmnode::FilmWin win;
  win.x0 = std::max(0, work.x0 - halo);
  win.y0 = std::max(0, work.y0 - halo);
  win.w = std::min(P.geo.outW, work.x1 + halo) - win.x0;
  win.h = std::min(P.geo.outH, work.y1 + halo) - win.y0;
  const filmnode::FilmImg fi = filmnode::recordFilmWindow(r, *fres, *ftab, *fplan, pre, filmSrcFn(), P.geo.outW, P.geo.outH, win);
  if (fi.img < 0) return false;
  job.film = fi.img;
  job.filmOx = fi.ox;
  job.filmOy = fi.oy;
  return true;
}

bool Walk::sweep(const Stage& st) {
  const auto& nodes = in.graph.nodes();
  r.W = work.w();
  r.H_ = work.h();
  const Format fmt = st.precision == Precision::F32 ? kF32 : kF16;
  if (D < 0) D = r.tr(fmt, r.W, r.H_);
  if (st.output && o8 < 0) o8 = r.tr(kU8, r.W, r.H_);
  if (needAux && job.aux0 < 0) {
    job.aux0 = r.tr(kF16, r.W, r.H_);
    job.aux1 = r.tr(kF16, r.W, r.H_);
  }
  job.W = r.W;
  job.Hh = r.H_;
  job.ox = work.x0;
  job.oy = work.y0;
  job.gridW = whole ? cdiv(work.w(), 4) : cdiv(in.frame.geo.outW, 4);
  job.gridH = whole ? cdiv(work.h(), 4) : cdiv(in.frame.geo.outH, 4);

  // the impl that records a group: the highest-ranked of its nodes
  auto lead = [&](const Group& g) {
    const GpuImpl* best = nullptr;
    for (NodeId id : g.nodes) {
      const GpuImpl* gi = gpuImplFor(nodes[id].type);
      if (gi && (!best || gi->rank > best->rank)) best = gi;
    }
    return best;
  };
  int lastPass = 0;  // the last group that ends with a real pass (an epilogue-only group rides on it)
  for (int i = 0; i < int(st.groups.size()); ++i) {
    const GpuImpl* gi = lead(st.groups[std::size_t(i)]);
    if (gi && !gi->epilogue) lastPass = i;
  }

  for (int gi = ran; gi < int(st.groups.size()); ++gi) {
    const Group& g = st.groups[std::size_t(gi)];
    if (g.nodes.empty()) continue;
    Epilogue e = ep;
    e.last = st.output && gi == lastPass;
    e.out8 = e.last ? o8 : -1;
    e.grain = ep.grain && st.output;
    const GpuImpl* impl = lead(g);
    if (!impl) return r.err = "node without a GPU implementation", false;
    if (impl->epilogue) continue;
    H nd = -1;
    if (g.kind == GroupKind::Head) {
      std::vector<NodeType> types;
      for (NodeId id : g.nodes) types.push_back(nodes[id].type);
      if (!impl->group) return r.err = "head group without a recipe", false;
      nd = impl->group(r, GroupCall{job, types, e, D});
    } else {
      const NodeType t = nodes[g.nodes.front()].type;
      if (!impl->node) return r.err = "window node without a record function", false;
      const SolveRes* s = solveOf(g.nodes.front());
      nd = impl->node(r, NodeCall{job, reg.block(t, view), e, D, s ? s->solved : -1});
    }
    if (nd < 0) return false;
    D = nd;
  }
  ran = int(st.groups.size());
  return true;
}

// the end of a sweep: the 1/4 grid of the image at the solving node's input, for the GlobalSolve that follows
bool Walk::accumulate(const Stage& st) {
  if (st.accum == Accum::None) return true;
  SolveRes* s = solveOf(st.solveNode);
  if (!s) return r.err = "accumulation without a solve", false;
  const int mode = st.accum == Accum::ColourGrid ? 0 : 1;
  if (whole) {
    s->grid = r.tr(kF32, job.gridW, job.gridH);
    return r.down(D, 4, mode, s->grid, WRect{0, 0, work.w(), work.h()}, 0, 0, job.gridW, job.gridH) >= 0;
  }
  return r.down(D, 4, mode, s->grid, interiorWin(), interior.x0 / 4, interior.y0 / 4, cdiv(interior.w(), 4), cdiv(interior.h(), 4)) >= 0;
}

bool Walk::solve(const Stage& st) {
  SolveRes* s = solveOf(st.solveNode);
  const NodeType t = in.graph.nodes()[st.solveNode].type;
  const GpuImpl* impl = gpuImplFor(t);
  if (!s || !impl || !impl->solve) return r.err = "solve without an implementation", false;
  s->solved = impl->solve(r, reg.block(t, view), s->grid, job.gridW, job.gridH);
  return s->solved >= 0;
}

}  // namespace dapple::engine::gpu
