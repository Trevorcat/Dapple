// CpuExecutor: runs a Plan (docs/engine/pipeline-architecture.md 5.2-5.5, 5.8, section 50).
//
//  - the head group runs as 256-pixel block fragments (CpuImpl::block, fused where the lead node's CpuImpl::fuse says so), rows spread
//    over the ThreadPool;
//  - window groups run on a tile (an ImageF window = the plan's work rect) through CpuImpl::tile;
//  - a GlobalSolve runs the node's CpuImpl::solve on the 1/4 grid the previous sweep accumulated; the sweeps after it hand the payload to
//    the node's tile (CpuTile::solved), looked up at absolute coordinates, so a tile result does not depend on the tile decomposition (5.5);
//  - a single-tile plan (Whole) keeps the image between sweeps, a multi-tile plan recomputes the chain per sweep;
//  - cancellation is polled between tiles and groups and by the ThreadPool chunk loop (CancelScope).
// No node type is named here: every node is reached through its NodeDesc (cpu, bind).
#include "engine/exec/cpu_executor.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "parallel.h"

namespace dapple::engine {

namespace {

struct Step {
  const CpuImpl* impl = nullptr;
  Resolved r;
  NodeId node = 0;
  Accum accum = Accum::None;
  std::shared_ptr<void> solved;
};

struct Run {
  const Plan& plan;
  ExecEnv& env;
  ExecStats* stats;
  const NodeRegistry& reg = NodeRegistry::instance();
  point::CpuFrame& fk;
  int W, H;
  point::ScratchFrame frameBuf;   // Whole mode: the one float frame the window nodes work on
  point::ScratchFrame filmScratch;
  ImageF* film = nullptr;         // the film stage's output plane
  ImageF tileBuf, cropBuf;        // tiled mode
  SpatialScratch scratch;         // luminance plane etc., kept between window nodes
  std::vector<Step> head, wins;   // wins: every window node of the final sweep, in chain order
  FrameBlocks blocks;             // resolved when a chain is bound (after the film stage: the exact variant's facts change there)
  std::vector<float> gridLow;     // accumulated 1/4 grid of the sweep in flight
  int gridW = 0, gridH = 0;
  bool single = true;
  ImageF* img = nullptr;          // persistent frame (single-tile plans)
  bool imgValid = false;
  std::size_t done = 0;           // window nodes already applied to the persistent frame
  // the side planes the head writes for the work window and a window node reads (Plan::needAux; local adjustments, section 40)
  local::AuxTarget aux;
  std::vector<float> auxBuf;
  int auxPlanes = 0;
  int outRow0 = 0;                // streaming export: frame row of env.out's first row (the band in flight)
  std::vector<float> filmAux;     // a tiled film frame: the side planes of the whole frame (the film input pass's), copied per work window
  local::AuxTarget filmAuxAt;
  ImageF filmBand;                // streaming export of a film frame: rows [filmY0, filmY1) of the film plane
  int filmY0 = 0, filmY1 = 0;
  bool filmStream = false;

  Run(const Plan& p, ExecEnv& e, ExecStats* s) : plan(p), env(e), stats(s), fk(e.fk), W(e.fk.width()), H(e.fk.height()) {}

  bool cancelled() const { return env.cancel.cancelled(); }
  const NodeDesc& desc(NodeId id) const { return reg.desc(env.graph.nodes()[id].type); }

  void auxWindow(const Rect& r) {
    if (auxPlanes == 0) return;
    aux.ox = r.x0, aux.oy = r.y0, aux.w = r.x1 - r.x0, aux.h = r.y1 - r.y0;
    aux.plane = std::size_t(aux.w) * aux.h;
    if (auxBuf.size() < aux.plane * std::size_t(auxPlanes)) auxBuf.resize(aux.plane * std::size_t(auxPlanes));
    aux.data = auxBuf.data();
  }

  FrameEnv frameEnv(std::span<const NodeType> chain) {
    FrameEnv fe;
    fe.fk = &fk;
    fe.blocks = &blocks;
    fe.film = film;
    fe.filmRow0 = filmStream ? &filmY0 : nullptr;
    fe.aux = auxPlanes ? &aux : nullptr;
    fe.chain = chain;
    return fe;
  }

