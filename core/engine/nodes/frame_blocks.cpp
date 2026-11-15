#include "engine/nodes/frame_blocks.h"

namespace dapple::engine {

FrameBlocks resolveFrameBlocks(const EditParams& p, const FrameFacts& f) {
  FrameBlocks b;
  b.vignette = vignette::resolve(p, f);
  b.nrLuma = nr::resolveLuma(p, f);
  b.nrChroma = nr::resolveChroma(p, f);
  b.dehaze = dehaze::resolve(p, f);
  b.detail = detail::resolve(p, f);
  b.grain = grain::resolve(p, f);
  return b;
}

}  // namespace dapple::engine
