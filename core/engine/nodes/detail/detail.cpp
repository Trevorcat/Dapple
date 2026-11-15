#include "engine/nodes/detail/detail.h"

#include <algorithm>
#include <cmath>

#include "dapple/modern.h"
#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"
#include "spatial.h"

namespace dapple::engine::detail {

Block detailFor(float clarity, float texture, float sharpen, float sharpRadius, float resizeSharpen, float scale) {
  Block b;
  b.clarity = clarity;
  b.texture = texture;
  b.sharpen = sharpen;
  b.sharpRadius = sharpRadius;
  if (clarity != 0.f) {
    b.guidedRadius = std::max(2, int(std::lround(0.02f * 2048.f * scale / 4.f)));
    b.clarityGain = clarity > 0.f ? clarity * 1.1f : clarity * 0.85f;
  }
  if (texture != 0.f) {
    b.textureRadius = std::max(1, int(std::lround(2.f * scale)));
    b.texturePos = texture > 0.f ? 1 : 0;
    b.textureGain = texture > 0.f ? texture * 1.5f : texture * 0.9f;
  }
  b.sharpAmount = sharpen > 0.f || resizeSharpen > 0.f ? sharpen + resizeSharpen : 0.f;
  if (b.sharpAmount > 0.f) {
    b.sharpSigma = std::max(0.4f, sharpRadius * scale * 0.5f);
    if (b.sharpSigma > 2.6f) b.sharpBox = std::max(1, int(std::lround(std::sqrt(b.sharpSigma * b.sharpSigma * 4.f / 3.f + 0.25f) - 0.5f)));
    else b.sharpGauss = std::max(1, int(std::ceil(b.sharpSigma * 3.f)));
  }
  return b;
}

Block resolve(const EditParams& p, const FrameFacts& f) {
  float clarity = 0.f, texture = 0.f, sharpen = 0.f, sharpRadius = 1.f;
  if (f.quality != 0) {
    sharpen = std::max(0.f, p.sharpen - (f.sceneReferred ? 0.f : 40.f)) / 100.f;
    sharpRadius = p.sharpRadius / 10.f;
    clarity = p.clarity / 100.f;
    texture = p.texture / 100.f;
    if (const ModernKind mk = modernKind(p.filmStock); mk != ModernKind::None) {  // local contrast of the modern rendering (section 37)
      const ModernRecipe& r = modernRecipe(mk);
      const float k = std::clamp(p.filmStrength / 100.f, 0.f, 1.f);
      clarity = std::clamp((p.clarity + r.clarity * k) / 100.f, -1.f, 1.f);
      texture = std::clamp((p.texture + r.texture * k) / 100.f, -1.f, 1.f);
    }
  }
  return detailFor(clarity, texture, sharpen, sharpRadius, f.resizeSharpen, f.spatialScale);
}

Halo halo(const Block& b) {
  int h = 0;
  // what the CPU passes read plus one pixel: the GPU kernels' reach (both executors run the plan's windows)
  if (b.texture != 0.f) h = std::max(h, 2 * b.textureRadius + 1);
  if (b.sharpAmount > 0.f) h = std::max(h, b.sharpBox > 0 ? 3 * b.sharpBox + 1 : b.sharpGauss + 1);
  return {h, h};
}

namespace {
Status tile(CpuTile& t, const void* r) {
  const auto& b = *static_cast<const Block*>(r);
  if (!b.active()) return Status::Ok;
  if (b.solve() && !t.solved) return Status::Internal;
  stageDetail(*t.img, *t.scratch, b, static_cast<const GuidedSolve*>(t.solved), t.ox, t.oy, t.frameW, t.frameH);
  return Status::Ok;
}
bool identity(const void* r) { return !static_cast<const Block*>(r)->active(); }
Halo haloFn(const void* r) { return halo(*static_cast<const Block*>(r)); }

// the guided-filter coefficients of the clarity base from the whole frame's 1/4 (Y, Y^2) grid
std::shared_ptr<void> solve(const std::vector<float>& grid, int gw, int gh, const void* r, std::size_t* bytes) {
  const auto& b = *static_cast<const Block*>(r);
  auto gs = std::make_shared<GuidedSolve>();
  guidedSolveGrid(grid, gw, gh, b.guidedRadius, kGuidedEps, *gs);
  *bytes = (gs->a.size() + gs->b.size()) * 4;
  return gs;
}
Accum accumOf(const void* r) { return static_cast<const Block*>(r)->solve() ? Accum::LumaGrid : Accum::None; }
const void* blockOf(const FrameView& v) { return v.blocks ? &v.blocks->detail : nullptr; }
bool bind(const FrameEnv& e, Resolved& out) {
  if (!e.blocks) return false;
  new (out.bytes) Block(e.blocks->detail);
  return true;
}
int planes(const void*) { return 7; }

const CpuImpl kCpu{nullptr, nullptr, tile, nullptr, nullptr, solve};
const NodeBehaviour kBehaviour{sizeof(Block), identity, haloFn, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& behaviour() { return kBehaviour; }
const CpuImpl& cpu() { return kCpu; }

NodeDesc desc() {
  ParamMask m = ParamGroups::get().spatial;
  m.add(keys({"clarity", "texture", "sharpen", "sharpRadius", "modern"}));
  NodeDesc d = nodeDesc("Detail", Domain::Image, Footprint::Window, 1, 1, m);  // guided base (frame-global solve) + detail passes
  d.inSpace[0] = Space::Display;
  d.wire = Wire::Display;
  d.order = 50;
  d.behaviour = &kBehaviour;
  d.block = blockOf;
  d.solve = accumOf;
  d.workPlanes = planes;
  d.cpu = &kCpu;
  d.bind = bind;
  return d;
}

}  // namespace dapple::engine::detail
