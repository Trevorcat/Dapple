// The point family's CPU binding of one frame (CpuFrame, nodes/point/cpu.h): the block fragments of the head (source copy / geometry
// resampling, gain, HDR, tone + colour through the lattice or the exact operator, the fused head), the display stages and the blend, and
// the local adjustments' view of the geometry. The film stage's part of the binding is in nodes/film/film_glue.cpp.
#include "engine/nodes/point/cpu_impl.h"

#include "engine/exec/cpu_executor.h"
#include "engine/nodes/point/scratch.h"

namespace dapple::engine::point {

namespace {
std::uint64_t nextFrameIdImpl() {
  static std::atomic<std::uint64_t> n{0};
  return ++n;
}
// The last row fetchRow resampled on this thread: the local adjustments' pre-gain source of the same block (sourceRow) reads it back instead
// of resampling the source a second time (LocalLinear runs right after Geometry + Gain on the same block).
struct RowMemo {
  std::uint64_t id = 0;
  int x0 = 0, y = 0, n = 0;
  float px[256 * 3];
};
thread_local RowMemo t_rowMemo;
}  // namespace

std::uint64_t nextFrameId() { return nextFrameIdImpl(); }

CpuFrame::CpuFrame(const Params& r, const SourceImage& src, const BakedLut* lut, const FastOptions& opt)
    : impl_(std::make_unique<Impl>(r, src, lut, opt)) {}
CpuFrame::~CpuFrame() = default;

bool CpuFrame::valid() const { return impl_->valid; }
int CpuFrame::width() const { return impl_->b.geo.outW; }
int CpuFrame::height() const { return impl_->b.geo.outH; }
bool CpuFrame::exactVariant() const { return impl_->exact; }
bool CpuFrame::useGeo() const { return impl_->b.geo.useGeo; }
bool CpuFrame::filmSelected() const { return impl_->fr.stock != nullptr; }
float CpuFrame::filmStrength() const { return impl_->fr.strength; }
float CpuFrame::spatialScale() const { return impl_->b.spatialScale; }
bool CpuFrame::filmOk() const { return impl_->filmOkV; }
bool CpuFrame::needNormal() const { return !impl_->filmOkV || impl_->fr.strength < 1.f; }

const EditParams& CpuFrame::params() const { return impl_->r.p; }

engine::FrameFacts CpuFrame::frameFacts() const { return engine::point::facts(impl_->b, impl_->opt, impl_->source.sceneReferred); }

const engine::point::Blocks& CpuFrame::blocks() const { return impl_->b; }

void CpuFrame::srcUV(int x, int y, float& u, float& v) const { impl_->srcUV(x, y, u, v); }
void CpuFrame::sourceRow(float* px, int n, int x0, int y) const {
  if (impl_->b.geo.useGeo) {
    const RowMemo& m = t_rowMemo;
    if (m.id == impl_->id && m.x0 == x0 && m.y == y && m.n == n) {
      std::memcpy(px, m.px, std::size_t(n) * 3 * sizeof(float));
      return;
    }
    fetchRow(px, n, x0, y);
  } else {
    copyRow(px, n, x0, y);
  }
}
void CpuFrame::linearRow(float* px, int n, int x0, int y) const {
  gain(px, n);
  hdrBlock(px, n, x0, y);
}
engine::FrameBound CpuFrame::frameBound() const { return {impl_->local.get(), impl_->wf}; }
engine::FrameBlocks CpuFrame::frameBlocks() const { return engine::resolveFrameBlocks(params(), frameFacts(), frameBound()); }

void CpuFrame::copyRow(float* px, int n, int x0, int y) const {
  const Impl& m = *impl_;
  const float* src = m.win ? m.win->row(y - m.winY) + 3 * std::ptrdiff_t(x0 - m.winX) : m.in.row(y) + 3 * std::size_t(x0);
  std::memcpy(px, src, std::size_t(n) * 3 * sizeof(float));
}

void CpuFrame::setSourceWindow(const ImageF* win, int ox, int oy) {
  impl_->win = win;
  impl_->winX = ox;
  impl_->winY = oy;
}

void CpuFrame::sourceRect(int x0, int y0, int x1, int y1, int& sx0, int& sy0, int& sx1, int& sy1) const {
  const Rect r = sourceWindow(impl_->b, Rect{x0, y0, x1, y1});
  sx0 = r.x0, sy0 = r.y0, sx1 = r.x1, sy1 = r.y1;
}

void CpuFrame::fetchRow(float* px, int n, int x0, int y) const {
  for (int i = 0; i < n; ++i) impl_->fetchPixel(x0 + i, y, px + 3 * i);
  if (n <= 256 && impl_->local) {  // the local adjustments read the same pixels again (sourceRow)
    RowMemo& m = t_rowMemo;
    m.id = impl_->id, m.x0 = x0, m.y = y, m.n = n;
    std::memcpy(m.px, px, std::size_t(n) * 3 * sizeof(float));
  }
}

void CpuFrame::gain(float* px, int n) const {
  const Impl& m = *impl_;
  if (m.exact) {  // renderFloat: (v * wb) * exposure
    const PointOp& op = *m.exactOp;
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c) px[3 * i + c] = std::max(0.f, px[3 * i + c] * op.wb[c] * op.exposureGain);
    return;
  }
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) px[3 * i + c] = std::max(0.f, px[3 * i + c] * m.b.gain[c]);
}

