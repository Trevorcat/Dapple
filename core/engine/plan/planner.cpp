// Planner (docs/engine/pipeline-architecture.md 5.1 - 5.4, 5.7, section 50). Backend independent: no kernel is called, no node type is
// named. Every decision comes from the node declarations (NodeDesc):
//   - identity (behaviour over the frame's block): the node is elided, its consumers read its input;
//   - Domain::Table: HostTable stage with cache lookups (by family when the node has an order key);
//   - Fusion::Film: the FilmFrame stage;  Fusion::Head: the head group of every sweep (a Resample node joins it when it feeds a head node);
//   - Footprint::Sink: the output;  anything else: a window group with the node's halo (behaviour), solve (NodeDesc::solve) and work planes;
//   - tiles: sweep halos = sum of the covered nodes' halos (+ slack), tile size from the memory budget and the executor's image limit,
//     source windows through the head's Resample nodes (behaviour inputRect) up to a grid-scaling one; precision = the widest of a sweep.
#include "engine/plan/plan.h"

#include <algorithm>
#include <cmath>

#include "engine/exec/result_cache.h"

namespace dapple::engine {

namespace {

int roundUp(int v, int a) { return (v + a - 1) / a * a; }

// Slack around a sweep that reads neighbours: the 1/4-grid blocks of the solve lookups and the GPU kernels' sampling reach.
constexpr int kSweepSlack = 8;
constexpr int kAlign = 4;

struct Win {
  const NodeInstance* n;
  const NodeDesc* d;
  const void* block;
  Halo halo;
  Accum accum;
  int planes;
};

}  // namespace

Plan Planner::plan(const Graph& g, const PlanRequest& rq, const ResultCache* cache) {
  Plan p;
  p.structureHash = g.structureHash();
  p.mode = rq.mode;
  const NodeRegistry& reg = NodeRegistry::instance();
  const auto& nodes = g.nodes();
  auto fail = [&](const char* why) {
    p.unsupported = true;
    p.why = why;
    return p;
  };

  // ---- elision + consumers ------------------------------------------------------------------------------------------------
  std::vector<char> live(nodes.size(), 1);
  for (const NodeInstance& n : nodes)
    if (reg.identity(n.type, rq.view)) live[n.id] = 0;
  std::vector<std::vector<NodeId>> consumers(nodes.size());
  for (const NodeInstance& n : nodes)
    for (const Edge& e : n.in) consumers[e.from].push_back(n.id);
  // the live consumers of a node (through elided ones)
  auto liveConsumers = [&](NodeId id) {
    std::vector<NodeId> out, todo = consumers[id];
    while (!todo.empty()) {
      const NodeId c = todo.back();
      todo.pop_back();
      if (live[c]) out.push_back(c);
      else todo.insert(todo.end(), consumers[c].begin(), consumers[c].end());
    }
    return out;
  };

  // ---- classify -------------------------------------------------------------------------------------------------------------
  Group tables{GroupKind::Table, {}, {}}, film{GroupKind::Film, {}, {}}, head{GroupKind::Head, {}, {}};
  std::vector<const NodeInstance*> headResample;
  std::vector<Win> windows;
  bool haveSink = false;
  for (const NodeInstance& n : nodes) {
    if (!live[n.id]) continue;
    const NodeDesc& d = reg.desc(n.type);
    if (d.output == Domain::Table) {
      tables.nodes.push_back(n.id);
      CacheOp op;
      op.node = n.id;
      op.type = n.type;
      op.key = n.key;
      if (cache) op.hit = d.orderKey != kNoKey ? cache->containsFamily(slotOf(n.type), n.family) : cache->contains(slotOf(n.type), n.key);
      p.cache.push_back(op);
      continue;
    }
    if (d.footprint == Footprint::Source) continue;
    if (d.footprint == Footprint::Sink) {
      p.sink = n.id;
      haveSink = true;
      continue;
    }
    if (d.fusion == Fusion::Film) {
      film.nodes.push_back(n.id);
      continue;
    }
    if (d.fusion == Fusion::Head) {
      if (d.footprint == Footprint::Resample) {  // joins the head when it feeds a head node that is not itself a resampling
        bool feeds = false;
        for (NodeId c : liveConsumers(n.id)) {
          const NodeDesc& cd = reg.desc(nodes[c].type);
          feeds |= cd.fusion == Fusion::Head && cd.footprint != Footprint::Resample;
        }
        if (!feeds) continue;
        headResample.push_back(&n);
      }
      if (!windows.empty()) return fail("a head (point) node after a window node");
      head.nodes.push_back(n.id);
      continue;
    }
    const void* blk = reg.block(n.type, rq.view);
    Win w{&n, &d, blk, {}, Accum::None, 0};
    if (d.behaviour && d.behaviour->halo && blk) w.halo = d.behaviour->halo(blk);
    if (d.solve && blk) w.accum = d.solve(blk);
    if (d.workPlanes && blk) w.planes = d.workPlanes(blk);
    if (d.readsAux) p.needAux = true;
    windows.push_back(w);
  }
  if (!haveSink) return fail("graph without a sink");
  p.hasFilm = !film.nodes.empty();

  if (!tables.nodes.empty()) {
    Stage st;
    st.kind = StageKind::HostTable;
    st.groups.push_back(std::move(tables));
    p.stages.push_back(std::move(st));
  }
  if (p.hasFilm) {
    Stage st;
    st.kind = StageKind::FilmFrame;
    st.groups.push_back(std::move(film));
    p.stages.push_back(std::move(st));
  }

  // ---- frame-global solves: cache lookups, accumulation cuts ----------------------------------------------------------------------
  struct Cut {
    std::size_t before;
    Accum accum;
    NodeId node;
  };
  std::vector<Cut> cuts;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const Win& w = windows[i];
    if (w.accum == Accum::None) continue;
    CacheOp op;
    op.node = w.n->id;
    op.type = w.n->type;
    op.key = w.n->key;
    op.hit = cache && cache->contains(slotOf(w.n->type, rq.device), w.n->key);
    p.cache.push_back(op);
    if (!op.hit) {
      op.kind = CacheOp::Store;
      p.cache.push_back(op);
      cuts.push_back({i, w.accum, w.n->id});
    }
  }

