#pragma once
// Resize node family (docs/engine/pipeline-architecture.md section 50): the stored source resampled (Lanczos-3) to the frame's source size.
// CPU: a bake of the whole source at a long edge (CpuImpl::bake, cached by the session: EngineSession::resizedSource), whose rows the chain's
// head then reads; GPU (resize/gpu.cpp, resize.comp): the resident source resampled once and cached, or each streamed window resampled.
#include <span>

#include "engine/graph/node.h"

namespace dapple { struct SourceImage; }  // dapple/decoder.h

namespace dapple::engine::resize {

struct Block {
  int srcW = 0, srcH = 0;  // the stored source
  int dstW = 0, dstH = 0;  // the frame's source (FrameFacts::srcW / srcH)
  bool active() const { return dstW != srcW || dstH != srcH; }
};
Block resolve(int storedW, int storedH, int frameSrcW, int frameSrcH);

// The source pixels (clamped, exclusive end) the Lanczos-3 filter reads to produce rect `dst` of the resized picture (NodeBehaviour::inputRect).
Rect sourceWindow(const Block& b, const Rect& dst);

// The CPU bake's input: the whole source, resampled to `longEdge` (payload: SourceImage).
struct BakeCtx {
  const SourceImage* full = nullptr;
  int longEdge = 0;
};
Status bake(const void* ctx, std::span<const void* const> inputs, void** payloadOut);
void freePayload(void* p);

const NodeBehaviour& behaviour();
NodeDesc desc();  // registered in engine/nodes/node_list.inc

}  // namespace dapple::engine::resize
