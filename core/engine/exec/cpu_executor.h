#pragma once
// CpuExecutor: runs a Plan on the CPU (docs/engine/pipeline-architecture.md 5.2).
//
// How an executor is selected per frame: EngineSession::renderFrame() builds the Full graph, the Planner turns it into a Plan,
// and the session hands the Plan to the executor that supports it (today the CpuExecutor; the GPU engineer's GpuExecutor is
// tried first and returns Status::Unsupported / a null result to fall through to this one, like gpuHook did). A Plan that
// contains a node without an impl for an executor is unsupported by it (Plan::unsupported / Status::Unsupported).
#include <atomic>
#include <cstdint>
#include <vector>

#include "dapple/image.h"
#include "engine/exec/result_cache.h"
#include "engine/graph/graph.h"
#include "engine/graph/node.h"
#include "engine/plan/plan.h"
#include "frame_kernels.h"
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
};

// What a node binds its resolved block to for one frame (nodes/main/cpu_nodes.cpp).
struct FrameEnv {
  const FrameKernels* fk = nullptr;
  const ImageF* film = nullptr;   // FilmSim output plane (whole frame), null when there is no film branch
  bool blendFollows = false;      // DisplayStages feeds a Blend (else it replaces the pixel value)
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
};

class CpuExecutor {
 public:
  // Runs everything but the HostTable stage (the session evaluated the tables before). Ok, Cancelled, Unsupported (e.g. the
  // film stage failed: the caller renders the frame with the legacy renderer), OutOfMemory.
  static Status run(const Plan& plan, ExecEnv& env, ExecStats* stats = nullptr);
};

}  // namespace dapple::engine
