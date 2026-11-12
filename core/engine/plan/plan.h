#pragma once
// Plan: the Planner's output, the only thing an executor runs (docs/engine/pipeline-architecture.md 5.1).
//
// Plan structure for the main pipeline (non-film and film):
//
//   stages = [ HostTable?  FilmFrame?  TileSweep  (GlobalSolve  TileSweep)*  ]
//
//   HostTable   the table nodes of the frame (ColourLattice ... Analysis). Cache decisions are in Plan::cache; the evaluation
//               itself is EngineSession's (it owns the renderer inputs the evaluators need).
//   FilmFrame   FilmSim as a whole-frame node (film engineer's nodes replace it); Whole mode only.
//   TileSweep   one pass over the tiles of the frame. groups[0] is the PointChain head (point nodes fused into 256-px block
//               fragments: Resize/Geometry, Gain, ToneColour, DisplayStages, Blend, Vignette + the implicit clamp), the
//               following groups are Window/Point groups that run on the tile (interior + stage halo) in order. A sweep lists
//               its groups CUMULATIVELY (the prefix of the window chain it covers): a multi-tile run recomputes the chain per
//               sweep, a single-tile run keeps the image and skips the groups it already ran.
//   GlobalSolve a frame-global solve (dehaze airlight / transmission, guided-filter coefficients) from the 1/4 grid the
//               previous sweep accumulated (Stage::accum). A solve whose result is in the ResultCache is not planned: the sweep
//               that would have produced its grid disappears.
#include <cstddef>
#include <vector>

#include "engine/graph/graph.h"
#include "engine/graph/node.h"
#include "engine/graph/types.h"

namespace dapple::engine {

enum class ExecMode : std::uint8_t { Whole, TiledExport };
enum class StageKind : std::uint8_t { HostTable, FilmFrame, TileSweep, GlobalSolve };
enum class GroupKind : std::uint8_t { PointChain, Window, Table, Film };
enum class Accum : std::uint8_t { None, DehazeGrid, LumaGrid };  // what a TileSweep leaves behind for the GlobalSolve after it

struct Group {
  GroupKind kind = GroupKind::PointChain;
  std::vector<NodeId> nodes;       // graph nodes in execution order
  Halo inHalo;                     // pixels of input needed beyond the output window, accumulated over the group
};

struct Stage {
  StageKind kind = StageKind::TileSweep;
  std::vector<Group> groups;
  Halo halo;                       // TileSweep: total halo of the window groups it covers (rounded up to the grid alignment)
  Accum accum = Accum::None;       // TileSweep: grid accumulated from the interior of every tile at the end of the sweep
  NodeId solveNode = 0;            // GlobalSolve: the Dehaze / Detail node whose solve this is (also its cache key owner)
  bool output = false;             // TileSweep: the last one, packs the 8-bit result (Pack8 epilogue)
  bool packInChain = false;        // TileSweep with output and no window group: the chain writes the 8-bit pixels directly
};

struct TileGrid {
  int frameW = 0, frameH = 0;
  int tile = 0;                    // interior size (multiple of align); == max(frameW, frameH) rounded up in Whole mode
  int align = 4;                   // 1/4 grids of dehaze / guided filter / chroma require tile origins on multiples of 4
  int nx = 1, ny = 1;
};

struct CacheOp {
  NodeId node = 0;
  NodeType type = NodeType::Source;
  Hash128 key;
  enum Kind : std::uint8_t { Lookup, Store } kind = Lookup;
  bool hit = false;                // Lookup: the result is in the cache now (the producer is pruned from the plan)
};

struct Alloc {                     // logical image / grid -> bytes and lifetime in stage indices (aliasing by the executor's pool)
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
  bool hasFilm = false;            // FilmFrame stage present (the film plane is a whole float frame)
  bool unsupported = false;        // an executor cannot run this plan (reason in `why`)
  const char* why = "";
  // convenience for diagnostics / tests
  int sweeps() const {
    int n = 0;
    for (const Stage& s : stages) n += s.kind == StageKind::TileSweep;
    return n;
  }
};

// What the planner needs to know about the frame besides the graph (all resolved by FrameKernels / the C layer).
struct PlanRequest {
  int frameW = 0, frameH = 0;
  ExecMode mode = ExecMode::Whole;
  std::size_t memBudget = 512u << 20;   // TiledExport: bytes the executor may add on top of the source and the output
  int tile = 0;                         // TiledExport: 0 = chosen from memBudget
  // resolved neighbourhood parameters (SpatialParams after quality / region / film), scale = radii scale
  float nrLuma = 0, nrColor = 0, dehaze = 0, clarity = 0, texture = 0, sharpen = 0, sharpRadius = 1, grain = 0, scale = 1;
  bool vignette = false, useGeo = false;
  int outChannels = 4;                  // 4 = RGBA8 (alpha 255), 3 = RGB8
};

// What an executor did (tests, edit-bench, EngineStats).
struct ExecStats {
  int sweeps = 0, tiles = 0, solveHits = 0, solveBuilds = 0;
  bool streamed = false;          // full-strength film frame went through the streaming path
};

class ResultCache;

class Planner {
 public:
  // Graph must have keys (Graph::computeKeys). `cache` is only read (peek), never modified.
  static Plan plan(const Graph& g, const PlanRequest& rq, const ResultCache* cache);
  // Neighbourhood of the window nodes in pixels of the frame grid (declared halo of NodeBehaviour::halo for the CPU nodes).
  static Halo windowHalo(NodeType t, const PlanRequest& rq);
};

}  // namespace dapple::engine
