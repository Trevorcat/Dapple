#include "engine/nodes/grain/grain.h"

#include <algorithm>

#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"
#include "spatial.h"

namespace dapple::engine::grain {

Block grainFor(float amount, float scale) {
  Block b;
  b.amount = amount > 0.f ? amount : 0.f;
  b.period = std::max(1.f, 1.6f * scale);
  b.inv = 1.f / b.period;
  b.amp = amount * 0.13f * (b.period > 1.01f ? 1.5f : 1.f);
  return b;
}

Block resolve(const EditParams& p, const FrameFacts& f) {
  float amount = 0.f;
  if (f.quality != 0) {
    amount = std::min(100.f, p.grain + f.lookGrain) / 100.f;
    if (f.quality == 1 && !f.keepGrain) amount = 0.f;
  }
  return grainFor(amount, f.spatialScale);
}

namespace {
Status tile(CpuTile& t, const void* r) {
  const auto& b = *static_cast<const Block*>(r);
  if (b.active()) stageGrain(*t.img, b.period, b.inv, b.amp, t.ox, t.oy);
  return Status::Ok;
}
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo noHalo(const void*) { return {}; }

const void* blockOf(const FrameView& v) { return v.blocks ? &v.blocks->grain : nullptr; }
bool bind(const FrameEnv& e, Resolved& out) {
  if (!e.blocks) return false;
  new (out.bytes) Block(e.blocks->grain);
  return true;
}
// 4.6 / section 14 (Q8): grain is dropped at the interactive tier unless a grain-family slider is being dragged; then the node stays wired
bool wanted(const BuildCtx& c) {
  const FrameRequest& f = c.ps.frame();
  const bool keep = f.interactive && (f.activeParam == "grain" || f.activeParam == "film_grain") && c.ps.edit().grain > 0.f;
  return keep || (c.view.blocks && c.view.blocks->grain.active());
}
const CpuImpl kCpu{nullptr, nullptr, tile, nullptr};
const NodeBehaviour kBehaviour{sizeof(Block), identity, noHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

// a per-tile node with no neighbourhood: hash noise on frame coordinates
NodeDesc desc() {
  ParamMask m = ParamGroups::get().spatial;
  m.add(keys({"grain", "ctx.activeParam", "ctx.filmOnly"}));
  NodeDesc d = nodeDesc("Grain", Domain::Image, Footprint::Point, 1, 1, m);
  d.inSpace[0] = Space::Display;
  d.wire = Wire::Display;
  d.order = 70;
  d.wanted = wanted;
  d.behaviour = &kBehaviour;
  d.block = blockOf;
  d.cpu = &kCpu;
  d.bind = bind;
  return d;
}

}  // namespace dapple::engine::grain