  // ---- tile grid ------------------------------------------------------------------------------------------------------------------
  p.grid.frameW = rq.frameW;
  p.grid.frameH = rq.frameH;
  p.grid.align = kAlign;
  ExecMode mode = rq.mode;
  if (mode == ExecMode::TiledExport && p.hasFilm && !rq.filmTiles) {
    mode = ExecMode::Whole;
    p.why = "film frame runs whole";
  }
  p.mode = mode;

  auto sweepHalo = [&](std::size_t nWin) {
    Halo h;
    bool reads = false;
    for (std::size_t i = 0; i < nWin; ++i) {
      h.x += windows[i].halo.x;
      h.y += windows[i].halo.y;
      reads |= windows[i].halo.x > 0 || windows[i].halo.y > 0 || windows[i].accum != Accum::None;
    }
    if (reads) h.x += kSweepSlack, h.y += kSweepSlack;
    return Halo{roundUp(h.x, kAlign), roundUp(h.y, kAlign)};
  };
  const Halo allHalo = sweepHalo(windows.size());
  int maxWork = 0, headPlanes = 0;
  for (const Win& w : windows) maxWork = std::max(maxWork, w.planes);
  for (NodeId id : head.nodes) {
    const NodeDesc& d = reg.desc(nodes[id].type);
    const void* blk = reg.block(nodes[id].type, rq.view);
    if (d.workPlanes && blk) headPlanes += d.workPlanes(blk);
  }
  // bytes per pixel of a work window: the float RGB image + the scratch planes of the widest window node + the head's side planes
  const std::size_t bpp = 12 + 4 * std::size_t(maxWork) + 4 * std::size_t(headPlanes);
  const int longEdge = std::max(rq.frameW, rq.frameH);
  const int haloMax = std::max(allHalo.x, allHalo.y);
  if (mode == ExecMode::Whole) {
    p.grid.tile = roundUp(longEdge, kAlign);
  } else if (rq.tile > 0) {
    p.grid.tile = std::max(roundUp(rq.tile, kAlign), 64);
  } else {
    int t = 4096;
    while (t > 256) {
      const std::size_t side = std::size_t(t) + 2 * std::size_t(haloMax);
      if (side * side * bpp <= rq.memBudget) break;
      t /= 2;
    }
    p.grid.tile = t;
  }
  if (mode == ExecMode::TiledExport && rq.maxSide > 0) {  // the executor's image limit holds the work window
    while (p.grid.tile + 2 * haloMax > rq.maxSide && p.grid.tile > 64) p.grid.tile = std::max(64, (p.grid.tile / 2) / kAlign * kAlign);
    if (p.grid.tile + 2 * haloMax > rq.maxSide) return fail("image limit too small for the halo");
  }
  p.grid.nx = (rq.frameW + p.grid.tile - 1) / p.grid.tile;
  p.grid.ny = (rq.frameH + p.grid.tile - 1) / p.grid.tile;
  const bool singleTile = p.grid.nx == 1 && p.grid.ny == 1;

