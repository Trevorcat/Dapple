#pragma once
// Dehaze node family (docs/engine/pipeline-architecture.md section 50). amount > 0: a frame-global solve (dark channel + airlight on the
// whole frame's 1/4 grid) and a pointwise apply; amount < 0: a pointwise veil of light grey. CPU: spatial.cpp dehazeSolveGrid /
// stageDehazeApply / stageDehazeVeil; GPU: nodes/dehaze/gpu.cpp (sepconv min / box, reduce, dehaze_apply.comp). Both read this block.
#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::engine::dehaze {

struct Block {
  float amount = 0.f;     // -1..1, 0 = identity
  int minRadius = 0;      // dark channel min filter radius on the 1/4 grid (solve)
  int boxRadius = 0;      // smoothing box radius on the 1/4 grid (2 iterations, solve)
  float omega = 0.f;      // transmission t = 1 - omega * darkChannel / Amin
  float veil = 0.f;       // amount < 0: weight of the grey veil
  float veilAir = 0.88f;  // its grey level
  bool active() const { return amount != 0.f; }
  bool solve() const { return amount > 0.f; }  // the frame-global part runs (the veil is pointwise)
};

Block resolve(const EditParams& p, const FrameFacts& f);
Block dehazeFor(float amount, float scale);

const NodeBehaviour& behaviour();
const CpuImpl& cpu();

}  // namespace dapple::engine::dehaze
