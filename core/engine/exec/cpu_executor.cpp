// CpuExecutor: runs a Plan (docs/engine/pipeline-architecture.md 5.2-5.5, 5.8).
//
//  - the PointChain head runs as 256-pixel block fragments (CpuImpl::block), rows spread over the ThreadPool;
//  - window nodes run on a tile (an ImageF window = interior + the sweep's halo) through CpuImpl::tile;
//  - frame-global parts (dehaze airlight / transmission, guided-filter coefficients) are solved once from the 1/4 grid the
//    previous sweep accumulated and are looked up at absolute coordinates by the sweeps after it, so a tile result does not
//    depend on the tile decomposition (tile invariance, 5.5);
//  - a single-tile plan (Whole) keeps the image between sweeps, a multi-tile plan recomputes the chain per sweep;
//  - cancellation is polled between tiles and groups and by the ThreadPool chunk loop (CancelScope).
#include "engine/exec/cpu_executor.h"

#include <algorithm>
#include <cstring>

#include "parallel.h"

namespace dapple::engine {

namespace {

struct Step {
  const CpuImpl* impl = nullptr;
  Resolved r;
  NodeType type = NodeType::Source;
};

template <class T>
std::shared_ptr<T> as(const std::shared_ptr<void>& p) {
  return std::static_pointer_cast<T>(p);
}

struct Run {
  const Plan& plan;
  ExecEnv& env;
  ExecStats* stats;
  FrameKernels& fk;
  int W, H;
  ScratchFrame frameBuf;        // Whole mode: the one float frame the window nodes work on
  ImageF filmPlane;             // FilmSim output (reused as the float frame when windows follow)
  ScratchFrame filmScratch;
  ImageF* film = nullptr;
  ImageF tileBuf, cropBuf;      // tiled mode
  SpatialScratch scratch;       // luminance plane etc., kept between window nodes like applySpatial
  std::vector<Step> head, wins; // wins: every window node of the final sweep, in chain order
  std::vector<NodeType> winTypes;
  std::shared_ptr<DehazeSolve> dehaze;
  std::shared_ptr<GuidedSolve> guided;
  std::vector<float> gridLow;   // accumulated 1/4 grid of the sweep in flight
  int gridW = 0, gridH = 0;
  bool single = true;
  ImageF* img = nullptr;        // persistent frame (single-tile plans)
  bool imgValid = false;
  std::size_t done = 0;         // window nodes already applied to the persistent frame

  Run(const Plan& p, ExecEnv& e, ExecStats* s)
      : plan(p), env(e), stats(s), fk(e.fk), W(e.fk.width()), H(e.fk.height()) {}

  bool cancelled() const { return env.cancel.cancelled(); }

