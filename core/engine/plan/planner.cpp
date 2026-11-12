// Planner: fusion of point nodes into block-fragment chains, tiling + halo propagation, sweep structure for the frame-global
// parts, cache decisions (docs/engine/pipeline-architecture.md 5.1 - 5.4, 5.7). Backend independent: no kernel is called.
#include "engine/plan/plan.h"

#include <algorithm>
#include <cmath>

#include "engine/exec/result_cache.h"

namespace dapple::engine {

namespace {

int roundUp(int v, int a) { return (v + a - 1) / a * a; }

bool isTable(NodeType t) { return NodeRegistry::instance().desc(t).output == Domain::Table; }

}  // namespace

// Neighbourhood of the window nodes, in pixels of the frame grid. The running-sum box blurs of spatial.cpp read `r` pixels
// per pass in each direction; the 1/4-grid parts add the block size of the grid on both sides. The frame-global parts
// (dehaze airlight / transmission, guided-filter coefficients) are NOT halos: they are solved on the whole frame's 1/4 grid
// (GlobalSolve) and looked up at absolute coordinates, so Dehaze and the clarity part of Detail are pointwise.
Halo Planner::windowHalo(NodeType t, const PlanRequest& rq) {
  const float s = rq.scale;
  switch (t) {
    case NodeType::NRLuma: {
      const int r = std::max(1, int(std::lround((0.8f + 1.8f * rq.nrLuma) * s)));
      return {2 * r, 2 * r};  // boxBlur(r, 2 iterations)
    }
    case NodeType::NRChroma: {
      const float rFull = (1.5f + 5.f * rq.nrColor) * s;
      const int f = rFull >= 6.f ? 4 : 2;
      const int rl = std::max(1, int(std::lround(rFull / float(f))));
      const int h = (2 * rl + 2) * f;  // box on the reduced grid (2 iterations) + block + bilinear upsample
      return {h, h};
    }
    case NodeType::Detail: {
      int h = 0;
      if (rq.texture != 0.f) h = std::max(h, 2 * std::max(1, int(std::lround(2.f * s))));
      if (rq.sharpen > 0.f) {
        const float sigma = std::max(0.4f, rq.sharpRadius * s * 0.5f);
        if (sigma > 2.6f) {
          const int r = std::max(1, int(std::lround(std::sqrt(sigma * sigma * 4.f / 3.f + 0.25f) - 0.5f)));
          h = std::max(h, 3 * r);
        } else {
          h = std::max(h, std::max(1, int(std::ceil(sigma * 3.f))));
        }
      }
      return {h, h};
    }
    default:
      return {0, 0};
  }
}

Plan Planner::plan(const Graph& g, const PlanRequest& rq, const ResultCache* cache) {
  Plan p;
  p.structureHash = g.structureHash();
  p.mode = rq.mode;
  const NodeRegistry& reg = NodeRegistry::instance();

  // ---- classify -------------------------------------------------------------------------------------------------------
  Group tables{GroupKind::Table, {}, {}};
  const NodeInstance *nResize = nullptr, *nGeo = nullptr, *nGain = nullptr, *nTone = nullptr, *nFilm = nullptr,
                     *nDisp = nullptr, *nBlend = nullptr, *nVig = nullptr, *nPack = nullptr;
  std::vector<const NodeInstance*> windows;
  for (const NodeInstance& n : g.nodes()) {
    if (isTable(n.type)) {
      tables.nodes.push_back(n.id);
      continue;
    }
    switch (n.type) {
      case NodeType::Resize: nResize = &n; break;
      case NodeType::Geometry: nGeo = &n; break;
      case NodeType::Gain: nGain = &n; break;
      case NodeType::ToneColour:
      case NodeType::ToneColourExact: nTone = &n; break;
      case NodeType::FilmSim: nFilm = &n; break;
      case NodeType::DisplayStages: nDisp = &n; break;
      case NodeType::Blend: nBlend = &n; break;
      case NodeType::Vignette: nVig = &n; break;
      case NodeType::NRLuma:
      case NodeType::NRChroma:
      case NodeType::Dehaze:
      case NodeType::Detail:
      case NodeType::Grain: windows.push_back(&n); break;
      case NodeType::Pack8: nPack = &n; break;
      case NodeType::LegacyRender:
        p.unsupported = true;
        p.why = "LegacyRender super-node has no CPU plan (use the Full graph)";
        return p;
      default: break;
    }
  }
  p.hasFilm = nFilm != nullptr;

  // ---- tables ---------------------------------------------------------------------------------------------------------
  if (!tables.nodes.empty()) {
    Stage st;
    st.kind = StageKind::HostTable;
    for (NodeId id : tables.nodes) {
      const NodeInstance& n = g.nodes()[id];
      CacheOp op;
      op.node = id;
      op.type = n.type;
      op.key = n.key;
      op.kind = CacheOp::Lookup;
      if (cache) {
        if (n.type == NodeType::ColourLattice) {  // any lattice of this colour stage at least as fine as asked for
          for (const auto& e : cache->entries(n.type))
            if (e.family == n.family) op.hit = true;
        } else {
          op.hit = cache->peek(n.type, n.key) != nullptr;
        }
      }
      p.cache.push_back(op);
    }
    st.groups.push_back(std::move(tables));
    p.stages.push_back(std::move(st));
  }

  // ---- film (whole-frame node, Whole mode only) -----------------------------------------------------------------------------
  if (nFilm) {
    Stage st;
    st.kind = StageKind::FilmFrame;
    Group gf{GroupKind::Film, {}, {}};
    if (nResize) gf.nodes.push_back(nResize->id);
    if (nGeo) gf.nodes.push_back(nGeo->id);
    gf.nodes.push_back(nFilm->id);
    st.groups.push_back(std::move(gf));
    p.stages.push_back(std::move(st));
  }

  // ---- the point chain (fused block fragments) ---------------------------------------------------------------------------
  Group head{GroupKind::PointChain, {}, {}};
  if (nGain) {  // the normal branch: its Resample head, then gain + tone/colour
    head.nodes.push_back(nGeo ? nGeo->id : (nResize ? nResize->id : 0));
    head.nodes.push_back(nGain->id);
    if (nTone) head.nodes.push_back(nTone->id);
  }
  if (nDisp) head.nodes.push_back(nDisp->id);
  if (nBlend) head.nodes.push_back(nBlend->id);
  if (nVig) head.nodes.push_back(nVig->id);

  // ---- window chain, solves and sweeps -----------------------------------------------------------------------------------------
  const int align = 4;
  const NodeInstance *nDehaze = nullptr, *nDetail = nullptr;
  for (const NodeInstance* w : windows) {
    if (w->type == NodeType::Dehaze) nDehaze = w;
    if (w->type == NodeType::Detail) nDetail = w;
  }
  auto cached = [&](const NodeInstance* n) { return cache && n && cache->peek(n->type, n->key) != nullptr; };
  const bool dehazeSolve = nDehaze && rq.dehaze > 0.f;  // amount < 0 (veil) is pointwise
  const bool guidedSolve = nDetail && rq.clarity != 0.f;
  const bool dehazeHit = dehazeSolve && cached(nDehaze), guidedHit = guidedSolve && cached(nDetail);
  for (const NodeInstance* n : {nDehaze, nDetail}) {
    if (!n) continue;
    const bool needs = n == nDehaze ? dehazeSolve : guidedSolve;
    if (!needs) continue;
    CacheOp op;
    op.node = n->id;
    op.type = n->type;
    op.key = n->key;
    op.kind = CacheOp::Lookup;
    op.hit = n == nDehaze ? dehazeHit : guidedHit;
    p.cache.push_back(op);
    if (!op.hit) {
      op.kind = CacheOp::Store;
      p.cache.push_back(op);
    }
  }

  // tile grid
  p.grid.frameW = rq.frameW;
  p.grid.frameH = rq.frameH;
  p.grid.align = align;
  ExecMode mode = rq.mode;
  if (mode == ExecMode::TiledExport && nFilm) {
    mode = ExecMode::Whole;  // the film plane is a whole float frame until the film nodes tile (film engineer)
    p.why = "film frame runs whole";
  }
  p.mode = mode;

  // total halo if every window ran in one sweep (used to size the tiles)
  Halo allHalo;
  for (const NodeInstance* w : windows) {
    const Halo h = windowHalo(w->type, rq);
    allHalo.x += h.x;
    allHalo.y += h.y;
  }
  allHalo.x = roundUp(allHalo.x, align);
  allHalo.y = roundUp(allHalo.y, align);

  const int longEdge = std::max(rq.frameW, rq.frameH);
  if (mode == ExecMode::Whole) {
    p.grid.tile = roundUp(longEdge, align);
  } else if (rq.tile > 0) {
    p.grid.tile = std::max(roundUp(rq.tile, align), 64);
  } else {
    // bytes per window pixel of a tile: float RGB image + luminance/work planes of the spatial stages (measured in the
    // executor: ~7 float planes) + the head chain's row buffers
    const std::size_t bpp = windows.empty() ? 0 : 3 * 4 + 6 * 4 + 4;
    int t = 4096;
    while (t > 256) {
      const std::size_t side = std::size_t(t) + 2 * std::size_t(std::max(allHalo.x, allHalo.y));
      if (side * side * bpp <= rq.memBudget) break;
      t /= 2;
    }
    p.grid.tile = t;
  }
  p.grid.nx = (rq.frameW + p.grid.tile - 1) / p.grid.tile;
  p.grid.ny = (rq.frameH + p.grid.tile - 1) / p.grid.tile;
  const bool singleTile = p.grid.nx == 1 && p.grid.ny == 1;

  auto sweepHalo = [&](std::size_t nWin) {
    Halo h;
    for (std::size_t i = 0; i < nWin; ++i) {
      const Halo w = windowHalo(windows[i]->type, rq);
      h.x += w.x;
      h.y += w.y;
    }
    h.x = roundUp(h.x, align);
    h.y = roundUp(h.y, align);
    return h;
  };
  auto windowGroups = [&](std::size_t nWin) {
    std::vector<Group> gs;
    gs.push_back(head);
    for (std::size_t i = 0; i < nWin; ++i) {
      Group w{GroupKind::Window, {windows[i]->id}, windowHalo(windows[i]->type, rq)};
      gs.push_back(std::move(w));
    }
    return gs;
  };
  auto indexOf = [&](const NodeInstance* n) {
    for (std::size_t i = 0; i < windows.size(); ++i)
      if (windows[i] == n) return i;
    return windows.size();
  };

  // accumulation sweeps, in chain order: the grid a solve needs is that of the image at its input
  struct Cut {
    std::size_t before;
    Accum accum;
    const NodeInstance* node;
  };
  std::vector<Cut> cuts;
  if (dehazeSolve && !dehazeHit) cuts.push_back({indexOf(nDehaze), Accum::DehazeGrid, nDehaze});
  if (guidedSolve && !guidedHit) cuts.push_back({indexOf(nDetail), Accum::LumaGrid, nDetail});
  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.before < b.before; });
  for (const Cut& c : cuts) {
    Stage sw;
    sw.kind = StageKind::TileSweep;
    sw.groups = windowGroups(c.before);
    sw.halo = sweepHalo(c.before);
    sw.accum = c.accum;
    p.stages.push_back(std::move(sw));
    Stage so;
    so.kind = StageKind::GlobalSolve;
    so.solveNode = c.node->id;
    so.accum = c.accum;
    p.stages.push_back(std::move(so));
  }
  {
    Stage fin;
    fin.kind = StageKind::TileSweep;
    fin.groups = windowGroups(windows.size());
    fin.halo = sweepHalo(windows.size());
    fin.output = true;
    fin.packInChain = windows.empty() && nPack != nullptr;
    p.stages.push_back(std::move(fin));
  }
  (void)reg;

  // ---- memory estimate + lifetimes --------------------------------------------------------------------------------------------
  const std::size_t fullPx = std::size_t(rq.frameW) * rq.frameH;
  const int last = int(p.stages.size()) - 1;
  const int firstSweep = [&] {
    for (int i = 0; i < int(p.stages.size()); ++i)
      if (p.stages[std::size_t(i)].kind == StageKind::TileSweep) return i;
    return 0;
  }();
  if (nFilm) p.allocs.push_back({"film plane", fullPx * 12, 0, last});
  if (windows.empty()) {
    p.allocs.push_back({"head rows", std::size_t(256) * 12 * 8, firstSweep, last});
  } else if (singleTile) {
    p.allocs.push_back({"frame float", fullPx * 12, firstSweep, last});
    p.allocs.push_back({"luma/work planes", fullPx * 4 * 5, firstSweep, last});
  } else {
    const std::size_t side = std::size_t(p.grid.tile) + 2 * std::size_t(std::max(allHalo.x, allHalo.y));
    p.allocs.push_back({"tile float", side * side * 12, firstSweep, last});
    p.allocs.push_back({"tile luma/work planes", side * side * 4 * 6, firstSweep, last});
    p.allocs.push_back({"1/4 grids", (fullPx / 16) * (12 + 4 + 8) + 64, firstSweep, last});
  }
  for (const Alloc& a : p.allocs) p.peakCpuBytes += a.bytes;
  return p;
}

}  // namespace dapple::engine