  // ---- binding ------------------------------------------------------------------------------------------------------------
  bool bindHead(const Stage& sweep) {
    blocks = fk.frameBlocks();
    std::vector<NodeType> chain;
    for (NodeId id : sweep.groups[0].nodes) chain.push_back(env.graph.nodes()[id].type);
    const FrameEnv fe = frameEnv(chain);
    head.clear();
    for (std::size_t i = 0; i < chain.size();) {
      const NodeDesc& d = reg.desc(chain[i]);
      Step s;
      s.node = sweep.groups[0].nodes[i];
      s.impl = d.cpu;
      if (!s.impl || !s.impl->block || !d.bind || !d.bind(fe, s.r)) return false;
      std::size_t consumed = 1;
      if (s.impl->fuse)
        if (const CpuImpl* f = s.impl->fuse(fe, std::span<const NodeType>(chain).subspan(i), &consumed); f && consumed > 1) s.impl = f;
      consumed = std::max<std::size_t>(consumed, 1);
      head.push_back(s);
      i += consumed;
    }
    return true;
  }
  bool bindWindows(const Stage& last) {
    blocks = fk.frameBlocks();
    const FrameEnv fe = frameEnv({});
    wins.clear();
    for (std::size_t g = 1; g < last.groups.size(); ++g)
      for (NodeId id : last.groups[g].nodes) {
        const NodeDesc& d = desc(id);
        Step s;
        s.node = id;
        s.impl = d.cpu;
        if (!s.impl || !s.impl->tile || !d.bind || !d.bind(fe, s.r)) return false;
        if (d.solve) s.accum = d.solve(s.r.bytes);
        wins.push_back(s);
      }
    return true;
  }

  // ---- the head chain over a rectangle of the frame -------------------------------------------------------------------------
  // dst != null: float pixels into dst (rw x rh, origin (rx0, ry0)); else packed 8-bit straight into env.out.
  void runHead(int rx0, int ry0, int rw, int rh, ImageF* dst) {
    const int ch = env.outChannels;
    parallelFor(
        rh,
        [&](int r) {
          const int y = ry0 + r;
          float* drow = dst ? dst->row(r) : nullptr;
          std::uint8_t* o = dst ? nullptr : env.out + (std::size_t(y - outRow0) * W + rx0) * ch;
          for (int x = 0; x < rw; x += 256) {
            const int n = std::min(256, rw - x);
            float px[256 * 3];
            const PixelPos pos{rx0 + x, y};
            for (const Step& s : head) s.impl->block(s.r.bytes, px, n, pos);
            if (drow) {  // clamp into the frame
              float* d = drow + 3 * std::size_t(x);
              for (int i = 0; i < n * 3; ++i) d[i] = std::clamp(px[i], 0.f, 1.f);
            } else {  // clamp + 8-bit encode (the sink, fused into the chain when no window node follows)
              for (int i = 0; i < n; ++i) {
                const float r = std::clamp(px[3 * i], 0.f, 1.f), g = std::clamp(px[3 * i + 1], 0.f, 1.f), b = std::clamp(px[3 * i + 2], 0.f, 1.f);
                o[ch * (x + i)] = std::uint8_t(r * 255.f + 0.5f);
                o[ch * (x + i) + 1] = std::uint8_t(g * 255.f + 0.5f);
                o[ch * (x + i) + 2] = std::uint8_t(b * 255.f + 0.5f);
                if (ch == 4) o[ch * (x + i) + 3] = 255;
              }
            }
          }
        },
        8);
  }

  // ---- window nodes on a tile --------------------------------------------------------------------------------------------
  Status runWindows(ImageF& im, int ox, int oy, std::size_t from, std::size_t to, SpatialScratch& sc) {
    CpuTile t;
    t.img = &im;
    t.scratch = &sc;
    t.ox = ox;
    t.oy = oy;
    t.frameW = W;
    t.frameH = H;
    t.aux = auxPlanes ? &aux : nullptr;
    for (std::size_t i = from; i < to; ++i) {
      if (cancelled()) return Status::Cancelled;
      t.solved = wins[i].solved.get();
      const Status st = wins[i].impl->tile(t, wins[i].r.bytes);
      if (st != Status::Ok) return st;
    }
    return cancelled() ? Status::Cancelled : Status::Ok;
  }

