#pragma once
// CpuExecutor: runs a Plan on the CPU (docs/engine/pipeline-architecture.md 5.2).
//
// How an executor is selected per frame: EngineSession::renderFrame() builds the Full graph, the Planner turns it into a Plan,
// and the session hands the Plan to the executor that supports it (today the CpuExecutor; the GPU engineer's GpuExecutor is
// tried first and returns Status::Unsupported / a null result to fall through to this one, like gpuHook did). A Plan that
// contains a node without an impl for an executor is unsupported by it (Plan::unsupported / Status::Unsupported).
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "dapple/image.h"
#include "engine/exec/result_cache.h"
#include "engine/graph/graph.h"
#include "engine/graph/node.h"
#include "engine/plan/plan.h"
#include "frame_kernels.h"
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

// A window of the frame handed to a window node (CpuImpl::tile).
struct CpuTile {
  ImageF* img = nullptr;          // interior + halo, in place
  SpatialScratch* scratch = nullptr;
  int ox = 0, oy = 0;             // origin of img in the frame
  int frameW = 0, frameH = 0;
  const DehazeSolve* dehaze = nullptr;   // frame-global solves (null until solved / when not needed)
  const GuidedSolve* guided = nullptr;
  // Pack8 only: the interior rect (in img coordinates) goes to out8 at frame position (ox + interior.x0, ...)
  Rect interior;
  std::uint8_t* out8 = nullptr;
  int outChannels = 4;
  int outRow0 = 0;                // frame row of out8's first row (streaming export: out8 holds the band in flight)
  // LocalDetail: the per-pixel amounts LocalLinear wrote for this window (docs/engine/pipeline-architecture.md section 40)
  const local::AuxTarget* aux = nullptr;
};

// What a node binds its resolved block to for one frame (nodes/main/cpu_nodes.cpp).
struct FrameEnv {
  const FrameKernels* fk = nullptr;
  const ImageF* film = nullptr;   // FilmSim output plane (whole frame), null when there is no film branch
  bool blendFollows = false;      // DisplayStages feeds a Blend (else it replaces the pixel value)
  const local::AuxTarget* aux = nullptr;  // LocalLinear: where the stage-W amounts of the work window go (null: none)
  const int* filmRow0 = nullptr;          // streaming export: frame row of film's first row (the film plane is a band)
};

constexpr std::size_t kMaxResolvedBytes = 128;
struct Resolved {
  alignas(16) unsigned char bytes[kMaxResolvedBytes];
};

// nodes/main/cpu_nodes.cpp: the CPU implementation table of the main pipeline and the per-frame binding of resolved blocks.
const CpuImpl* cpuImpl(NodeType t);
bool bindResolved(NodeType t, const FrameEnv& env, Resolved& out);
// CPU fusion recipe for the head of a chain: [Resize|Geometry] + Gain + ToneColour as one block fragment (baked variant).
const CpuImpl* cpuFusedHead();

struct ExecEnv {
  FrameKernels& fk;
  const Graph& graph;
  ResultCache& cache;
  CancelToken cancel;
  std::uint8_t* out = nullptr;    // w * h * outChannels, caller-allocated
  int outChannels = 4;
  // Float output (analysis, swatches, reference renders): the clamped, display-encoded frame instead of 8-bit pixels. The executor
  // sizes it; Whole plans only (a tiled plan returns Unsupported). `out` is then null.
  ImageF* outF = nullptr;
  // Streaming export (docs/engine/pipeline-architecture.md section 42), TiledExport plans: `out` holds one band of tile rows (plan.grid.tile rows),
  // `prepare` is called with the rect of the frame the head is about to compute (before every head run; it hands the source pixels of that rect to
  // the FrameKernels) and `bandDone` with each finished band of the output sweep (rows [y0, y1) of the frame, in order, the pixels in `out`). A false
  // return aborts the run (Status::Cancelled). Film plans are not run this way (Unsupported).
  std::function<bool(int x0, int y0, int x1, int y1)> prepare;
  std::function<bool(int y0, int y1)> bandDone;
  // Streaming export of a film frame (section 42.6): filmBegin replaces the whole-frame film stage (the film's pre-passes; false = render
  // the frame whole), filmRows(y0, y1, out) renders rows of the film plane before the head reads them, filmAux fills the stage-W amounts of
  // a work window (the local adjustments of a film frame come from its input pass).
  std::function<bool()> filmBegin;
  std::function<bool(int y0, int y1, ImageF& out)> filmRows;
  std::function<bool(int x0, int y0, int x1, int y1, const local::AuxTarget& aux)> filmAux;
};

class CpuExecutor {
 public:
  // Runs everything but the HostTable stage (the session evaluated the tables before). Ok, Cancelled, Unsupported (e.g. the
  // film stage failed: the caller renders the frame with the legacy renderer), OutOfMemory.
  static Status run(const Plan& plan, ExecEnv& env, ExecStats* stats = nullptr);
};

}  // namespace dapple::engine