void CpuFrame::toneColour(float* px, int n) const {
  const Impl& m = *impl_;
  if (m.exact) {
    for (int i = 0; i < n; ++i) {
      const float v[3] = {px[3 * i], px[3 * i + 1], px[3 * i + 2]};
      m.exactOp->apply(v, px + 3 * i);
    }
    return;
  }
  const LutSampler sample = *m.sample;
  for (int i = 0; i < n; ++i) {
    float* p = px + 3 * i;
    const float v[3] = {p[0], p[1], p[2]};
    float e3[3], tm[3];
    if (!sample(v, e3, tm)) {
      std::call_once(m.opOnce, [&] { m.overflowOp.emplace(makePointOp(m.r, m.source, m.lutp)); });
      m.overflowOp->colour(tm, e3);
    }
    p[0] = e3[0], p[1] = e3[1], p[2] = e3[2];
  }
}

bool CpuFrame::canFuseHead() const { return !impl_->exact; }
bool CpuFrame::hdrActive() const { return impl_->b.hdr.active(); }
void CpuFrame::hdrBlock(float* px, int n, int x0, int y) const {
  if (!impl_->b.hdr.active()) return;
  for (int i = 0; i < n; ++i) impl_->hdrPixel(x0 + i, y, px + 3 * i);
}

void CpuFrame::headGainTone(float* px, int n, int x0, int y) const {
  const Impl& m = *impl_;
  // local copies: the compiler cannot keep fields of an object it may alias with the float stores below in registers
  const LutSampler sample = *m.sample;
  const float gain[3] = {m.b.gain[0], m.b.gain[1], m.b.gain[2]};
  const float* srcRow = m.b.geo.useGeo ? nullptr : (m.win ? m.win->row(y - m.winY) + 3 * std::ptrdiff_t(x0 - m.winX) : m.in.row(y) + 3 * std::size_t(x0));
  const bool hdrOn = m.b.hdr.active();
  for (int i = 0; i < n; ++i) {
    float v[3];
    if (srcRow) {
      v[0] = srcRow[3 * i], v[1] = srcRow[3 * i + 1], v[2] = srcRow[3 * i + 2];
    } else {
      m.fetchPixel(x0 + i, y, v);
    }
    for (int c = 0; c < 3; ++c) v[c] = std::max(0.f, v[c] * gain[c]);
    if (hdrOn) m.hdrPixel(x0 + i, y, v);
    float e3[3], tm[3];
    if (!sample(v, e3, tm)) {
      std::call_once(m.opOnce, [&] { m.overflowOp.emplace(makePointOp(m.r, m.source, m.lutp)); });
      m.overflowOp->colour(tm, e3);
    }
    float* p = px + 3 * i;
    p[0] = e3[0], p[1] = e3[1], p[2] = e3[2];
  }
}

void CpuFrame::displayStages(float* px, int n) const {
  for (int i = 0; i < n; ++i) point::displayStages(impl_->r, px + 3 * i);
}

void CpuFrame::blend(float* main, const float* film, int n) const {
  const float s = impl_->fr.strength;
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) main[3 * i + c] = main[3 * i + c] + s * (film[3 * i + c] - main[3 * i + c]);
}

void CpuFrame::finalizeSpatial() {
  Impl& m = *impl_;
  if (!m.exact || m.opt.spatial == SpatialQuality::Off) return;
  // renderFloat: a film at full strength brings its own grain, the look's grain would double it
  EditParams pe = m.r.p;
  if (m.filmOkV && m.fr.strength >= 1.f) pe.look = "std";
  m.b.lookGrain = (m.filmOkV && m.fr.strength >= 1.f ? makeStyle(pe) : m.exactOp->style).grain;
  m.b.spatialScale = float(std::max(m.b.geo.outW, m.b.geo.outH)) / 2048.f;  // the exact variant takes its own long edge once the film ran
}