  Status sink(ImageF& im, int ox, int oy, const Rect& interior) {
    CpuTile t;
    t.img = &im;
    t.ox = ox;
    t.oy = oy;
    t.frameW = W;
    t.frameH = H;
    t.interior = interior;
    t.out8 = env.out;
    t.outChannels = env.outChannels;
    t.outRow0 = outRow0;
    const CpuImpl* pack = desc(plan.sink).cpu;
    return pack && pack->tile ? pack->tile(t, nullptr) : Status::Unsupported;
  }

  // ---- accumulate the 1/4 grid a global solve needs, from the interior of a tile -----------------------------------------------
  void accumulate(Accum kind, const ImageF& im, const Rect& in, int ox, int oy, bool whole, SpatialScratch& sc) {
    const int iw = in.x1 - in.x0, ih = in.y1 - in.y0;
    const ImageF* src = &im;
    if (!whole && kind == Accum::ColourGrid) {  // the interior alone (its origin is on the 1/4 grid: the frame's blocks)
      if (cropBuf.data.size() < std::size_t(iw) * ih * 3) cropBuf.data.resize(std::size_t(iw) * ih * 3);
      cropBuf.width = iw;
      cropBuf.height = ih;
      for (int y = 0; y < ih; ++y)
        std::memcpy(cropBuf.row(y), im.row(in.y0 + y) + 3 * std::size_t(in.x0), std::size_t(iw) * 3 * sizeof(float));
      src = &cropBuf;
    }
    std::vector<float> low;
    int lw = 0, lh = 0;
    const int ch = kind == Accum::ColourGrid ? 3 : 1;
    if (kind == Accum::ColourGrid) dehazeLowGrid(*src, low, lw, lh);
    else lumaGrid(im, sc, in.x0, in.y0, in.x1, in.y1, low, lw, lh);
    if (whole) {
      gridLow = std::move(low);
      gridW = lw;
      gridH = lh;
      return;
    }
    if (gridW == 0) {
      gridW = (W + 3) / 4;
      gridH = (H + 3) / 4;
      gridLow.assign(std::size_t(gridW) * gridH * ch, 0.f);
    }
    const int gx = (ox + in.x0) / 4, gy = (oy + in.y0) / 4;
    for (int y = 0; y < lh; ++y)
      std::memcpy(gridLow.data() + (std::size_t(gy + y) * gridW + gx) * ch, low.data() + std::size_t(y) * lw * ch, std::size_t(lw) * ch * sizeof(float));
  }

  // ---- streaming export hooks (section 42) ------------------------------------------------------------------------------------------
  bool prepare(const TileWin& tw) {
    const Rect& r = tw.work;
    if (!filmAux.empty() && auxPlanes && aux.data) {  // the window's part of the whole-frame side planes
      for (int k = 0; k < auxPlanes; ++k)
        for (int y = r.y0; y < r.y1; ++y)
          std::memcpy(aux.data + std::size_t(k) * aux.plane + std::size_t(y - aux.oy) * aux.w + std::size_t(r.x0 - aux.ox),
                      filmAux.data() + std::size_t(k) * filmAuxAt.plane + std::size_t(y) * filmAuxAt.w + std::size_t(r.x0),
                      std::size_t(r.x1 - r.x0) * sizeof(float));
    }
    if (filmStream) {  // the film rows of the rect, the side planes of its window; the source only when the normal branch is read
      if (filmBand.height == 0 || r.y0 < filmY0 || r.y1 > filmY1) {
        if (!env.filmRows || !env.filmRows(r.y0, r.y1, filmBand)) return false;
        filmY0 = r.y0, filmY1 = r.y1;
      }
      if (auxPlanes && aux.data && env.filmAux && !env.filmAux(r.x0, r.y0, r.x1, r.y1, aux)) return false;
      if (!fk.needNormal()) return true;
    }
    return !env.prepare || env.prepare(tw);
  }
  bool bandEnd(const Stage& st, int y0, int y1) { return !(st.output && env.bandDone) || env.bandDone(y0, y1); }

