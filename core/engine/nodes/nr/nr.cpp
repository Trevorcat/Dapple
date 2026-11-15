#include "engine/nodes/nr/nr.h"

#include <algorithm>
#include <cmath>

#include "engine/exec/cpu_executor.h"
#include "spatial.h"

namespace dapple::engine::nr {

namespace {
bool runs(const FrameFacts& f) { return f.quality != 0 && !(f.quality == 1 && !f.noise); }
}  // namespace

LumaBlock lumaFor(float amount, float scale) {
  LumaBlock b;
  if (!(amount > 0.f)) return b;
  b.amount = amount;
  b.radius = std::max(1, int(std::lround((0.8f + 1.8f * amount) * scale)));
  b.blend = std::min(1.f, std::pow(amount, 0.75f) * 1.15f);
  const float sigma = 0.006f + 0.05f * amount;
  b.inv2 = 1.f / (sigma * sigma);
  return b;
}

ChromaBlock chromaFor(float amount, float scale) {
  ChromaBlock b;
  if (!(amount > 0.f)) return b;
  b.amount = amount;
  const float rFull = (1.5f + 5.f * amount) * scale;
  b.factor = rFull >= 6.f ? 4 : 2;  // chroma noise is low-frequency: work on a reduced copy
  b.lowRadius = std::max(1, int(std::lround(rFull / float(b.factor))));
  b.lumaRadius = std::max(1, b.lowRadius / 2);
  b.blend = std::min(1.f, amount * 1.4f);
  b.inv2 = 1.f / (0.05f * 0.05f);
  return b;
}

LumaBlock resolveLuma(const EditParams& p, const FrameFacts& f) {
  return runs(f) ? lumaFor(p.nrLuma / 100.f, f.spatialScale) : LumaBlock{};
}
ChromaBlock resolveChroma(const EditParams& p, const FrameFacts& f) {
  return runs(f) ? chromaFor(std::max(0.f, p.nrColor - (f.sceneReferred ? 0.f : 25.f)) / 100.f, f.spatialScale) : ChromaBlock{};
}

Halo lumaHalo(const LumaBlock& b) { return b.active() ? Halo{2 * b.radius, 2 * b.radius} : Halo{}; }
Halo chromaHalo(const ChromaBlock& b) {
  if (!b.active()) return {};
  const int h = (2 * b.lowRadius + 2) * b.factor;
  return {h, h};
}

namespace {
Status tileLuma(CpuTile& t, const void* r) {
  const auto& b = *static_cast<const LumaBlock*>(r);
  if (b.active()) stageNRLuma(*t.img, *t.scratch, b.radius, b.blend, b.inv2);
  return Status::Ok;
}
Status tileChroma(CpuTile& t, const void* r) {
  const auto& b = *static_cast<const ChromaBlock*>(r);
  if (b.active()) stageNRChroma(*t.img, *t.scratch, b.factor, b.lowRadius, b.lumaRadius, b.blend, b.inv2);
  return Status::Ok;
}
bool lumaIdentity(const void* r) { return !static_cast<const LumaBlock*>(r)->active(); }
bool chromaIdentity(const void* r) { return !static_cast<const ChromaBlock*>(r)->active(); }
Halo lumaHaloFn(const void* r) { return lumaHalo(*static_cast<const LumaBlock*>(r)); }
Halo chromaHaloFn(const void* r) { return chromaHalo(*static_cast<const ChromaBlock*>(r)); }

const CpuImpl kLumaCpu{nullptr, nullptr, tileLuma, nullptr};
const CpuImpl kChromaCpu{nullptr, nullptr, tileChroma, nullptr};
const NodeBehaviour kLuma{sizeof(LumaBlock), nullptr, lumaIdentity, lumaHaloFn, nullptr};
const NodeBehaviour kChroma{sizeof(ChromaBlock), nullptr, chromaIdentity, chromaHaloFn, nullptr};
static_assert(sizeof(LumaBlock) <= kMaxResolvedBytes && sizeof(ChromaBlock) <= kMaxResolvedBytes, "resolved block too large");
}  // namespace

const NodeBehaviour& lumaBehaviour() { return kLuma; }
const NodeBehaviour& chromaBehaviour() { return kChroma; }
const CpuImpl& lumaCpu() { return kLumaCpu; }
const CpuImpl& chromaCpu() { return kChromaCpu; }

}  // namespace dapple::engine::nr