  // ---- binding ------------------------------------------------------------------------------------------------------------
  bool bind(const Stage& sweep) {
    FrameEnv fe;
    fe.fk = &fk;
    fe.film = film;
    for (NodeId id : sweep.groups[0].nodes)
      if (env.graph.nodes()[id].type == NodeType::Blend) fe.blendFollows = true;
    head.clear();
    for (NodeId id : sweep.groups[0].nodes) {
      Step s;
      s.type = env.graph.nodes()[id].type;
      s.impl = cpuImpl(s.type);
      if (!s.impl || !s.impl->block || !bindResolved(s.type, fe, s.r)) return false;
      head.push_back(s);
    }
    // fusion recipe: the three leading fragments become one pass over the block (same operations, same order)
    // (an Hdr node after the Gain is part of the fused pass: headGainTone applies the HDR gain between the two)
    const std::size_t nf = head.size() >= 4 && head[2].type == NodeType::Hdr ? 4 : 3;
    if (head.size() >= nf && fk.canFuseHead() && (head[0].type == NodeType::Resize || head[0].type == NodeType::Geometry ||
                                                  head[0].type == NodeType::Source) &&
        head[1].type == NodeType::Gain && head[nf - 1].type == NodeType::ToneColour) {
      Step f = head[0];
      f.impl = cpuFusedHead();
      f.type = NodeType::ToneColour;
      head.erase(head.begin(), head.begin() + long(nf));
      head.insert(head.begin(), f);
    }
    return true;
  }
  bool bindWindows(const Stage& last) {
    FrameEnv fe;
    fe.fk = &fk;
    fe.film = film;
    wins.clear();
    for (std::size_t g = 1; g < last.groups.size(); ++g)
      for (NodeId id : last.groups[g].nodes) {
        Step s;
        s.type = env.graph.nodes()[id].type;
        s.impl = cpuImpl(s.type);
        if (!s.impl || !s.impl->tile || !bindResolved(s.type, fe, s.r)) return false;
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
          std::uint8_t* o = dst ? nullptr : env.out + (std::size_t(y) * W + rx0) * ch;
          for (int x = 0; x < rw; x += 256) {
            const int n = std::min(256, rw - x);
            float px[256 * 3];
            const PixelPos pos{rx0 + x, y};
            for (const Step& s : head) s.impl->block(s.r.bytes, px, n, pos);
            if (drow) {  // clamp into the frame
              float* d = drow + 3 * std::size_t(x);
              for (int i = 0; i < n * 3; ++i) d[i] = std::clamp(px[i], 0.f, 1.f);
            } else {  // clamp + 8-bit encode (Pack8 epilogue)
              for (int i = 0; i < n; ++i) {
                const float r = std::clamp(px[3 * i], 0.f, 1.f), g = std::clamp(px[3 * i + 1], 0.f, 1.f),
                            b = std::clamp(px[3 * i + 2], 0.f, 1.f);
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
    t.dehaze = dehaze.get();
    t.guided = guided.get();
    for (std::size_t i = from; i < to; ++i) {
      if (cancelled()) return Status::Cancelled;
      const Status st = wins[i].impl->tile(t, wins[i].r.bytes);
      if (st != Status::Ok) return st;
    }
    return cancelled() ? Status::Cancelled : Status::Ok;
  }

  // ---- accumulate the 1/4 grid a global solve needs, from the interior of a tile -----------------------------------------------
  void accumulate(Accum kind, const ImageF& im, const Rect& in, int ox, int oy, bool whole, SpatialScratch& sc) {
    const int iw = in.x1 - in.x0, ih = in.y1 - in.y0;
    const ImageF* src = &im;
    if (!whole && kind == Accum::DehazeGrid) {  // the interior alone (its origin is on the 1/4 grid: the frame's blocks)
      if (cropBuf.data.size() < std::size_t(iw) * ih * 3) cropBuf.data.resize(std::size_t(iw) * ih * 3);
      cropBuf.width = iw;
      cropBuf.height = ih;
      for (int y = 0; y < ih; ++y)
        std::memcpy(cropBuf.row(y), im.row(in.y0 + y) + 3 * std::size_t(in.x0), std::size_t(iw) * 3 * sizeof(float));
      src = &cropBuf;
    }
    std::vector<float> low;
    int lw = 0, lh = 0;
    const int ch = kind == Accum::DehazeGrid ? 3 : 1;
    if (kind == Accum::DehazeGrid) {
      dehazeLowGrid(*src, low, lw, lh);
    } else {
      lumaGrid(im, sc, in.x0, in.y0, in.x1, in.y1, low, lw, lh);
    }
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
      std::memcpy(gridLow.data() + (std::size_t(gy + y) * gridW + gx) * ch, low.data() + std::size_t(y) * lw * ch,
                  std::size_t(lw) * ch * sizeof(float));
  }

  // ---- one TileSweep ----------------------------------------------------------------------------------------------------------
  Status sweep(const Stage& st) {
    if (stats) ++stats->sweeps;
    if (!bind(st)) return Status::Unsupported;
    const std::size_t nWin = st.groups.size() - 1;
    const int T = plan.grid.tile;
    gridW = gridH = 0;

    if (nWin == 0 && st.output && env.outF) {  // float frame straight from the chain (Whole plans)
      *env.outF = ImageF(W, H);
      runHead(0, 0, W, H, env.outF);
      if (stats) ++stats->tiles;
      return cancelled() ? Status::Cancelled : Status::Ok;
    }
    if (nWin == 0 && st.output) {  // no neighbourhood stage: the chain packs the 8-bit pixels itself
      for (int ty = 0; ty < H; ty += T)
        for (int tx = 0; tx < W; tx += T) {
          if (cancelled()) return Status::Cancelled;
          runHead(tx, ty, std::min(T, W - tx), std::min(T, H - ty), nullptr);
          if (stats) ++stats->tiles;
        }
      return cancelled() ? Status::Cancelled : Status::Ok;
    }

    if (single) {
      if (!imgValid) {
        img = film ? film : &frameBuf.get(W, H);
        if (st.groups[0].nodes.empty() == false) runHead(0, 0, W, H, img);
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
        CpuTile t;
        t.img = img;
        t.ox = t.oy = 0;
        t.frameW = W;
        t.frameH = H;
        t.interior = all;
        t.out8 = env.out;
        t.outChannels = env.outChannels;
        const CpuImpl* pack = cpuImpl(NodeType::Pack8);
        s = pack->tile(t, nullptr);
        if (s != Status::Ok) return s;
      }
      return cancelled() ? Status::Cancelled : Status::Ok;
    }

    // multi-tile: the chain is recomputed per sweep (5.4)
    for (int ty = 0; ty < H; ty += T)
      for (int tx = 0; tx < W; tx += T) {
        if (cancelled()) return Status::Cancelled;
        const Rect in{tx, ty, std::min(tx + T, W), std::min(ty + T, H)};
        const int x0 = std::max(0, in.x0 - st.halo.x), y0 = std::max(0, in.y0 - st.halo.y);
        const int x1 = std::min(W, in.x1 + st.halo.x), y1 = std::min(H, in.y1 + st.halo.y);
        const int rw = x1 - x0, rh = y1 - y0;
        if (tileBuf.data.size() < std::size_t(rw) * rh * 3) tileBuf.data.resize(std::size_t(rw) * rh * 3);
        tileBuf.width = rw;
        tileBuf.height = rh;
        runHead(x0, y0, rw, rh, &tileBuf);
        if (cancelled()) return Status::Cancelled;
        scratch.yValid = false;
        Status s = runWindows(tileBuf, x0, y0, 0, nWin, scratch);
        if (s != Status::Ok) return s;
        const Rect local{in.x0 - x0, in.y0 - y0, in.x1 - x0, in.y1 - y0};
        if (st.accum != Accum::None) accumulate(st.accum, tileBuf, local, x0, y0, false, scratch);
        if (st.output) {
          CpuTile t;
          t.img = &tileBuf;
          t.ox = x0;
          t.oy = y0;
          t.frameW = W;
          t.frameH = H;
          t.interior = local;
          t.out8 = env.out;
          t.outChannels = env.outChannels;
          s = cpuImpl(NodeType::Pack8)->tile(t, nullptr);
          if (s != Status::Ok) return s;
        }
        if (stats) ++stats->tiles;
      }
    return cancelled() ? Status::Cancelled : Status::Ok;
  }

  // ---- a GlobalSolve --------------------------------------------------------------------------------------------------------
  Status solve(const Stage& st) {
    const NodeInstance& n = env.graph.nodes()[st.solveNode];
    const SpatialParams& sp = fk.spatial();
    const float scale = fk.spatialScale();
    if (cancelled()) return Status::Cancelled;
    if (n.type == NodeType::Dehaze) {
      auto sv = std::make_shared<DehazeSolve>();
      dehazeSolveGrid(gridLow, gridW, gridH, sp.dehaze, scale, *sv);
      if (cancelled()) return Status::Cancelled;
      dehaze = sv;
      ResultCache::Entry e;
      e.key = e.family = n.key;
      e.payload = sv;
      e.bytes = sv->t.size() * 4;
      env.cache.put(NodeType::Dehaze, std::move(e));
    } else {
      auto gs = std::make_shared<GuidedSolve>();
      guidedSolveGrid(gridLow, gridW, gridH, scale, *gs);
      if (cancelled()) return Status::Cancelled;
      guided = gs;
      ResultCache::Entry e;
      e.key = e.family = n.key;
      e.payload = gs;
      e.bytes = (gs->a.size() + gs->b.size()) * 4;
      env.cache.put(NodeType::Detail, std::move(e));
    }
    if (stats) ++stats->solveBuilds;
    gridLow.clear();
    gridLow.shrink_to_fit();
    return Status::Ok;
  }

  // Solves the plan did not schedule because they are in the cache.
  Status loadCachedSolves() {
    bool dehazeStage = false, guidedStage = false;
    for (const Stage& s : plan.stages)
      if (s.kind == StageKind::GlobalSolve) {
        const NodeType t = env.graph.nodes()[s.solveNode].type;
        (t == NodeType::Dehaze ? dehazeStage : guidedStage) = true;
      }
    const SpatialParams& sp = fk.spatial();
    for (const NodeInstance& n : env.graph.nodes()) {
      if (n.type == NodeType::Dehaze && sp.dehaze > 0.f && !dehazeStage) {
        ResultCache::Entry* e = env.cache.find(NodeType::Dehaze, n.key);
        if (!e) return Status::Internal;
        dehaze = as<DehazeSolve>(e->payload);
        if (stats) ++stats->solveHits;
      }
      if (n.type == NodeType::Detail && sp.clarity != 0.f && !guidedStage) {
        ResultCache::Entry* e = env.cache.find(NodeType::Detail, n.key);
        if (!e) return Status::Internal;
        guided = as<GuidedSolve>(e->payload);
        if (stats) ++stats->solveHits;
      }
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
  if (env.cancel.cancelled()) return Status::Cancelled;

  // the last TileSweep lists every window node
  const Stage* lastSweep = nullptr;
  for (const Stage& s : plan.stages)
    if (s.kind == StageKind::TileSweep) lastSweep = &s;
  if (!lastSweep) return Status::Unsupported;

  // film: a whole-frame node before the sweeps (the streaming path finishes the frame by itself)
  for (const Stage& s : plan.stages) {
    if (s.kind != StageKind::FilmFrame) continue;
    if (!env.fk.exactVariant() && env.fk.filmFull() && env.outChannels == 4 && !env.outF && !env.fk.spatial().any()) {
      if (env.fk.streamFilm(env.out)) {
        if (stats) stats->streamed = true;
        return Status::Ok;
      }
    }
    r.film = &r.filmScratch.get(r.W, r.H);
    if (!env.fk.runFilm(*r.film)) return Status::Unsupported;  // the caller renders the frame with the legacy renderer
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
