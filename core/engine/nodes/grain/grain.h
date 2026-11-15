#pragma once
// Grain node family (docs/engine/pipeline-architecture.md section 50): luminance-weighted value noise on absolute frame coordinates.
// CPU: spatial.cpp stageGrain; GPU: the epilogue slot of the last pass (Epilogue::amp / period, every kernel that can end the chain).
#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::engine::grain {

struct Block {
  float amount = 0.f;   // 0..1 (user grain + the look's grain), 0 = identity
  float period = 1.f;   // noise cell size in frame pixels (scale-aware)
  float inv = 1.f;      // 1 / period
  float amp = 0.f;      // amplitude of the noise
  bool active() const { return amount > 0.f; }
};

// Off: none; Interactive: only while a grain-family slider is dragged (FrameFacts::keepGrain, 14 Q8).
Block resolve(const EditParams& p, const FrameFacts& f);
Block grainFor(float amount, float scale);

const NodeBehaviour& behaviour();
const CpuImpl& cpu();

}  // namespace dapple::engine::grain
