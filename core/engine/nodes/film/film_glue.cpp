// Film glue (docs/engine/pipeline-architecture.md sections 20 and 50): the film stage of a CPU frame. Builds the film's arguments from the
// edit and the point family's binding of the frame (its geometry, gain and local adjustments, CpuFrame) and runs it: whole frame
// (runFilm: the film nodes, or the FilmSimulator when the nodes are off), streamed to 8 bit (streamFilm) or in bands (filmStream*).
#include <cstring>

#include "dapple/film_match.h"
#include "engine/exec/cpu_executor.h"
#include "engine/nodes/point/cpu_impl.h"
#include "engine/nodes/point/scratch.h"
#include "engine/nodes/vignette/vignette_kernel.h"
#include "parallel.h"

namespace dapple::engine::point {

namespace {

constexpr float kWarmthFilterUnits = 12.f;  // enlarger filtration (CC-like units) at warmth / tint = +-100

film::FilmParams makeFilmParams(const EditParams& p, const FilmRun& run, SpatialQuality q, float formatScale) {
  film::FilmParams fp;
  fp.film = p.filmStock;
  fp.paper = run.paper;
  fp.camera.autoExposure = false;  // the develop pipeline's own exposure slider is the exposure control
  fp.enlarger.printExposure = std::exp2(-double(std::clamp(p.filmPrintEv, -9.f, 9.f)));  // print exposure (film_match.h)
  applyFilmPaperMapping(fp, p.filmBlack);  // paper white / black -> display white / black (stage C)
  applyFilmWb(fp, p);  // film white balance: the scene illuminant reaches the film with degree of adaptation 1 - film_wb
  // (the exposure offset is folded into the input gain in runFilmStage: it then costs no re-bake while dragging)
  fp.camera.filmFormatMm *= formatScale;
  // print filtration: more yellow / magenta filtration = less of that dye = cooler / greener print
  fp.enlarger.yFilterShift = -p.filmWarmth / 100.f * kWarmthFilterUnits;
  fp.enlarger.mFilterShift = -p.filmTint / 100.f * kWarmthFilterUnits;
  const double g = std::clamp(p.filmGrain / 100.f, 0.f, 2.f);
  if (g <= 0) {
    fp.filmRender.grain.active = false;
  } else {
    fp.filmRender.grain.particleAreaUm2 *= g * g;  // grain amplitude ~ sqrt(particle area)
  }
  const double h = std::clamp(p.filmHalation / 100.f, 0.f, 2.f);
  fp.filmRender.halation.halationAmount = h;  // 0..2: above 1 the halation is heavier than the stock's own
  fp.filmRender.halation.scatterAmount = std::min(h, 1.0);  // the scatter is a mix weight: above 1 it would extrapolate (negative lobe)
  if (q == SpatialQuality::Off) fp.disableSpatialAndStochastic();
  else if (q == SpatialQuality::Interactive) fp.settings.previewMode = true;
  return fp;
}

// Film simulation of a w x h frame into `out` (display sRGB-encoded, unclamped). `src` is the source in linear
// ProPhoto; when the frame is a resampling of it (geometry / region) `fetch(y, row)` produces row y (w * 3 floats,
// >= 0), otherwise `src` is passed on as it is. `gain` = white balance x exposure, folded into the input matrix.
// `pitch` = size of an output pixel in source pixels (spatial effects are defined in film-frame micrometres).
// The arguments of the film nodes for a w x h frame of a srcW x srcH source (the film nodes of point::CpuFrame, whole frame and streamed).
engine::filmnode::FilmFrameArgs filmNodeArgs(const EditParams& p, const FilmRun& run, const float gain[3], int srcW, int srcH, int w, int h, float pitch,
                                             SpatialQuality q, const FastOptions* fo) {
  const float formatScale = pitch * float(std::max(w, h)) / float(std::max(srcW, srcH));
  engine::filmnode::FilmFrameArgs a;
  a.lib = run.lib.get();
  a.stock = p.filmStock.c_str();
  a.paper = run.paper.c_str();
  for (int c = 0; c < 3; ++c) a.gain[c] = gain[c];
  a.filmExposure = p.filmExposure + p.filmKeyEv;
  a.printEv = p.filmPrintEv;
  a.black = p.filmBlack;
  {
    double sx, sy, sd;
    filmInputAdaptation(p, &sx, &sy, &sd);
    a.sceneX = float(sx), a.sceneY = float(sy), a.adapt = float(sd);
  }
  a.warmth = p.filmWarmth;
  a.tint = p.filmTint;
  a.grainPct = p.filmGrain;
  a.halationPct = p.filmHalation;
  a.formatScale = formatScale;
  a.w = w;
  a.h = h;
  a.quality = q == SpatialQuality::Off ? 0 : (q == SpatialQuality::Interactive ? 1 : 2);
  if (fo) {
    a.keepGrain = fo->keepGrain;
    a.coarse = fo->coarseFilm;
    a.fullLongEdge = fo->fullLongEdge;
    a.frameIndex = fo->frameIndex;
  }
  return a;
}

template <class Fetch>
bool runFilmStage(const EditParams& p, const FilmRun& run, const float gain[3], const ImageF& src, bool resample, int w,
                  int h, float pitch, SpatialQuality q, Fetch&& fetch, ImageF& out, const FastOptions* fo = nullptr) {
  if (!run.stock) return false;
  const float formatScale = pitch * float(std::max(w, h)) / float(std::max(src.width, src.height));
  const float evGain = std::exp2(p.filmExposure + p.filmKeyEv);
  const float gainEv[3] = {gain[0] * evGain, gain[1] * evGain, gain[2] * evGain};
  if (engine::filmnode::enabled()) {
    // the film as engine nodes (core/engine/nodes/film): byte-identical with the FilmSimulator below
    const engine::filmnode::FilmFrameArgs a = filmNodeArgs(p, run, gain, src.width, src.height, w, h, pitch, q, fo);
    ScratchImage nodeScratch;
    const ImageF* nin = &src;
    if (resample) {
      ImageF& lin = nodeScratch.get(w, h);
      parallelRows(h, [&](int y) { fetch(y, lin.row(y)); });
      nin = &lin;
    }
    if (out.width != nin->width || out.height != nin->height) out = ImageF(nin->width, nin->height);
    if (nin->width == w && nin->height == h &&
        engine::filmnode::renderFilmFrame(a, nin->data.data(), std::size_t(nin->width) * 3, out.data.data(), std::size_t(out.width) * 3))
      return true;
  }
  film::SimulatorLease lease = film::acquireSimulator(run.lib, p.filmStock, run.paper);
  std::string err;
  if (!lease->setParams(makeFilmParams(p, run, q, formatScale), &err)) return false;
  ScratchImage linScratch;
  const ImageF* in = &src;
  if (resample) {
    ImageF& lin = linScratch.get(w, h);
    parallelRows(h, [&](int y) { fetch(y, lin.row(y)); });
    in = &lin;
  }
  const bool ok = lease->render(*in, out, nullptr, gainEv);
  return ok;
}

}  // namespace

bool CpuFrame::streamFilm(std::uint8_t* rgba) {
  Impl& m = *impl_;
  if (m.exact || !(m.fr.stock && m.fr.strength >= 1.f) || !rgba || m.win) return false;
  if (frameBlocks().anyWindow()) return false;  // a neighbourhood stage of a migrated family runs
  if (m.opt.keepGrain || (m.opt.fullLongEdge > 0 && m.opt.spatial == SpatialQuality::Full)) return false;  // scale-aware / kept grain: node chain only
  const Params& rr = m.r;
  const int w = m.b.geo.outW, h = m.b.geo.outH;
  film::SimulatorLease lease = film::acquireSimulator(m.fr.lib, rr.p.filmStock, m.fr.paper);
  std::string err;
  const float evGain = std::exp2(rr.p.filmExposure + rr.p.filmKeyEv);
  const float gainEv[3] = {m.b.gain[0] * evGain, m.b.gain[1] * evGain, m.b.gain[2] * evGain};
  if (!(lease && lease->setParams(makeFilmParams(rr.p, m.fr, m.opt.spatial, m.b.geo.pitch()), &err))) return false;
  const bool plain = rr.curves.master.identity && rr.curves.red.identity && rr.curves.green.identity && rr.curves.blue.identity &&
                     !(rr.lut && rr.p.lutAmount > 0);
  const engine::vignette::Block vig = engine::vignette::resolve(rr.p, geometryFacts(m.b.geo));
  auto sink = [&](int y, float* t, int W) {
    std::uint8_t* o = rgba + std::size_t(y) * W * 4;
    if (!plain || vig.active()) {
      for (int x = 0; x < W; ++x) {
        float e3[3] = {t[3 * x], t[3 * x + 1], t[3 * x + 2]};
        if (!plain) point::displayStages(rr, e3);
        if (vig.active()) engine::vignette::applyAt(vig, e3, x, y, false);  // the film's display values are sRGB here
        t[3 * x] = e3[0], t[3 * x + 1] = e3[1], t[3 * x + 2] = e3[2];
      }
    }
    for (int i = 0; i < W; ++i) {  // clamp + encode (vectorises: fixed trip count, no branches)
      const float rr_ = std::min(std::max(t[3 * i], 0.f), 1.f), g = std::min(std::max(t[3 * i + 1], 0.f), 1.f),
                  b = std::min(std::max(t[3 * i + 2], 0.f), 1.f);
      o[4 * i] = std::uint8_t(rr_ * 255.f + 0.5f);
      o[4 * i + 1] = std::uint8_t(g * 255.f + 0.5f);
      o[4 * i + 2] = std::uint8_t(b * 255.f + 0.5f);
      o[4 * i + 3] = 255;
    }
  };
  auto tramp = [](void* u, int y, float* t, int W) { (*static_cast<decltype(sink)*>(u))(y, t, W); };
  auto fetchRowFn = [&](int y, float* row) {
    for (int x = 0; x < w; ++x) m.fetchPixel(x, y, row + 3 * x);
  };
  auto fetchTramp = [](void* u, int y, float* row) { (*static_cast<decltype(fetchRowFn)*>(u))(y, row); };
  const bool resampled = m.b.geo.useGeo;  // geometry, region or a reshaping field
  return lease->renderRows(m.in, w, h, gainEv, resampled ? fetchTramp : nullptr, &fetchRowFn, tramp, &sink);
}

bool CpuFrame::runFilm(ImageF& F, const local::AuxTarget* aux) {
  Impl& m = *impl_;
  m.filmRan = true;
  if (!m.fr.stock || m.win) return false;  // a streaming export never runs the whole-frame film branch here (section 42)
  const int w = m.b.geo.outW, h = m.b.geo.outH;
  float g[3];
  if (m.exact) {
    for (int c = 0; c < 3; ++c) g[c] = m.exactOp->wb[c] * m.exactOp->exposureGain;
  } else {
    for (int c = 0; c < 3; ++c) g[c] = m.b.gain[c];
  }
  const SpatialQuality q = m.exact ? (m.opt.spatial == SpatialQuality::Off ? SpatialQuality::Off : SpatialQuality::Full)
                                   : m.opt.spatial;
  const local::Frame* lf = m.local.get();
  m.filmOkV = runFilmStage(
      m.r.p, m.fr, g, m.in, m.b.geo.useGeo || lf, w, h, m.b.geo.pitch(), q,
      [&](int y, float* row) {
        if (m.b.geo.useGeo) {
          for (int x = 0; x < w; ++x) m.fetchPixel(x, y, row + 3 * x);
        } else {
          std::memcpy(row, m.in.row(y), std::size_t(w) * 3 * sizeof(float));
        }
        if (!lf) return;
        // local adjustments, stage L, on the film's scene input (docs section 40.7): in the gained domain, back to the film's input domain
        const bool wantK = aux && aux->data && lf->anyWindow;
        float K[kLocalWindowSlots];
        for (int x = 0; x < w; ++x) {
          float* p = row + 3 * x;
          const float src[3] = {p[0], p[1], p[2]};
          float lin[3] = {std::max(0.f, p[0] * g[0]), std::max(0.f, p[1] * g[1]), std::max(0.f, p[2] * g[2])};
          float u, v;
          m.srcUV(x, y, u, v);
          local::applyLinear(*lf, u, v, src, lin, wantK ? K : nullptr);
          for (int c = 0; c < 3; ++c) p[c] = g[c] > 0.f ? lin[c] / g[c] : 0.f;
          if (wantK) {
            const std::size_t o = std::size_t(y - aux->oy) * aux->w + std::size_t(x - aux->ox);
            for (int k = 0; k < kLocalWindowSlots; ++k)
              if (aux->slot[k] >= 0) aux->data[std::size_t(aux->slot[k]) * aux->plane + o] = K[k];
          }
        }
      },
      F, &m.opt);
  return m.filmOkV;
}

// ---- streaming export, film branch (section 42.6) ----------------------------------------------------------------------------------
void CpuFrame::setSourceProvider(std::function<bool(int, int, int, int)> fn) { impl_->sourceFn = std::move(fn); }

namespace {
// The film's input of output pixels [x0, x1) of row y (pre-gain source, through the geometry), then stage L of the local adjustments
// (CpuFrame::runFilm's fetch, without the aux writes). The source window must cover them.
}  // namespace

bool CpuFrame::filmStreamBegin(std::string* why) {
  Impl& m = *impl_;
  m.filmRan = true;
  if (!m.fr.stock || !m.sourceFn || !engine::filmnode::enabled()) {
    if (why) *why = "film: no stock / no source provider / film nodes off";
    return false;
  }
  const int w = m.b.geo.outW, h = m.b.geo.outH;
  float g[3];
  if (m.exact) {
    for (int c = 0; c < 3; ++c) g[c] = m.exactOp->wb[c] * m.exactOp->exposureGain;
  } else {
    for (int c = 0; c < 3; ++c) g[c] = m.b.gain[c];
  }
  const SpatialQuality q = m.exact ? (m.opt.spatial == SpatialQuality::Off ? SpatialQuality::Off : SpatialQuality::Full) : m.opt.spatial;
  const engine::filmnode::FilmFrameArgs a = filmNodeArgs(m.r.p, m.fr, g, m.in.width, m.in.height, w, h, m.b.geo.pitch(), q, &m.opt);
  const local::Frame* lf = m.local.get();
  const float gg[3] = {g[0], g[1], g[2]};
  auto input = [this, w, lf, gg](int y0, int y1, float* rgb) -> bool {
    Impl& mm = *impl_;
    // source windows in column chunks when the geometry slants (straighten): the window of a full-width band would be tall
    int chunk = w;
    if (mm.b.geo.useGeo) {
      int sx0, sy0, sx1, sy1;
      sourceRect(0, y0, w, y1, sx0, sy0, sx1, sy1);
      if (double(sx1 - sx0) * double(sy1 - sy0) > 2.5 * double(w) * double(y1 - y0)) chunk = 512;
    }
    if (!rgb) {  // prefetch: the source window of all the rows (the slices that follow fall inside it)
      if (chunk < w) return true;
      int sx0, sy0, sx1, sy1;
      sourceRect(0, y0, w, y1, sx0, sy0, sx1, sy1);
      return mm.sourceFn(sx0, sy0, sx1, sy1);
    }
    for (int cx = 0; cx < w; cx += chunk) {
      const int cx1 = std::min(w, cx + chunk);
      int sx0, sy0, sx1, sy1;
      sourceRect(cx, y0, cx1, y1, sx0, sy0, sx1, sy1);
      if (!mm.sourceFn(sx0, sy0, sx1, sy1)) return false;
      parallelFor(
          y1 - y0,
          [&](int k) {
            const int y = y0 + k;
            float* row = rgb + (std::size_t(k) * w + cx) * 3;
            if (mm.b.geo.useGeo) {
              for (int x = cx; x < cx1; ++x) mm.fetchPixel(x, y, row + 3 * (x - cx));
            } else {
              copyRow(row, cx1 - cx, cx, y);
            }
            if (!lf) return;
            for (int x = cx; x < cx1; ++x) {  // stage L on the film's scene input, as runFilm does
              float* p = row + 3 * (x - cx);
              const float src[3] = {p[0], p[1], p[2]};
              float lin[3] = {std::max(0.f, p[0] * gg[0]), std::max(0.f, p[1] * gg[1]), std::max(0.f, p[2] * gg[2])};
              float u, v;
              mm.srcUV(x, y, u, v);
              local::applyLinear(*lf, u, v, src, lin, nullptr);
              for (int c = 0; c < 3; ++c) p[c] = gg[c] > 0.f ? lin[c] / gg[c] : 0.f;
            }
          },
          4);
    }
    return true;
  };
  m.filmBands = std::make_unique<engine::filmnode::FilmBands>();
  if (!m.filmBands->init(a, input, why)) {
    m.filmBands.reset();
    return false;
  }
  m.filmOkV = true;
  return true;
}

bool CpuFrame::filmStreamRows(int y0, int y1, ImageF& out) {
  Impl& m = *impl_;
  if (!m.filmBands) return false;
  const int w = m.b.geo.outW;
  if (out.width != w || out.height != y1 - y0) {
    out.width = w, out.height = y1 - y0;
    out.data.resize(std::size_t(w) * std::max(0, y1 - y0) * 3);
  }
  return m.filmBands->rows(y0, y1, out.data.data(), std::size_t(w) * 3);
}

bool CpuFrame::filmStreamAux(int x0, int y0, int x1, int y1, const local::AuxTarget& aux) {
  Impl& m = *impl_;
  const local::Frame* lf = m.local.get();
  if (!lf || !lf->anyWindow || !aux.data) return true;
  float g[3];
  if (m.exact) {
    for (int c = 0; c < 3; ++c) g[c] = m.exactOp->wb[c] * m.exactOp->exposureGain;
  } else {
    for (int c = 0; c < 3; ++c) g[c] = m.b.gain[c];
  }
  int sx0, sy0, sx1, sy1;
  sourceRect(x0, y0, x1, y1, sx0, sy0, sx1, sy1);
  if (!m.sourceFn || !m.sourceFn(sx0, sy0, sx1, sy1)) return false;
  parallelFor(
      y1 - y0,
      [&](int k) {
        const int y = y0 + k;
        float K[kLocalWindowSlots];
        for (int x = x0; x < x1; ++x) {
          float p[3];
          if (m.b.geo.useGeo) m.fetchPixel(x, y, p);
          else copyRow(p, 1, x, y);
          const float src[3] = {p[0], p[1], p[2]};
          float lin[3] = {std::max(0.f, p[0] * g[0]), std::max(0.f, p[1] * g[1]), std::max(0.f, p[2] * g[2])};
          float u, v;
          m.srcUV(x, y, u, v);
          local::applyLinear(*lf, u, v, src, lin, K);
          const std::size_t o = std::size_t(y - aux.oy) * aux.w + std::size_t(x - aux.ox);
          for (int s = 0; s < kLocalWindowSlots; ++s)
            if (aux.slot[s] >= 0) aux.data[std::size_t(aux.slot[s]) * aux.plane + o] = K[s];
        }
      },
      4);
  return true;
}

}  // namespace dapple::engine::point
