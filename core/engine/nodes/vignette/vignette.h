#pragma once
// Vignette node family (docs/engine/pipeline-architecture.md section 50): the resolved block, its resolve, the CPU block fragment and the
// per-pixel formula (vignette_kernel.h). GPU: the point recipe (nodes/point/gpu.cpp -> point.comp) and the film finish (film_fin.comp) read the same block.
#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::engine::vignette {

struct Block {
  float amount = 0.f;                                   // -1..1, 0 = identity (EditParams::vignette / 100)
  int p3 = 0;                                           // 1: pixels are Display P3 encoded (export); the formula runs on sRGB-encoded values
  float ox0 = 0.f, oy0 = 0.f, stepX = 1.f, stepY = 1.f;  // output pixel -> picture position (FrameFacts)
  float cx = 0.f, cy = 0.f;                             // picture centre
  bool active() const { return amount != 0.f; }
};

Block resolve(const EditParams& p, const FrameFacts& f);
const NodeBehaviour& behaviour();
const CpuImpl& cpu();
NodeDesc desc();  // registered in engine/nodes/node_list.inc

}  // namespace dapple::engine::vignette
