#include "engine/nodes/grain/grain.h"

#include <algorithm>

#include "engine/exec/cpu_executor.h"
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

const CpuImpl kCpu{nullptr, nullptr, tile, nullptr};
const NodeBehaviour kBehaviour{sizeof(Block), nullptr, identity, noHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

}  // namespace dapple::engine::grain