  // ---- one TileSweep ----------------------------------------------------------------------------------------------------------
  Status sweep(const Stage& st) {
    if (stats) ++stats->sweeps;
    if (!bindHead(st)) return Status::Unsupported;
    const std::size_t nWin = st.groups.size() - 1;
    gridW = gridH = 0;

    if (nWin == 0 && st.output && env.outF) {  // float frame straight from the chain (Whole plans)
      *env.outF = ImageF(W, H);
      runHead(0, 0, W, H, env.outF);
      if (stats) ++stats->tiles;
      return cancelled() ? Status::Cancelled : Status::Ok;
    }
    if (single && !(nWin == 0 && st.output)) {  // one tile: the image stays between sweeps
      if (!imgValid) {
        const TileWin& tw = st.tiles.front();
        img = film ? film : &frameBuf.get(W, H);
        auxWindow(tw.work);
        if (!prepare(tw)) return Status::Cancelled;
        if (!st.groups[0].nodes.empty()) runHead(0, 0, W, H, img);
        if (cancelled()) return Status::Cancelled;
        scratch.yValid = false;
        imgValid = true;
        done = 0;
      }
      if (stats) ++stats->tiles;
      Status s = runWindows(*img, 0, 0, done, nWin, scratch);
      if (s != Status::Ok) return s;
      done = nWin;
      const Rect all{0, 0, W, H};
      if (st.accum != Accum::None) accumulate(st.accum, *img, all, 0, 0, true, scratch);
      if (st.output && env.outF) {  // float output: the finished frame as it is (the 8-bit pack is a pure encode of it)
        *env.outF = *img;
        return cancelled() ? Status::Cancelled : Status::Ok;
      }
      if (st.output) {
        if ((s = sink(*img, 0, 0, all)) != Status::Ok) return s;
        if (!bandEnd(st, 0, H)) return Status::Cancelled;
      }
      return cancelled() ? Status::Cancelled : Status::Ok;
    }

    // the plan's tiles, row by row (a band = one row of tiles); the chain is recomputed per sweep (5.4)
    int bandY0 = -1, bandY1 = 0;
    for (const TileWin& tw : st.tiles) {
      if (tw.interior.y0 != bandY0) {
        if (bandY0 >= 0 && !bandEnd(st, bandY0, bandY1)) return Status::Cancelled;
        bandY0 = tw.interior.y0, bandY1 = tw.interior.y1;
        if (env.bandDone) outRow0 = bandY0;
      }
      if (cancelled()) return Status::Cancelled;
      const Rect& w = tw.work;
      const int rw = w.x1 - w.x0, rh = w.y1 - w.y0;
      auxWindow(w);
      if (!prepare(tw)) return Status::Cancelled;
      if (nWin == 0 && st.output) {  // no window node: the chain packs the 8-bit pixels itself
        runHead(w.x0, w.y0, rw, rh, nullptr);
      } else {
        if (tileBuf.data.size() < std::size_t(rw) * rh * 3) tileBuf.data.resize(std::size_t(rw) * rh * 3);
        tileBuf.width = rw;
        tileBuf.height = rh;
        runHead(w.x0, w.y0, rw, rh, &tileBuf);
        if (cancelled()) return Status::Cancelled;
        scratch.yValid = false;
        Status s = runWindows(tileBuf, w.x0, w.y0, 0, nWin, scratch);
        if (s != Status::Ok) return s;
        const Rect in{tw.interior.x0 - w.x0, tw.interior.y0 - w.y0, tw.interior.x1 - w.x0, tw.interior.y1 - w.y0};
        if (st.accum != Accum::None) accumulate(st.accum, tileBuf, in, w.x0, w.y0, false, scratch);
        if (st.output && (s = sink(tileBuf, w.x0, w.y0, in)) != Status::Ok) return s;
      }
      if (stats) ++stats->tiles;
    }
    if (cancelled()) return Status::Cancelled;
    if (bandY0 >= 0 && !bandEnd(st, bandY0, bandY1)) return Status::Cancelled;
    return Status::Ok;
  }

  Step* windowOf(NodeId id) {
    for (Step& s : wins)
      if (s.node == id) return &s;
    return nullptr;
  }

  // ---- a GlobalSolve --------------------------------------------------------------------------------------------------------
  Status solve(const Stage& st) {
    if (cancelled()) return Status::Cancelled;
    Step* w = windowOf(st.solveNode);
    if (!w || !w->impl->solve) return Status::Unsupported;
    std::size_t bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    w->solved = w->impl->solve(gridLow, gridW, gridH, w->r.bytes, &bytes);
    if (!w->solved) return Status::Internal;
    if (cancelled()) return Status::Cancelled;
    const NodeInstance& n = env.graph.nodes()[st.solveNode];
    ResultCache::Entry e;
    e.slot = slotOf(n.type);
    e.key = e.family = n.key;
    e.payload = w->solved;
    e.hostBytes = bytes;
    e.costMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    env.cache.put(std::move(e));
    if (stats) ++stats->solveBuilds;
    gridLow.clear();
    gridLow.shrink_to_fit();
    return Status::Ok;
  }

