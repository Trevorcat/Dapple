#include "engine/nodes/local/local.h"

#include "engine/exec/cpu_executor.h"
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

bool linearIdentity(const void* r) { return static_cast<const LinearCpu*>(r)->frame == nullptr; }
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
const NodeBehaviour kLinear{sizeof(LinearCpu), nullptr, linearIdentity, noHalo, nullptr};
const NodeBehaviour kDetail{sizeof(Block), nullptr, detailIdentity, detailHalo, nullptr};
static_assert(sizeof(Block) <= kMaxResolvedBytes && sizeof(LinearCpu) <= kMaxResolvedBytes, "resolved block too large");

}  // namespace

const NodeBehaviour& linearBehaviour() { return kLinear; }
const NodeBehaviour& detailBehaviour() { return kDetail; }
const CpuImpl& linearCpu() { return kLinearCpu; }
const CpuImpl& detailCpu() { return kDetailCpu; }

}  // namespace dapple::engine::localadj