  // source window of a work rect: through the head's Resample nodes, last to first, up to a grid-scaling one
  auto sourceOf = [&](const Rect& work) {
    Rect r = work;
    for (auto it = headResample.rbegin(); it != headResample.rend(); ++it) {
      const NodeDesc& d = reg.desc((*it)->type);
      if (d.rescales) break;
      const void* blk = reg.block((*it)->type, rq.view);
      if (d.behaviour && d.behaviour->inputRect && blk) r = d.behaviour->inputRect(blk, r);
    }
    return r;
  };
  auto tilesOf = [&](const Halo& h) {
    std::vector<TileWin> ts;
    const int T = p.grid.tile;
    for (int ty = 0; ty < rq.frameH; ty += T)
      for (int tx = 0; tx < rq.frameW; tx += T) {
        TileWin t;
        t.interior = Rect{tx, ty, std::min(tx + T, rq.frameW), std::min(ty + T, rq.frameH)};
        t.work = Rect{std::max(0, t.interior.x0 - h.x), std::max(0, t.interior.y0 - h.y), std::min(rq.frameW, t.interior.x1 + h.x),
                      std::min(rq.frameH, t.interior.y1 + h.y)};
        t.source = sourceOf(t.work);
        ts.push_back(t);
      }
    return ts;
  };
  auto precisionOf = [&](const std::vector<Group>& gs) {
    Precision pr = Precision::F16;
    for (const Group& gr : gs)
      for (NodeId id : gr.nodes) pr = std::max(pr, reg.desc(nodes[id].type).precision);
    return pr;
  };
  auto sweep = [&](std::size_t nWin) {
    Stage st;
    st.kind = StageKind::TileSweep;
    st.groups.push_back(head);
    for (std::size_t i = 0; i < nWin; ++i) st.groups.push_back(Group{GroupKind::Window, {windows[i].n->id}, windows[i].halo});
    st.halo = sweepHalo(nWin);
    st.precision = precisionOf(st.groups);
    st.tiles = tilesOf(st.halo);
    return st;
  };

  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.before < b.before; });
  for (const Cut& c : cuts) {
    Stage sw = sweep(c.before);
    sw.accum = c.accum;
    sw.solveNode = c.node;
    p.stages.push_back(std::move(sw));
    Stage so;
    so.kind = StageKind::GlobalSolve;
    so.solveNode = c.node;
    so.accum = c.accum;
    p.stages.push_back(std::move(so));
  }
  {
    Stage fin = sweep(windows.size());
    fin.output = true;
    p.stages.push_back(std::move(fin));
  }

  // ---- memory estimate + lifetimes (CPU executor) ---------------------------------------------------------------------------------
  const std::size_t fullPx = std::size_t(rq.frameW) * rq.frameH;
  const int last = int(p.stages.size()) - 1;
  int firstSweep = 0;
  while (firstSweep < last && p.stages[std::size_t(firstSweep)].kind != StageKind::TileSweep) ++firstSweep;
  if (p.hasFilm) p.allocs.push_back({"film plane", fullPx * 12, 0, last});
  if (windows.empty()) {
    p.allocs.push_back({"head rows", std::size_t(256) * 12 * 8, firstSweep, last});
  } else {
    const std::size_t px = singleTile ? fullPx : (std::size_t(p.grid.tile) + 2 * std::size_t(haloMax)) * (std::size_t(p.grid.tile) + 2 * std::size_t(haloMax));
    p.allocs.push_back({singleTile ? "frame float" : "tile float", px * 12, firstSweep, last});
    if (maxWork) p.allocs.push_back({"work planes", px * 4 * std::size_t(maxWork), firstSweep, last});
    if (headPlanes) p.allocs.push_back({"head side planes", px * 4 * std::size_t(headPlanes), firstSweep, last});
    if (!cuts.empty() && !singleTile) {
      std::size_t cell = 0;
      for (const Cut& c : cuts) cell += c.accum == Accum::ColourGrid ? 12 + 4 : 4 + 8;  // the grid and its solve
      p.allocs.push_back({"1/4 grids", (fullPx / 16 + 1) * cell, firstSweep, last});
    }
  }
  for (const Alloc& a : p.allocs) p.peakCpuBytes += a.bytes;
  return p;
}

}  // namespace dapple::engine