  // Solves the plan did not schedule because they are in the cache.
  Status loadCachedSolves() {
    for (Step& s : wins) {
      if (s.accum == Accum::None) continue;
      bool planned = false;
      for (const Stage& st : plan.stages) planned |= st.kind == StageKind::GlobalSolve && st.solveNode == s.node;
      if (planned) continue;
      const NodeInstance& n = env.graph.nodes()[s.node];
      const ResultCache::Hit h = env.cache.find(slotOf(n.type), n.key);
      if (!h) return Status::Internal;
      s.solved = h.payload;
      if (stats) ++stats->solveHits;
    }
    return Status::Ok;
  }
};

}  // namespace

Status CpuExecutor::run(const Plan& plan, ExecEnv& env, ExecStats* stats) {
  if (plan.unsupported || !env.fk.valid() || (!env.out && !env.outF)) return Status::Unsupported;
  if (env.outF && !(plan.grid.nx == 1 && plan.grid.ny == 1)) return Status::Unsupported;
  CancelScope scope(env.cancel.current, env.cancel.mine);
  Run r(plan, env, stats);
  r.single = plan.grid.nx == 1 && plan.grid.ny == 1;
  if (const FrameBound b = env.fk.frameBound(); b.local && plan.needAux) r.auxPlanes = local::auxLayout(*b.local, r.aux);
  if (env.cancel.cancelled()) return Status::Cancelled;

  const Stage* lastSweep = nullptr;  // the last TileSweep lists every window node
  for (const Stage& s : plan.stages)
    if (s.kind == StageKind::TileSweep) lastSweep = &s;
  if (!lastSweep) return Status::Unsupported;

  // the film stage: whole-frame before the sweeps (the streaming path finishes the frame by itself)
  for (const Stage& s : plan.stages) {
    if (s.kind != StageKind::FilmFrame) continue;
    if (env.prepare) {  // streaming export: the film in bands (section 42.6), rows rendered as the sweeps need them
      if (!env.filmBegin || !env.filmBegin()) return Status::Unsupported;
      r.filmStream = true;
      r.film = &r.filmBand;
      env.fk.finalizeSpatial();
      if (env.cancel.cancelled()) return Status::Cancelled;
      continue;
    }
    if (!env.fk.exactVariant() && env.fk.filmFull() && env.outChannels == 4 && !env.outF && !env.fk.frameBlocks().anyWindow() && !env.fk.frameBound().local) {
      if (env.fk.streamFilm(env.out)) {
        if (stats) stats->streamed = true;
        return Status::Ok;
      }
    }
    r.film = &r.filmScratch.get(r.W, r.H);
    r.auxWindow(Rect{0, 0, r.W, r.H});  // local adjustments: the film input pass writes the side planes of the whole frame
    if (!env.fk.runFilm(*r.film, r.auxPlanes ? &r.aux : nullptr)) return Status::Unsupported;  // the frame fails with an explicit engine error
    if (r.auxPlanes && !r.single) {  // the sweeps size the side planes per work window: keep the whole frame's apart
      r.filmAuxAt = r.aux;
      r.filmAux.assign(r.auxBuf.begin(), r.auxBuf.begin() + std::ptrdiff_t(r.aux.plane * std::size_t(r.auxPlanes)));
    }
    env.fk.finalizeSpatial();
    if (env.cancel.cancelled()) return Status::Cancelled;
  }

  if (!r.bindWindows(*lastSweep)) return Status::Unsupported;
  Status s = r.loadCachedSolves();
  if (s != Status::Ok) return s;

  for (const Stage& st : plan.stages) {
    if (env.cancel.cancelled()) return Status::Cancelled;
    if (st.kind == StageKind::TileSweep) s = r.sweep(st);
    else if (st.kind == StageKind::GlobalSolve) s = r.solve(st);
    else continue;
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

}  // namespace dapple::engine
