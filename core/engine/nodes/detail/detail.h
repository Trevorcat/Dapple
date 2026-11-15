#pragma once
// Detail node family (docs/engine/pipeline-architecture.md section 50): clarity (guided-filter base layer, frame-global solve on the 1/4
// grid), texture (box-blurred luminance, compressed edges) and sharpening (soft-limited unsharp mask), applied together on the luminance.
// CPU: spatial.cpp guidedSolveGrid / stageDetail; GPU: nodes/detail/gpu.cpp (gf_ab + detail.comp); planner: halo(). All read this block.
#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::engine::detail {

constexpr float kGuidedEps = 0.045f * 0.045f;  // regularisation of the guided filter (clarity base, local clarity)
constexpr float kSharpLimit = 0.15f;           // soft limit of the sharpening (no big halos)
constexpr float kSharpGate = 0.0025f;          // gate that leaves sensor-noise-sized detail alone

struct Block {
  float clarity = 0.f, texture = 0.f;  // -1..1
  float sharpen = 0.f;                 // the develop sharpening 0..1.5
  float sharpRadius = 1.f;             // its radius in pixels at 2048 px (0.5..3.0)
  float sharpAmount = 0.f;             // sharpen + the GPU export's output sharpening after a downscale (FrameFacts::resizeSharpen)
  int guidedRadius = 0;                // clarity: box radius of the guided filter on the 1/4 grid
  float clarityGain = 0.f;
  int textureRadius = 0;               // texture: box radius (2 iterations)
  float textureGain = 0.f;
  int texturePos = 0;                  // 1: positive texture (big edges compressed)
  float sharpSigma = 0.f;              // sharpening blur sigma; > 2.6: 3 box iterations of sharpBox, else a direct kernel of half width sharpGauss
  int sharpBox = 0, sharpGauss = 0;
  bool active() const { return clarity != 0.f || texture != 0.f || sharpAmount > 0.f; }
  bool solve() const { return clarity != 0.f; }  // the frame-global part (guided-filter coefficients) runs
};

// Off: no develop amounts (the export's output sharpening stays). Display-referred sources: sharpen 40 is the no-op level (sharpened
// in camera). A modern rendering (EditParams::filmStock = modern_*) adds its local contrast (section 37).
Block resolve(const EditParams& p, const FrameFacts& f);
Block detailFor(float clarity, float texture, float sharpen, float sharpRadius, float resizeSharpen, float scale);
// Neighbourhood the CPU stage reads around a window (texture box 2 iterations, sharpening blur); the clarity part is a solve.
Halo halo(const Block& b);

const NodeBehaviour& behaviour();
const CpuImpl& cpu();

}  // namespace dapple::engine::detail
