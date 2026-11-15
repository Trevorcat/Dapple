#include "engine/nodes/resize/resize.h"

#include <algorithm>
#include <cmath>

#include "dapple/decoder.h"
#include "engine/exec/cpu_executor.h"

namespace dapple::engine::resize {

Block resolve(int storedW, int storedH, int frameSrcW, int frameSrcH) { return Block{storedW, storedH, frameSrcW, frameSrcH}; }

Rect sourceWindow(const Block& b, const Rect& dst) {
  Rect w;
  auto axis = [](int srcLen, int dstLen, int d0, int d1, int& lo, int& hi) {
    const float s = float(srcLen) / float(dstLen), S = std::max(1.f, s);
    lo = int(std::floor((float(d0) + 0.5f) * s - 0.5f - 3.f * S)) - 1;
    hi = int(std::floor((float(d1) - 0.5f) * s - 0.5f + 3.f * S)) + 3;
    lo = std::max(0, lo);
    hi = std::min(srcLen, hi);
  };
  axis(b.srcW, b.dstW, dst.x0, dst.x1, w.x0, w.x1);
  axis(b.srcH, b.dstH, dst.y0, dst.y1, w.y0, w.y1);
  return w;
}

Status bake(const void* ctx, std::span<const void* const>, void** out) {
  const BakeCtx& c = *static_cast<const BakeCtx*>(ctx);
  auto* img = new SourceImage();
  img->sceneReferred = c.full->sceneReferred;
  img->linear = resizeToLongEdge(c.full->linear, c.longEdge);
  *out = img;
  return Status::Ok;
}
void freePayload(void* p) { delete static_cast<SourceImage*>(p); }

namespace {
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo noHalo(const void*) { return {}; }
Rect inputRect(const void* r, const Rect& out) { return sourceWindow(*static_cast<const Block*>(r), out); }
const NodeBehaviour kBehaviour{sizeof(Block), nullptr, identity, noHalo, inputRect};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }

}  // namespace dapple::engine::resize
