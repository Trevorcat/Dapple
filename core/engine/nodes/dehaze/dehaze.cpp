#include "engine/nodes/dehaze/dehaze.h"

#include <algorithm>
#include <cmath>

#include "engine/exec/cpu_executor.h"
#include "spatial.h"

namespace dapple::engine::dehaze {

Block resolve(const EditParams& p, const FrameFacts& f) {
  return f.quality == 0 ? Block{} : dehazeFor(p.dehaze / 100.f, f.spatialScale);
}

Block dehazeFor(float amount, float scale) {
  Block b;
  b.amount = amount;
  if (b.amount > 0.f) {
    b.minRadius = std::max(1, int(std::lround(2.5f * scale)));
    b.boxRadius = std::max(1, int(std::lround(3.f * scale)));
  }
  b.omega = 0.7f * b.amount;
  b.veil = 0.5f * -b.amount;
  return b;
}

namespace {
Status tile(CpuTile& t, const void* r) {
  const auto& b = *static_cast<const Block*>(r);
  if (!b.active()) return Status::Ok;
  if (b.solve() && !t.dehaze) return Status::Internal;  // the solve must exist before the apply
  if (b.solve()) stageDehazeApply(*t.img, *t.scratch, t.ox, t.oy, t.frameW, t.frameH, *t.dehaze);
  else stageDehazeVeil(*t.img, *t.scratch, b.veil, b.veilAir);
  return Status::Ok;
}
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo noHalo(const void*) { return {}; }  // the frame-global part is a solve, not a halo; the apply is pointwise

const CpuImpl kCpu{nullptr, nullptr, tile, nullptr};
const NodeBehaviour kBehaviour{sizeof(Block), nullptr, identity, noHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

}  // namespace dapple::engine::dehaze
