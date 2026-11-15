#pragma once
// FrameBlocks: the resolved parameter blocks of one frame, for the node families that resolve their own parameters
// (docs/engine/pipeline-architecture.md section 50). Pure function of EditParams + FrameFacts (+ what the frame bound from its source). The session resolves it for the graph
// builder (identity decides wiring), the plan request and the GPU run; the CPU executor resolves it again when it binds (after the film
// stage). Executors hand `&blocks.<node>` to the node's CpuImpl / GpuImpl and never read the family's EditParams fields themselves.
#include "dapple/edit_params.h"
#include "engine/nodes/frame_facts.h"
#include "engine/nodes/dehaze/dehaze.h"
#include "engine/nodes/detail/detail.h"
#include "engine/nodes/grain/grain.h"
#include "engine/nodes/local/local.h"
#include "engine/nodes/nr/nr.h"
#include "engine/nodes/vignette/vignette.h"

namespace dapple::engine {

// What a frame bound from its source that a family's resolve reads (field builds, plane caches). Owned by the binder: FrameKernels on the
// CPU (FrameKernels::frameBound), the session's GPU frame (localadj::bind + the FramePlan's reshaping field).
struct FrameBound {
  const local::Frame* local = nullptr;  // localadj::bind
  const warp::Field* warp = nullptr;    // portrait reshaping field
};

struct FrameBlocks {
  vignette::Block vignette;
  nr::LumaBlock nrLuma;
  nr::ChromaBlock nrChroma;
  dehaze::Block dehaze;
  detail::Block detail;
  grain::Block grain;
  localadj::Block local;
  // a neighbourhood stage runs (the CPU film streaming path has none)
  bool anyWindow() const { return nrLuma.active() || nrChroma.active() || dehaze.active() || detail.active() || grain.active(); }
};

FrameBlocks resolveFrameBlocks(const EditParams& p, const FrameFacts& f, const FrameBound& bound);

}  // namespace dapple::engine
