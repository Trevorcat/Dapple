// CpuImpl of the main-pipeline nodes (docs/engine/pipeline-architecture.md 4.5). Every function wraps an existing kernel:
// the point nodes are block fragments of FrameKernels (the per-pixel loop of renderer.cpp), the window nodes call the stage
// functions of spatial.cpp. Nothing is re-implemented here.
//
// Block fragment contract (CpuImpl::block): `px` = n pixels of interleaved float RGB (3 floats, not float4), n <= 256,
// processed in place; `pos` = frame coordinates of the first pixel. The head of a chain (Resize/Geometry/DisplayStages in
// replace mode) ignores the incoming values and produces them.
#include <cstring>
#include <vector>

#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "local_eval.h"
#include "parallel.h"

namespace dapple::engine {

namespace {

struct PtRes {
  const FrameKernels* fk;
};
struct DispRes {
  const FrameKernels* fk;
  const ImageF* film;
  bool replace;
  const int* row0;  // streaming export: frame row of the film band's first row (null: the whole frame)
};
struct WinRes {
  const FrameKernels* fk;
  SpatialParams sp;
  float scale;
};
struct LocalRes {
  const FrameKernels* fk;
  const local::AuxTarget* aux;
};
static_assert(sizeof(WinRes) <= kMaxResolvedBytes && sizeof(DispRes) <= kMaxResolvedBytes, "resolved block too large");

// ---- point block fragments ---------------------------------------------------------------------------------------------
void blkCopy(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const PtRes*>(r)->fk->copyRow(px, n, pos.x, pos.y);
}
void blkGeometry(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const PtRes*>(r)->fk->fetchRow(px, n, pos.x, pos.y);
}
void blkFusedHead(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const PtRes*>(r)->fk->headGainTone(px, n, pos.x, pos.y);
}
void blkGain(const void* r, float* px, int n, const PixelPos&) { static_cast<const PtRes*>(r)->fk->gain(px, n); }
void blkHdr(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const PtRes*>(r)->fk->hdrBlock(px, n, pos.x, pos.y);
}
void blkToneColour(const void* r, float* px, int n, const PixelPos&) { static_cast<const PtRes*>(r)->fk->toneColour(px, n); }
void blkLocal(const void* r, float* px, int n, const PixelPos& pos) {
  const auto* l = static_cast<const LocalRes*>(r);
  if (const local::Frame* f = l->fk->localFrame()) local::linearBlock(*f, *l->fk, px, n, pos.x, pos.y, l->aux);
}
void blkVignette(const void* r, float* px, int n, const PixelPos& pos) {
  static_cast<const PtRes*>(r)->fk->vignette(px, n, pos.x, pos.y);
}

thread_local float t_side[256 * 3];  // display-staged film pixels waiting for the Blend that follows in the chain

void blkDisplay(const void* r, float* px, int n, const PixelPos& pos) {
  const auto* d = static_cast<const DispRes*>(r);
  float* dst = d->replace ? px : t_side;
  std::memcpy(dst, d->film->row(pos.y - (d->row0 ? *d->row0 : 0)) + 3 * std::size_t(pos.x), std::size_t(n) * 3 * sizeof(float));
  d->fk->displayStages(dst, n);
}
void blkBlend(const void* r, float* px, int n, const PixelPos&) { static_cast<const PtRes*>(r)->fk->blend(px, t_side, n); }

// ---- tile (window / whole-frame) nodes -------------------------------------------------------------------------------------
Status tileNRLuma(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  if (w->sp.nrLuma > 0.f) stageNRLuma(*t.img, *t.scratch, w->sp.nrLuma, w->scale);
  return Status::Ok;
}
Status tileNRChroma(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  if (w->sp.nrColor > 0.f) stageNRChroma(*t.img, *t.scratch, w->sp.nrColor, w->scale);
  return Status::Ok;
}
Status tileDehaze(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  if (w->sp.dehaze == 0.f) return Status::Ok;
  if (w->sp.dehaze > 0.f && !t.dehaze) return Status::Internal;  // the solve must exist before the apply
  stageDehazeApply(*t.img, *t.scratch, w->sp.dehaze, t.ox, t.oy, t.frameW, t.frameH, t.dehaze);
  return Status::Ok;
}
Status tileDetail(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  const SpatialParams& p = w->sp;
  if (!(p.clarity != 0.f || p.texture != 0.f || p.sharpen > 0.f)) return Status::Ok;
  if (p.clarity != 0.f && !t.guided) return Status::Internal;
  stageDetail(*t.img, *t.scratch, p, w->scale, t.guided, t.ox, t.oy, t.frameW, t.frameH);
  return Status::Ok;
}
Status tileLocalDetail(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  const local::Frame* f = w->fk->localFrame();
  if (!f || !f->anyWindow) return Status::Ok;
  if (!t.aux || !t.aux->data || t.aux->ox != t.ox || t.aux->oy != t.oy || t.aux->w != t.img->width || t.aux->h != t.img->height)
    return Status::Internal;  // the amounts must come from the head chain of this very window
  local::detailWindow(*f, *t.img, *t.aux, t.frameW, t.frameH, w->scale, w->fk);
  return Status::Ok;
}
Status tileGrain(CpuTile& t, const void* r) {
  const auto* w = static_cast<const WinRes*>(r);
  if (w->sp.grain > 0.f) stageGrain(*t.img, w->sp.grain, w->scale, t.ox, t.oy);
  return Status::Ok;
}
Status tileFilmSim(CpuTile& t, const void* r) {
  // FilmSim as a whole-frame node: the film render of the frame into t.img (super-node until the film nodes land)
  const auto* p = static_cast<const PtRes*>(r);
  return const_cast<FrameKernels*>(p->fk)->runFilm(*t.img) ? Status::Ok : Status::Unsupported;
}
// Pack8: clamp-free 8-bit encode of the interior of the tile (the chain already clamped to [0, 1])
Status tilePack8(CpuTile& t, const void* r) {
  (void)r;
  const ImageF& img = *t.img;
  const Rect& in = t.interior;
  const int ch = t.outChannels;
  {
    const int rows = in.y1 - in.y0;
    ThreadPool::instance().run(rows, 16, [&](int a, int b) {
      for (int yy = a; yy < b; ++yy) {
        const int y = in.y0 + yy;
        const float* s = img.row(y) + 3 * std::size_t(in.x0);
        std::uint8_t* o = t.out8 + (std::size_t(t.oy + y - t.outRow0) * t.frameW + std::size_t(t.ox + in.x0)) * ch;
        const int n = in.x1 - in.x0;
        for (int x = 0; x < n; ++x) {
          o[ch * x] = std::uint8_t(s[3 * x] * 255.f + 0.5f);
          o[ch * x + 1] = std::uint8_t(s[3 * x + 1] * 255.f + 0.5f);
          o[ch * x + 2] = std::uint8_t(s[3 * x + 2] * 255.f + 0.5f);
          if (ch == 4) o[4 * x + 3] = 255;
        }
      }
    });
  }
  return Status::Ok;
}

// ---- table nodes: CpuImpl::bake (the HostTable stage; docs/engine/pipeline-architecture.md 10.7 item 4) ---------------------------------
// `resolved` is the per-frame TableBakeCtx (engine_session.h), `inputs` the payloads of the node's table inputs in edge order
// (null = edge not wired). The payload is a heap object the caller wraps in a shared_ptr with `freePayload`.
const TableBakeCtx& tctx(const void* r) { return *static_cast<const TableBakeCtx*>(r); }
template <class T>
void freeAs(void* p) {
  delete static_cast<T*>(p);
}
Status bakeLattice(const void* r, std::span<const void* const>, void** out) {
  const TableBakeCtx& c = tctx(r);
  *out = new std::vector<float>(c.env->renderer.bakeColourLattice(c.env->src, c.n));
  return Status::Ok;
}
Status bakePre(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  // the fast analysis reads the lattice (moved into a scratch BakedLut and back: unchanged on return)
  auto* lattice = in.empty() ? nullptr : const_cast<std::vector<float>*>(static_cast<const std::vector<float>*>(in[0]));
  *out = new Renderer::TonePre(c.env->renderer.bakeTonePre(c.env->src, lattice, c.n, c.fast));
  return Status::Ok;
}
Status bakeToneTable(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  const auto* pre = in.empty() ? nullptr : static_cast<const Renderer::TonePre*>(in[0]);
  BakedLut tmp;
  c.env->renderer.bakeToneTable(c.env->src, pre ? *pre : Renderer::TonePre{}, tmp);
  *out = new ToneTableOut{std::move(tmp.tone), std::move(tmp.toneMove)};
  return Status::Ok;
}
Status bakeAnalysisNode(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  const auto* pre = in.empty() ? nullptr : static_cast<const Renderer::TonePre*>(in[0]);
  *out = new ToneAnalysis(c.env->renderer.bakeAnalysis(c.env->src, pre ? *pre : Renderer::TonePre{}));
  return Status::Ok;
}
Status bakeOverflow(const void* r, std::span<const void* const>, void** out) {
  const TableBakeCtx& c = tctx(r);
  BakedLut base;
  base.rgb.resize(4);  // only its presence matters: the overflow lattice is a function of the colour stage alone
  *out = new std::vector<float>(c.env->renderer.bakeOverflow(c.env->src, base, c.n, c.maxV));
  return Status::Ok;
}
// Resize: the frame-global step of the node (the source resampled to the requested long edge). Its payload is the scaled
// SourceImage the rest of the frame reads (the block fragment `blkCopy` then copies rows of it).
Status bakeResize(const void* r, std::span<const void* const>, void** out) {
  const auto& c = *static_cast<const ResizeBakeCtx*>(r);
  auto* img = new SourceImage();
  img->sceneReferred = c.full->sceneReferred;
  img->linear = resizeToLongEdge(c.full->linear, c.longEdge);
  *out = img;
  return Status::Ok;
}

const CpuImpl kSource{nullptr, blkCopy, nullptr, nullptr};
const CpuImpl kResize{nullptr, blkCopy, nullptr, bakeResize, freeAs<SourceImage>};
const CpuImpl kLattice{nullptr, nullptr, nullptr, bakeLattice, freeAs<std::vector<float>>};
const CpuImpl kPre{nullptr, nullptr, nullptr, bakePre, freeAs<Renderer::TonePre>};
const CpuImpl kToneTable{nullptr, nullptr, nullptr, bakeToneTable, freeAs<ToneTableOut>};
const CpuImpl kAnalysis{nullptr, nullptr, nullptr, bakeAnalysisNode, freeAs<ToneAnalysis>};
const CpuImpl kOverflow{nullptr, nullptr, nullptr, bakeOverflow, freeAs<std::vector<float>>};
const CpuImpl kGeometry{nullptr, blkGeometry, nullptr, nullptr};
const CpuImpl kFusedHead{nullptr, blkFusedHead, nullptr, nullptr};
const CpuImpl kGain{nullptr, blkGain, nullptr, nullptr};
const CpuImpl kHdr{nullptr, blkHdr, nullptr, nullptr};
const CpuImpl kToneColour{nullptr, blkToneColour, nullptr, nullptr};
const CpuImpl kVignette{nullptr, blkVignette, nullptr, nullptr};
const CpuImpl kLocalLinear{nullptr, blkLocal, nullptr, nullptr};
const CpuImpl kLocalDetail{nullptr, nullptr, tileLocalDetail, nullptr};
const CpuImpl kDisplay{nullptr, blkDisplay, nullptr, nullptr};
const CpuImpl kBlend{nullptr, blkBlend, nullptr, nullptr};
const CpuImpl kNRLuma{nullptr, nullptr, tileNRLuma, nullptr};
const CpuImpl kNRChroma{nullptr, nullptr, tileNRChroma, nullptr};
const CpuImpl kDehaze{nullptr, nullptr, tileDehaze, nullptr};
const CpuImpl kDetail{nullptr, nullptr, tileDetail, nullptr};
const CpuImpl kGrain{nullptr, nullptr, tileGrain, nullptr};
const CpuImpl kFilm{nullptr, nullptr, tileFilmSim, nullptr};
const CpuImpl kPack8{nullptr, nullptr, tilePack8, nullptr};

}  // namespace

const CpuImpl* cpuFusedHead() { return &kFusedHead; }

const CpuImpl* cpuImpl(NodeType t) {
  switch (t) {
    case NodeType::Source: return &kSource;
    case NodeType::Resize: return &kResize;
    case NodeType::Geometry: return &kGeometry;
    case NodeType::Gain: return &kGain;
    case NodeType::Hdr: return &kHdr;
    case NodeType::ToneColour:
    case NodeType::ToneColourExact: return &kToneColour;
    case NodeType::Vignette: return &kVignette;
    case NodeType::LocalLinear: return &kLocalLinear;
    case NodeType::LocalDetail: return &kLocalDetail;
    case NodeType::DisplayStages: return &kDisplay;
    case NodeType::Blend: return &kBlend;
    case NodeType::NRLuma: return &kNRLuma;
    case NodeType::NRChroma: return &kNRChroma;
    case NodeType::Dehaze: return &kDehaze;
    case NodeType::Detail: return &kDetail;
    case NodeType::Grain: return &kGrain;
    case NodeType::FilmSim: return &kFilm;
    case NodeType::Pack8: return &kPack8;
    case NodeType::ColourLattice: return &kLattice;
    case NodeType::ToneAnalysisPre: return &kPre;
    case NodeType::ToneTable: return &kToneTable;
    case NodeType::Analysis: return &kAnalysis;
    case NodeType::LatticeOverflow: return &kOverflow;
    default: return nullptr;  // LegacyRender has no plan
  }
}

bool bindResolved(NodeType t, const FrameEnv& env, Resolved& out) {
  switch (t) {
    case NodeType::DisplayStages:
      new (out.bytes) DispRes{env.fk, env.film, !env.blendFollows, env.filmRow0};
      return true;
    case NodeType::NRLuma:
    case NodeType::NRChroma:
    case NodeType::Dehaze:
    case NodeType::Detail:
    case NodeType::LocalDetail:
    case NodeType::Grain:
      new (out.bytes) WinRes{env.fk, env.fk->spatial(), env.fk->spatialScale()};  // read after the film outcome is known
      return true;
    case NodeType::Pack8: return true;
    case NodeType::LocalLinear:
      new (out.bytes) LocalRes{env.fk, env.aux};
      return true;
    default:
      if (!cpuImpl(t) || (!cpuImpl(t)->block && !cpuImpl(t)->tile)) return false;  // table nodes bake, they are not bound per frame
      new (out.bytes) PtRes{env.fk};
      return true;
  }
}

}  // namespace dapple::engine
