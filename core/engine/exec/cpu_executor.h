#pragma once
// CpuExecutor: runs a Plan on the CPU (docs/engine/pipeline-architecture.md 5.2).
//
// The executor knows no node: a head group runs the CpuImpl::block fragments of its nodes (fused when the lead node's CpuImpl::fuse says
// so), a window group the CpuImpl::tile of its node, a GlobalSolve the CpuImpl::solve of its node, the output the sink node's tile. Each
// node binds its own slot for the frame (NodeDesc::bind). The tiles and their source windows are the plan's (Stage::tiles), the same the
// GPU executor runs.
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "dapple/image.h"
#include "engine/exec/result_cache.h"
#include "engine/graph/graph.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_blocks.h"
#include "engine/nodes/point/cpu.h"
#include "engine/plan/plan.h"
#include "local_eval.h"
#include "spatial.h"

namespace dapple::engine {

// Latest-wins cancellation (5.8): a request carries the ticket current at submit time; the executors poll it between tiles,
// between groups and, through ThreadPool::CancelScope, between pool chunks.
struct CancelToken {
  const std::atomic<std::uint64_t>* current = nullptr;
  std::uint64_t mine = 0;
  bool cancelled() const { return current && current->load(std::memory_order_relaxed) != mine; }
};

// A window of the frame handed to a window / sink node (CpuImpl::tile).
struct CpuTile {
  ImageF* img = nullptr;          // interior + halo, in place
  SpatialScratch* scratch = nullptr;
  int ox = 0, oy = 0;             // origin of img in the frame
  int frameW = 0, frameH = 0;
  const void* solved = nullptr;   // the node's frame-global solve (CpuImpl::solve payload), null when it needs none
  // sink only: the interior rect (in img coordinates) goes to out8 at frame position (ox + interior.x0, ...)
  Rect interior;
  std::uint8_t* out8 = nullptr;
  int outChannels = 4;
  int outRow0 = 0;                // frame row of out8's first row (streaming export: out8 holds the band in flight)
  // the per-pixel side planes the head chain wrote for this window (Plan::needAux; local adjustments, docs section 40)
  const local::AuxTarget* aux = nullptr;
};

// What a node binds its slot to for one frame (NodeDesc::bind).
struct FrameEnv {
  const point::CpuFrame* fk = nullptr;
  const FrameBlocks* blocks = nullptr;    // the resolved blocks of the families (copied into the slot)
  const ImageF* film = nullptr;           // the film stage's output plane (whole frame or the band in flight), null without a film
  const int* filmRow0 = nullptr;          // streaming export: frame row of film's first row (the film plane is a band)
  const local::AuxTarget* aux = nullptr;  // the side planes of the work window (null: none)
  std::span<const NodeType> chain;        // the head chain being bound (a node may depend on what follows it, e.g. a Blend)
};

constexpr std::size_t kMaxResolvedBytes = 128;
struct Resolved {
  alignas(16) unsigned char bytes[kMaxResolvedBytes];
};

struct ExecEnv {
  point::CpuFrame& fk;
  const Graph& graph;
  ResultCache& cache;
  CancelToken cancel;
  std::uint8_t* out = nullptr;    // w * h * outChannels, caller-allocated
  int outChannels = 4;
  // Float output (analysis, swatches, reference renders): the clamped, display-encoded frame instead of 8-bit pixels. The executor
  // sizes it; Whole plans only (a tiled plan returns Unsupported). `out` is then null.
  ImageF* outF = nullptr;
  // Streaming export (docs/engine/pipeline-architecture.md section 42), TiledExport plans: `out` holds one band of tile rows (plan.grid.tile rows),
  // `prepare` is called with the plan's window of the frame the head is about to compute (before every head run; the session hands the source
  // pixels of its source window to the point::CpuFrame) and `bandDone` with each finished band of the output sweep (rows [y0, y1) of the frame,
  // in order, the pixels in `out`). A false return aborts the run (Status::Cancelled).
  std::function<bool(const TileWin& win)> prepare;
  std::function<bool(int y0, int y1)> bandDone;
  // Streaming export of a film frame (section 42.6): filmBegin replaces the whole-frame film stage (the film's pre-passes; false = render
  // the frame whole), filmRows(y0, y1, out) renders rows of the film plane before the head reads them, filmAux fills the side planes of
  // a work window (the local adjustments of a film frame come from its input pass).
  std::function<bool()> filmBegin;
  std::function<bool(int y0, int y1, ImageF& out)> filmRows;
  std::function<bool(int x0, int y0, int x1, int y1, const local::AuxTarget& aux)> filmAux;
};

class CpuExecutor {
 public:
  // Runs everything but the HostTable stage (the session evaluated the tables before). Ok, Cancelled, Unsupported (e.g. the
  // film stage failed: the frame fails with an explicit engine error), OutOfMemory.
  static Status run(const Plan& plan, ExecEnv& env, ExecStats* stats = nullptr);
};

}  // namespace dapple::engine