struct ScratchFrame::Impl {
  ScratchImage s;
};
ScratchFrame::ScratchFrame() : impl_(std::make_unique<Impl>()) {}
ScratchFrame::~ScratchFrame() = default;
ImageF& ScratchFrame::get(int w, int h) { return impl_->s.get(w, h); }

// ================================================================================================
// The family's CPU nodes: block fragments over the frame's binding (resolved slot: FrameSlot; DisplayStages: DisplayCpu)
// ================================================================================================
const CpuImpl* fuseHead(const FrameEnv& env, std::span<const NodeType> c, std::size_t* consumed);

namespace {

// ---- point block fragments ---------------------------------------------------------------------------------------------
void copyBlockImpl(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const FrameSlot*>(r)->frame->copyRow(px, n, pos.x, pos.y);
}
void blkGeometry(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const FrameSlot*>(r)->frame->fetchRow(px, n, pos.x, pos.y);
}
void blkFusedHead(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const FrameSlot*>(r)->frame->headGainTone(px, n, pos.x, pos.y);
}
void blkGain(const void* r, float* px, int n, const PixelPos&) { static_cast<const FrameSlot*>(r)->frame->gain(px, n); }
void blkHdr(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const FrameSlot*>(r)->frame->hdrBlock(px, n, pos.x, pos.y);
}
void blkToneColour(const void* r, float* px, int n, const PixelPos&) { static_cast<const FrameSlot*>(r)->frame->toneColour(px, n); }

thread_local float t_side[256 * 3];  // display-staged film pixels waiting for the Blend that follows in the chain

void blkDisplay(const void* r, float* px, int n, const PixelPos& pos) {
  const auto* d = static_cast<const DisplayCpu*>(r);
  float* dst = d->replace ? px : t_side;
  std::memcpy(dst, d->film->row(pos.y - (d->row0 ? *d->row0 : 0)) + 3 * std::size_t(pos.x), std::size_t(n) * 3 * sizeof(float));
  d->frame->displayStages(dst, n);
}
void blkBlend(const void* r, float* px, int n, const PixelPos&) { static_cast<const FrameSlot*>(r)->frame->blend(px, t_side, n); }

const CpuImpl kGeometry{nullptr, blkGeometry, nullptr, nullptr, nullptr, nullptr, fuseHead};
const CpuImpl kFusedHead{nullptr, blkFusedHead, nullptr, nullptr};
const CpuImpl kGain{nullptr, blkGain, nullptr, nullptr};
const CpuImpl kHdr{nullptr, blkHdr, nullptr, nullptr};
const CpuImpl kToneColour{nullptr, blkToneColour, nullptr, nullptr};
const CpuImpl kDisplay{nullptr, blkDisplay, nullptr, nullptr};
const CpuImpl kBlend{nullptr, blkBlend, nullptr, nullptr};
static_assert(sizeof(DisplayCpu) <= kMaxResolvedBytes, "resolved block too large");
static_assert(sizeof(FrameSlot) <= kMaxResolvedBytes, "resolved block too large");

}  // namespace

void copyBlock(const void* r, float* px, int n, const PixelPos& pos) { copyBlockImpl(r, px, n, pos); }
// The CPU fusion recipe of a head chain led by a resampling node: [Resize | Geometry] + Gain + [Hdr] + ToneColour as one block fragment
// (headGainTone: the same operations in the same order), baked variant only.
const CpuImpl* fuseHead(const FrameEnv& env, std::span<const NodeType> c, std::size_t* consumed) {
  *consumed = 0;
  if (!env.fk || !env.fk->canFuseHead() || c.size() < 3 || c[1] != NodeType::Gain) return nullptr;
  const std::size_t nf = c.size() >= 4 && c[2] == NodeType::Hdr ? 4 : 3;
  if (c.size() < nf || c[nf - 1] != NodeType::ToneColour) return nullptr;
  *consumed = nf;
  return &kFusedHead;
}
const CpuImpl& geometryCpu() { return kGeometry; }
const CpuImpl& gainCpu() { return kGain; }
const CpuImpl& hdrCpu() { return kHdr; }
const CpuImpl& toneColourCpu() { return kToneColour; }
const CpuImpl& displayCpu() { return kDisplay; }
const CpuImpl& blendCpu() { return kBlend; }

}  // namespace dapple::engine::point
