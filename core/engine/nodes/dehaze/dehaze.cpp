#include "engine/nodes/dehaze/dehaze.h"

#include <algorithm>
#include <cmath>

#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"
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
  if (b.solve() && !t.solved) return Status::Internal;  // the solve must exist before the apply
  if (b.solve()) stageDehazeApply(*t.img, *t.scratch, t.ox, t.oy, t.frameW, t.frameH, *static_cast<const DehazeSolve*>(t.solved));
  else stageDehazeVeil(*t.img, *t.scratch, b.veil, b.veilAir);
  return Status::Ok;
}
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo noHalo(const void*) { return {}; }  // the frame-global part is a solve, not a halo; the apply is pointwise

// the airlight / transmission of the whole frame from its 1/4 colour grid
std::shared_ptr<void> solve(const std::vector<float>& grid, int gw, int gh, const void* r, std::size_t* bytes) {
  const auto& b = *static_cast<const Block*>(r);
  auto sv = std::make_shared<DehazeSolve>();
  dehazeSolveGrid(grid, gw, gh, b.minRadius, b.boxRadius, b.omega, *sv);
  *bytes = sv->t.size() * 4;
  return sv;
}
Accum accumOf(const void* r) { return static_cast<const Block*>(r)->solve() ? Accum::ColourGrid : Accum::None; }
const void* blockOf(const FrameView& v) { return v.blocks ? &v.blocks->dehaze : nullptr; }
bool bind(const FrameEnv& e, Resolved& out) {
  if (!e.blocks) return false;
  new (out.bytes) Block(e.blocks->dehaze);
  return true;
}
int planes(const void*) { return 7; }

const CpuImpl kCpu{nullptr, nullptr, tile, nullptr, nullptr, solve};
const NodeBehaviour kBehaviour{sizeof(Block), identity, noHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

NodeDesc desc() {
  ParamMask m = ParamGroups::get().spatial;
  m.add(keys({"dehaze"}));
  NodeDesc d = nodeDesc("Dehaze", Domain::Image, Footprint::Window, 1, 1, m);  // dark channel on a 1/4 grid + global airlight
  d.inSpace[0] = Space::Display;
  d.wire = Wire::Display;
  d.order = 40;
  d.behaviour = &kBehaviour;
  d.block = blockOf;
  d.solve = accumOf;
  d.workPlanes = planes;
  d.cpu = &kCpu;
  d.bind = bind;
  return d;
}

}  // namespace dapple::engine::dehaze
