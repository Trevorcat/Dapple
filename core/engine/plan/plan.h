#pragma once
// Plan: the Planner's output, the only thing an executor runs (docs/engine/pipeline-architecture.md 5.1, section 50).
//
//   stages = [ HostTable?  FilmFrame?  TileSweep  (GlobalSolve  TileSweep)*  ]
//
//   HostTable   the Table-domain nodes of the frame, with their cache decisions (Plan::cache); the session evaluates them.
//   FilmFrame   the Film-fusion nodes, run as one whole-frame stage before the sweeps.
//   TileSweep   one pass over the plan's tiles. groups[0] is the Head group (Head-fusion nodes: CPU block fragments, GPU one recipe pass),
//               then one Window group per neighbourhood / per-tile node, in chain order. A sweep lists its groups CUMULATIVELY (the prefix
//               of the window chain it covers): a multi-tile run recomputes the chain per sweep, a single-tile run keeps the image and skips
//               the groups it already ran. Its tiles carry the interior, the work rect (interior + the sweep's halo) and the source window
//               the head reads for the work rect; both executors run exactly these windows.
//   GlobalSolve a node's frame-global solve (NodeDesc::solve) from the 1/4 grid the previous sweep accumulated (Stage::accum). A solve in
//               the cache is not planned: the sweep that would have produced its grid disappears.
//
// Everything is derived from the node declarations: footprint class, fusion group, behaviour (identity, halo, inputRect), solve, work
// planes, precision. The planner has no list of node types.
#include <algorithm>
#include <cstddef>
#include <vector>

#include "engine/graph/graph.h"
#include "engine/graph/node.h"
#include "engine/graph/types.h"

namespace dapple::engine {

enum class ExecMode : std::uint8_t { Whole, TiledExport };
enum class StageKind : std::uint8_t { HostTable, FilmFrame, TileSweep, GlobalSolve };
enum class GroupKind : std::uint8_t { Head, Window, Table, Film };

struct Group {
  GroupKind kind = GroupKind::Head;
  std::vector<NodeId> nodes;       // graph nodes in execution order
  Halo halo;                       // Window: the node's neighbourhood
};

// One tile of a sweep: rects in frame pixels; `source` in the grid the head's input lives in (after a grid-scaling Resample such as Resize).
struct TileWin {
  Rect interior, work, source;
};

struct Stage {
  StageKind kind = StageKind::TileSweep;
  std::vector<Group> groups;
  Halo halo;                       // TileSweep: total halo of the window groups it covers (+ slack, rounded up to the grid alignment)
  Accum accum = Accum::None;       // TileSweep: grid accumulated from the interior of every tile at the end of the sweep
  NodeId solveNode = 0;            // GlobalSolve: the node whose solve this is (also its cache key owner); TileSweep with accum: the node
                                   // whose grid it accumulates
  bool output = false;             // TileSweep: the last one, writes the output through the sink
  Precision precision = Precision::F16;  // TileSweep: the working image (widest precision of its nodes)
  std::vector<TileWin> tiles;      // TileSweep: the windows both executors run, row-major
};

struct TileGrid {
  int frameW = 0, frameH = 0;
  int tile = 0;                    // interior size (multiple of align); == max(frameW, frameH) rounded up in Whole mode
  int align = 4;                   // 1/4 grids of the solves require tile origins on multiples of 4
  int nx = 1, ny = 1;
};

struct CacheOp {
  NodeId node = 0;
  NodeType type = NodeType::Source;
  Hash128 key;
  enum Kind : std::uint8_t { Lookup, Store } kind = Lookup;
  bool hit = false;                // Lookup: the result is in the cache now (the producer is pruned from the plan)
};

struct Alloc {                     // logical image / grid -> bytes and lifetime in stage indices
  const char* what = "";
  std::size_t bytes = 0;
  int firstStage = 0, lastStage = 0;
};

struct Plan {
  Hash128 structureHash;
  ExecMode mode = ExecMode::Whole;
  std::vector<Stage> stages;
  TileGrid grid;
  std::vector<Alloc> allocs;
  std::vector<CacheOp> cache;
  std::size_t peakCpuBytes = 0;
  NodeId sink = 0;                 // the node that writes the output (Footprint::Sink)
  bool hasFilm = false;            // FilmFrame stage present
  bool needAux = false;            // a node reads the side planes the head writes (NodeDesc::readsAux)
  bool unsupported = false;        // no executor can run this plan (reason in `why`)
  const char* why = "";
  int sweeps() const {
    int n = 0;
    for (const Stage& s : stages) n += s.kind == StageKind::TileSweep;
    return n;
  }
  int maxHalo() const {
    int h = 0;
    for (const Stage& s : stages) h = std::max({h, s.halo.x, s.halo.y});
    return h;
  }
  // The nodes of every group of every stage.
  template <class F>
  void forEachNode(F&& f) const {
    for (const Stage& s : stages)
      for (const Group& g : s.groups)
        for (NodeId id : g.nodes) f(id);
  }
};

// What the planner needs besides the graph.
struct PlanRequest {
  int frameW = 0, frameH = 0;
  ExecMode mode = ExecMode::Whole;
  std::size_t memBudget = 512u << 20;   // TiledExport: bytes the executor may add on top of the source and the output
  int tile = 0;                         // TiledExport: 0 = chosen from memBudget
  int maxSide = 0;                      // largest image side the executor can allocate (GPU image limit): tile + 2 halo fits; 0 = no limit
  FrameView view;                       // the frame's resolved blocks: identity, halos, solves, source windows
  bool filmTiles = false;               // a film frame may keep ExecMode::TiledExport (else it runs whole)
  bool device = false;                  // the solves are looked up among the executor's device copies (ResultCache device slots)
};

// What an executor did (tests, edit-bench, EngineStats).
struct ExecStats {
  int sweeps = 0, tiles = 0, solveHits = 0, solveBuilds = 0;
  bool streamed = false;          // full-strength film frame went through the streaming path
};

class ResultCache;

class Planner {
 public:
  // Graph must have keys (Graph::computeKeys). `cache` is only peeked, never modified.
  static Plan plan(const Graph& g, const PlanRequest& rq, const ResultCache* cache);
};

}  // namespace dapple::engine
