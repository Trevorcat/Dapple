#include "engine/nodes/local/local.h"

#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"
#include <cstring>

#include "local_eval.h"
#include "local_retouch.h"

namespace dapple::engine::localadj {

std::shared_ptr<const local::Frame> bind(const EditParams& p, const SourceImage& src, bool interactive) {
  if (p.local.empty()) return nullptr;
  return local::bind(p.local, src, interactive);
}

Block resolve(const local::Frame* f, const warp::Field* wf, const FrameFacts& facts) {
  Block b;
  if (!f) return b;
  b.frame = f;
  b.warp = wf;
  b.scale = facts.spatialScale;
  b.detail = f->anyWindow;
  if (b.detail) {
    b.halo = local::detailRadii(*f, b.scale).halo;
    local::AuxTarget t;
    b.auxPlanes = local::auxLayout(*f, t);
  }
  b.retouch = f->retouchW || !f->spots.empty();
  if (b.retouch) std::memcpy(b.photo, local::frameAffine(facts, *f).m, sizeof b.photo);
  return b;
}

namespace {

void linearBlock(const void* r, float* px, int n, const PixelPos& pos) {
  const LinearCpu& l = *static_cast<const LinearCpu*>(r);
  if (l.frame) local::linearBlock(*l.frame, *l.src, px, n, pos.x, pos.y, l.aux);
}

Status detailTile(CpuTile& t, const void* r) {
  const Block& b = *static_cast<const Block*>(r);
  if (!b.frame || !b.detail) return Status::Ok;
  if (!t.aux || !t.aux->data || t.aux->ox != t.ox || t.aux->oy != t.oy || t.aux->w != t.img->width || t.aux->h != t.img->height)
    return Status::Internal;  // the amounts must come from the head chain of this very window
  local::Affine A;
  std::memcpy(A.m, b.photo, sizeof A.m);
  local::detailWindow(*b.frame, *t.img, *t.aux, b.scale, b.retouch ? &A : nullptr, b.warp);
  return Status::Ok;
}

bool linearIdentity(const void* r) { return static_cast<const Block*>(r)->frame == nullptr; }
bool detailIdentity(const void* r) {
  const Block& b = *static_cast<const Block*>(r);
  return !b.frame || !b.detail;
}
Halo noHalo(const void*) { return {}; }
Halo detailHalo(const void* r) {
  const Block& b = *static_cast<const Block*>(r);
  return {b.halo, b.halo};
}

const CpuImpl kLinearCpu{nullptr, linearBlock, nullptr, nullptr};
const CpuImpl kDetailCpu{nullptr, nullptr, detailTile, nullptr};
const NodeBehaviour kLinear{sizeof(Block), linearIdentity, noHalo, nullptr};
const void* blockOf(const FrameView& v) { return v.blocks ? &v.blocks->local : nullptr; }
bool bindLinear(const FrameEnv& e, Resolved& out) {
  if (!e.blocks || !e.fk) return false;
  new (out.bytes) LinearCpu{e.blocks->local.frame, e.fk, e.aux};
  return true;
}
bool bindDetail(const FrameEnv& e, Resolved& out) {
  if (!e.blocks) return false;
  new (out.bytes) Block(e.blocks->local);  // resolved after the film outcome is known (radii scale)
  return true;
}
int auxPlanes(const void* r) { return static_cast<const Block*>(r)->auxPlanes; }
int detailPlanes(const void* r) { return static_cast<const Block*>(r)->auxPlanes + 4 + 7; }  // the amounts, the band planes, the scratch
const NodeBehaviour kDetail{sizeof(Block), detailIdentity, detailHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes && sizeof(LinearCpu) <= kMaxResolvedBytes, "resolved block too large");

}  // namespace

const NodeBehaviour& linearBehaviour() { return kLinear; }
const NodeBehaviour& detailBehaviour() { return kDetail; }
const CpuImpl& linearCpu() { return kLinearCpu; }
const CpuImpl& detailCpu() { return kDetailCpu; }

NodeDesc linearDesc() {
  // the list is the node's own key; the masks read the source (ranges, raster guide) and the frame geometry
  ParamMask m = ParamGroups::get().frame;
  m.add(keys({"local", "ctx.source", "ctx.quality"}));
  NodeDesc d = nodeDesc("LocalLinear", Domain::Image, Footprint::Point, 1, 1, m);
  d.inSpace[0] = Space::Scene;
  d.fusion = Fusion::Head;
  d.wire = Wire::Scene;
  d.order = 20;
  d.behaviour = &kLinear;
  d.block = blockOf;
  d.workPlanes = auxPlanes;  // writes the stage-W amounts of the work window
  d.cpu = &kLinearCpu;
  d.bind = bindLinear;
  return d;
}

NodeDesc detailDesc() {
  ParamMask m = ParamGroups::get().spatial;
  m.add(keys({"local"}));
  NodeDesc d = nodeDesc("LocalDetail", Domain::Image, Footprint::Window, 1, 1, m);  // fine boxes + guided base on the window's 1/4 grid
  d.inSpace[0] = Space::Display;
  d.wire = Wire::Display;
  d.order = 60;
  d.behaviour = &kDetail;
  d.block = blockOf;
  d.workPlanes = detailPlanes;
  d.readsAux = true;
  d.cpu = &kDetailCpu;
  d.bind = bindDetail;
  return d;
}

}  // namespace dapple::engine::localadj
