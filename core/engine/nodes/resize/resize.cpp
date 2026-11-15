#include "engine/nodes/resize/resize.h"

#include <algorithm>
#include <cmath>

#include "dapple/decoder.h"
#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"

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
const NodeBehaviour kBehaviour{sizeof(Block), identity, noHalo, inputRect};
bool bindFrame(const FrameEnv& env, Resolved& out) {
  if (!env.fk) return false;
  new (out.bytes) point::FrameSlot{env.fk};
  return true;
}
// leading a head chain: the frame's source row as it is (the frame binds the resized source); fuses like Geometry
const CpuImpl kCpu{nullptr, point::copyBlock, nullptr, bake, freePayload, nullptr, point::fuseHead};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }

// The resized source is a picture of its own (the frame's source grid): source windows of the plan are in the grid after this node.
NodeDesc desc() {
  NodeDesc d = nodeDesc("Resize", Domain::Image, Footprint::Resample, 1, 1, keys({"ctx.outLongEdge", "ctx.region", "ctx.srcSize"}));
  d.inSpace[0] = Space::Source;
  d.outSpace = Space::Source;
  d.rescales = true;
  d.fusion = Fusion::Head;
  d.behaviour = &kBehaviour;
  d.cpu = &kCpu;
  d.bind = bindFrame;
  return d;
}

}  // namespace dapple::engine::resize
