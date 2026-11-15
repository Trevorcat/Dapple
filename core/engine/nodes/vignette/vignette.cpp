#include "engine/nodes/vignette/vignette_kernel.h"

#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"

namespace dapple::engine::vignette {

Block resolve(const EditParams& p, const FrameFacts& f) {
  Block b;
  b.amount = p.vignette / 100.f;
  b.p3 = p.outputSpace == 1 ? 1 : 0;
  b.ox0 = f.geo.ox0, b.oy0 = f.geo.oy0, b.stepX = f.geo.stepX, b.stepY = f.geo.stepY;
  b.cx = f.geo.fullW * 0.5f;
  b.cy = f.geo.fullH * 0.5f;
  return b;
}

namespace {

void block(const void* r, float* px, int n, const PixelPos& pos) {
  const Block& b = *static_cast<const Block*>(r);
  const bool p3 = b.p3 == 1;
  for (int i = 0; i < n; ++i) applyAt(b, px + 3 * i, pos.x + i, pos.y, p3);
}
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo noHalo(const void*) { return {}; }

const void* blockOf(const FrameView& v) { return v.blocks ? &v.blocks->vignette : nullptr; }
bool bind(const FrameEnv& e, Resolved& out) {
  if (!e.blocks) return false;
  new (out.bytes) Block(e.blocks->vignette);
  return true;
}
const CpuImpl kCpu{nullptr, block, nullptr, nullptr};
const NodeBehaviour kBehaviour{sizeof(Block), identity, noHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");

}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

NodeDesc desc() {
  ParamMask m = ParamGroups::get().frame;
  m.add(keys({"vignette", "ctx.outSpace"}));
  NodeDesc d = nodeDesc("Vignette", Domain::Image, Footprint::Point, 1, 1, m);
  d.inSpace[0] = Space::Display;
  d.fusion = Fusion::Head;
  d.wire = Wire::Display;
  d.order = 10;
  d.behaviour = &kBehaviour;
  d.block = blockOf;
  d.cpu = &kCpu;
  d.bind = bind;
  return d;
}

}  // namespace dapple::engine::vignette
