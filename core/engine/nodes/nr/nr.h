#pragma once
// Noise reduction node family: NRLuma (edge-stopped blend towards a box-blurred luminance) and NRChroma (low-resolution chroma blur),
// docs/engine/pipeline-architecture.md section 50. Every number the kernels use is derived here, once: the CPU stages (spatial.cpp
// stageNRLuma / stageNRChroma), the GPU passes (nodes/nr/gpu.cpp -> nr_luma.comp / nr_chroma.comp) and the planner's halo read the block.
#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::engine::nr {

struct LumaBlock {
  float amount = 0.f;     // 0..1, 0 = identity
  int radius = 0;         // box radius at this frame's scale (2 iterations)
  float blend = 0.f;      // strength of the blend towards the blurred luminance
  float inv2 = 0.f;       // 1 / sigma^2 of the edge stop
  bool active() const { return amount > 0.f; }
};
struct ChromaBlock {
  float amount = 0.f;     // 0..1, 0 = identity
  int factor = 2;         // the chroma planes are blurred on a grid reduced by this factor
  int lowRadius = 0;      // box radius of the chroma planes on that grid (2 iterations)
  int lumaRadius = 0;     // box radius of the luminance guide on that grid
  float blend = 0.f, inv2 = 0.f;
  bool active() const { return amount > 0.f; }
};

// The amounts after the frame's quality (Off: none; Interactive: only when the frame asks for noise reduction) and the source
// (display-referred sources were denoised in camera: nrColor 25 is their no-op level).
LumaBlock resolveLuma(const EditParams& p, const FrameFacts& f);
ChromaBlock resolveChroma(const EditParams& p, const FrameFacts& f);
// The derived numbers of an amount at a radii scale (also the planner's halo).
LumaBlock lumaFor(float amount, float scale);
ChromaBlock chromaFor(float amount, float scale);
// Neighbourhood the CPU stage reads around a window (box blur 2 iterations; chroma: + block + bilinear upsample).
Halo lumaHalo(const LumaBlock& b);
Halo chromaHalo(const ChromaBlock& b);

const NodeBehaviour& lumaBehaviour();
const NodeBehaviour& chromaBehaviour();
const CpuImpl& lumaCpu();
const CpuImpl& chromaCpu();
NodeDesc lumaDesc();    // registered in engine/nodes/node_list.inc
NodeDesc chromaDesc();

}  // namespace dapple::engine::nr
