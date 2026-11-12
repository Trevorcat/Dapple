#include "engine/video/video_frame_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "color.h"
#include "dapple/film_match.h"
#include "engine/exec/source_store.h"
#include "parallel.h"

namespace dapple::engine::video {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// half -> float for every code (256 KB, built once)
const float* halfTable() {
  static const std::vector<float> t = [] {
    std::vector<float> v(65536);
    for (std::uint32_t i = 0; i < 65536; ++i) {
      const std::uint32_t s = (i & 0x8000u) << 16, e = (i >> 10) & 0x1fu, m = i & 0x3ffu;
      std::uint32_t bits;
      if (e == 0) {
        if (m == 0) {
          bits = s;
        } else {  // subnormal
          int sh = 0;
          std::uint32_t mm = m;
          while (!(mm & 0x400u)) mm <<= 1, ++sh;
          bits = s | (std::uint32_t(127 - 15 - sh + 1) << 23) | ((mm & 0x3ffu) << 13);
        }
      } else if (e == 31) {
        bits = s | 0x7f800000u | (m << 13);
      } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
      }
      std::memcpy(&v[i], &bits, 4);
    }
    return v;
  }();
  return t.data();
}

const std::uint8_t kBayer8[64] = {0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46,
                                  6,  38, 60, 28, 52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27,
                                  49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};

inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

}  // namespace

float halfToFloat(std::uint16_t h) { return halfTable()[h]; }

bool VideoFrameSession::toSource(const InputFrame& f, const Config& cfg, SourceImage& out, std::string* err) {
  if (!f.rgbaF16 || f.width < 2 || f.height < 2) {
    if (err) *err = "empty video frame";
    return false;
  }
  const int rot = ((f.rotationDeg % 360) + 360) % 360;
  if (rot % 90 != 0) {
    if (err) *err = "rotation must be a multiple of 90";
    return false;
  }
  const int sw = f.width, sh = f.height;
  const int dw = (rot == 90 || rot == 270) ? sh : sw, dh = (rot == 90 || rot == 270) ? sw : sh;
  // centre crop to the still's aspect (the clip of a Motion Photo normally has it; a clip of another shape is fitted, never stretched)
  int cw = dw, ch = dh, cx0 = 0, cy0 = 0;
  if (cfg.stillAspect > 0.f) {
    const float a = cfg.stillAspect, have = float(dw) / float(dh);
    if (std::fabs(have - a) / a > 0.01f) {
      if (have > a) cw = std::max(2, int(std::lround(float(dh) * a)) & ~1);
      else ch = std::max(2, int(std::lround(float(dw) / a)) & ~1);
      cx0 = (dw - cw) / 2;
      cy0 = (dh - ch) / 2;
    }
  }
  const float* H = halfTable();
  const auto& toPro = color::srgbToProPhoto();
  ImageF img(cw, ch);
  parallelRows(ch, [&](int y) {
    float* d = img.row(y);
    const int yd = y + cy0;
    for (int x = 0; x < cw; ++x) {
      const int xd = x + cx0;
      int sx, sy;
      switch (rot) {
        case 90: sx = yd; sy = sh - 1 - xd; break;
        case 180: sx = sw - 1 - xd; sy = sh - 1 - yd; break;
        case 270: sx = sw - 1 - yd; sy = xd; break;
        default: sx = xd; sy = yd; break;
      }
      const std::uint16_t* s = f.rgbaF16 + (std::size_t(sy) * sw + sx) * 4;
      const float lin[3] = {H[s[0]], H[s[1]], H[s[2]]};
      toPro.apply(lin, d + 3 * x);
    }
  });
  out = SourceImage{};
  out.sceneReferred = f.kind == ColourKind::Slog3 || f.kind == ColourKind::Hlg || f.kind == ColourKind::Pq;
  out.linear = cfg.maxLongEdge > 0 && std::max(cw, ch) > cfg.maxLongEdge ? resizeToLongEdge(img, cfg.maxLongEdge) : std::move(img);
  out.meta.sensorWidth = out.linear.width;
  out.meta.sensorHeight = out.linear.height;
  out.meta.cfa = CfaKind::None;
  out.meta.bitsPerSample = 16;
  out.meta.asShotIlluminantXy[0] = cfg.sceneIlluminantX;
  out.meta.asShotIlluminantXy[1] = cfg.sceneIlluminantY;
  return true;
}

std::size_t VideoFrameSession::packedBytes(PackFormat fmt, int w, int h) {
  const std::size_t W = std::size_t(w & ~1), H = std::size_t(h & ~1);
  switch (fmt) {
    case PackFormat::Rgba8: return std::size_t(w) * h * 4;
    case PackFormat::Rgb10A2:
    case PackFormat::Rgb10A2Hdr: return std::size_t(w) * h * 4;
    case PackFormat::I420: return W * H * 3 / 2;
    default: return W * H * 3;  // P010: 2 bytes per sample, 1.5 samples per pixel
  }
}

void VideoFrameSession::pack(const ImageF& img, PackFormat fmt, std::vector<std::uint8_t>& out, int* wOut, int* hOut) {
  const int w = img.width, h = img.height;
  if (fmt == PackFormat::Rgba8) {
    out.resize(packedBytes(fmt, w, h));
    parallelRows(h, [&](int y) {
      const float* s = img.row(y);
      std::uint8_t* d = out.data() + std::size_t(y) * w * 4;
      for (int x = 0; x < w; ++x) {
        d[4 * x] = std::uint8_t(clamp01(s[3 * x]) * 255.f + 0.5f);
        d[4 * x + 1] = std::uint8_t(clamp01(s[3 * x + 1]) * 255.f + 0.5f);
        d[4 * x + 2] = std::uint8_t(clamp01(s[3 * x + 2]) * 255.f + 0.5f);
        d[4 * x + 3] = 255;
      }
    });
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    return;
  }
  if (fmt == PackFormat::Rgb10A2 || fmt == PackFormat::Rgb10A2Hdr) {
    out.resize(packedBytes(fmt, w, h));
    parallelRows(h, [&](int y) {
      const float* s = img.row(y);
      auto* d = reinterpret_cast<std::uint32_t*>(out.data()) + std::size_t(y) * w;
      for (int x = 0; x < w; ++x) {
        const std::uint32_t r = std::uint32_t(clamp01(s[3 * x]) * 1023.f + 0.5f), g = std::uint32_t(clamp01(s[3 * x + 1]) * 1023.f + 0.5f),
                            b = std::uint32_t(clamp01(s[3 * x + 2]) * 1023.f + 0.5f);
        d[x] = r | (g << 10) | (b << 20) | (3u << 30);
      }
    });
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    return;
  }
  const int W = w & ~1, H = h & ~1;
  if (wOut) *wOut = W;
  if (hOut) *hOut = H;
  out.resize(packedBytes(fmt, w, h));
  // Non-constant luminance on the display-encoded values: BT.709 for SDR (the same convention as the video core's I420 / P010 outputs), BT.2020 NCL
  // for the HDR variant (the caller passes the HLG / PQ encoded BT.2020 frame).
  const bool wide = fmt == PackFormat::P010Hdr;
  const float kr = wide ? 0.2627f : 0.2126f, kb = wide ? 0.0593f : 0.0722f, kg = 1.f - kr - kb;
  const float cbDiv = 2.f * (1.f - kb), crDiv = 2.f * (1.f - kr);
  const bool ten = fmt == PackFormat::P010 || fmt == PackFormat::P010Hdr;
  std::uint8_t* yPlane = out.data();
  std::uint8_t* uvPlane = out.data() + std::size_t(W) * H * (ten ? 2 : 1);
  std::uint8_t* uPlane = uvPlane;
  std::uint8_t* vPlane = uvPlane + std::size_t(W / 2) * (H / 2);
  parallelRows(H / 2, [&](int cy) {
    for (int cx = 0; cx < W / 2; ++cx) {
      float cbSum = 0.f, crSum = 0.f;
      for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
          const int x = 2 * cx + i, y = 2 * cy + j;
          const float* p = img.row(y) + 3 * x;
          const float r = clamp01(p[0]), g = clamp01(p[1]), b = clamp01(p[2]);
          const float yy = kr * r + kg * g + kb * b;
          cbSum += (b - yy) / cbDiv;
          crSum += (r - yy) / crDiv;
          if (ten) {
            const std::uint16_t v = std::uint16_t(std::uint16_t(std::lround(64.f + 876.f * yy)) << 6);
            std::memcpy(yPlane + (std::size_t(y) * W + x) * 2, &v, 2);
          } else {
            const float t = (kBayer8[((y & 7) << 3) | (x & 7)] + 0.5f) / 64.f;
            yPlane[std::size_t(y) * W + x] = std::uint8_t(std::min(235.f, 16.f + 219.f * yy + t));
          }
        }
      const float cb = cbSum * 0.25f, cr = crSum * 0.25f;
      if (ten) {
        const std::uint16_t u = std::uint16_t(std::uint16_t(std::lround(512.f + 896.f * cb)) << 6);
        const std::uint16_t v = std::uint16_t(std::uint16_t(std::lround(512.f + 896.f * cr)) << 6);
        std::uint8_t* d = uvPlane + (std::size_t(cy) * (W / 2) + cx) * 4;
        std::memcpy(d, &u, 2);
        std::memcpy(d + 2, &v, 2);
      } else {
        const float t = (kBayer8[((cy & 7) << 3) | (cx & 7)] + 0.5f) / 64.f;
        uPlane[std::size_t(cy) * (W / 2) + cx] = std::uint8_t(std::min(240.f, std::max(16.f, 128.f + 224.f * cb + t)));
        vPlane[std::size_t(cy) * (W / 2) + cx] = std::uint8_t(std::min(240.f, std::max(16.f, 128.f + 224.f * cr + t)));
      }
    }
  });
}

namespace {
float hlgOetf(float e) {  // ARIB STD-B67, e = scene linear 0..1
  const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
  e = std::max(e, 0.f);
  return e <= 1.f / 12.f ? std::sqrt(3.f * e) : a * std::log(12.f * e - b) + c;
}
float pqOetf(float nits) {  // SMPTE ST 2084, absolute luminance in nits
  const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const double y = std::pow(std::max(0.0, double(nits)) / 10000.0, m1);
  return float(std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2));
}
// linear BT.709 -> linear BT.2020
const float kTo2020[9] = {0.6274039f, 0.3292830f, 0.0433131f, 0.0690973f, 0.9195404f, 0.0113623f, 0.0163914f, 0.0880133f, 0.8955953f};
}  // namespace

bool VideoFrameSession::toHdrEncoded(const ImageF& sdrEnc, const SourceImage& src, float gainEv, OutputKind kind, ImageF& out) {
  if (src.linear.width != sdrEnc.width || src.linear.height != sdrEnc.height || kind == OutputKind::Sdr709) return false;
  const float g = std::exp2(gainEv);
  const auto& toSrgb = color::proPhotoToSrgb();
  out = ImageF(sdrEnc.width, sdrEnc.height);
  const bool hlg = kind == OutputKind::Hlg2020;
  parallelRows(sdrEnc.height, [&](int y) {
    const float* e = sdrEnc.row(y);
    const float* s = src.linear.row(y);
    float* o = out.row(y);
    for (int x = 0; x < sdrEnc.width; ++x) {
      float lin[3];
      for (int c = 0; c < 3; ++c) lin[c] = color::srgbDecode(clamp01(e[3 * x + c]));
      // the part of the source above SDR white (luminance based, so the hue of the highlight is the source's), after the edit's exposure
      const float Y = (0.2880402f * s[3 * x] + 0.7118741f * s[3 * x + 1] + 0.0000857f * s[3 * x + 2]) * g;
      const float r = Y > 1.f ? 1.f - 1.f / Y : 0.f;
      const float pp[3] = {s[3 * x] * g * r, s[3 * x + 1] * g * r, s[3 * x + 2] * g * r};
      float ex[3];
      toSrgb.apply(pp, ex);
      for (int c = 0; c < 3; ++c) lin[c] = std::max(0.f, lin[c] + std::max(0.f, ex[c]));
      for (int c = 0; c < 3; ++c) {
        const float L = kTo2020[3 * c] * lin[0] + kTo2020[3 * c + 1] * lin[1] + kTo2020[3 * c + 2] * lin[2];
        o[3 * x + c] = clamp01(hlg ? hlgOetf(L * 0.2650f) : pqOetf(L * 203.f));
      }
    }
  });
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
struct VideoFrameSession::Impl {
  Config cfg;
  EditParams held;  // cfg.params + the film match of the reference
  std::shared_ptr<SourceImage> proxy;
  std::unique_ptr<Renderer> renderer;
  EngineSession es;
  bool ready = false;
  int refW = 0, refH = 0, outW = 0, outH = 0;
  OutputKind outKind = OutputKind::Sdr709;
  std::string outNote = "SDR source or SDR by policy";
  ColourKind refKind = ColourKind::Standard;
  std::mutex mu;

  void freezeFrom(const SourceImage& ref) {
    proxy = std::make_shared<SourceImage>();
    proxy->sceneReferred = ref.sceneReferred;
    proxy->linear = resizeToLongEdge(ref.linear, 256);
    refW = ref.linear.width;
    refH = ref.linear.height;
    rematch();
    decideOutput();
    ready = true;
  }
  // The HDR policy (docs/engine/pipeline-architecture.md section 30.8).
  void decideOutput() {
    outKind = OutputKind::Sdr709;
    const bool hdrSource = refKind == ColourKind::Hlg || refKind == ColourKind::Pq;
    const bool geometry = (!held.crop.empty() && held.crop != "orig") || held.rotate != 0 || held.straighten != 0.f || held.flip;
    if (!hdrSource) outNote = refKind == ColourKind::Slog3 ? "S-Log3 source: converted to SDR BT.709 (as the unedited clip is)" : "SDR source";
    else if (!cfg.allowHdr) outNote = "HDR source, HDR output switched off: SDR BT.709";
    else if (!held.filmStock.empty()) outNote = "film is a print: SDR BT.709 (10-bit Main10)";
    else if (geometry) outNote = "HDR source with a geometry edit (crop / rotate / straighten / flip): SDR BT.709, the highlight headroom is not warped";
    else {
      outKind = refKind == ColourKind::Hlg ? OutputKind::Hlg2020 : OutputKind::Pq2020;
      outNote = refKind == ColourKind::Hlg ? "HLG source without film: stays HLG BT.2020" : "PQ source without film: stays PQ BT.2020";
    }
  }
  // The film exposure of the clip: metered once on the reference proxy, held for every frame.
  void rematch() {
    held = cfg.params;
    held.filmKeyEv = 0.f;
    held.filmPrintEv = 0.f;
    held.filmSceneX = cfg.sceneIlluminantX;
    held.filmSceneY = cfg.sceneIlluminantY;
    if (!held.filmStock.empty()) {
      const FilmExposure ev = matchFilmExposure(held, cfg.camera, cfg.lut.get(), *proxy);
      held.filmKeyEv = ev.filmEv;
      held.filmPrintEv = ev.printEv;
    }
    renderer = std::make_unique<Renderer>(held, cfg.camera, cfg.lut.get());
    renderer->setToneProxy(proxy.get());
  }

  bool run(const SourceImage& src, std::uint32_t frameIndex, bool wantFloat, ImageF* outF, std::vector<std::uint8_t>* rgba, FrameInfo* info,
           std::string* err) {
    FrameJob job{held, *renderer, src, proxy.get(), cfg.camera, cfg.lut.get(), {}, {}, {}};
    job.holdSolves = true;
    job.request.interactive = false;
    job.request.bakeSize = cfg.bakeSize;
    job.request.noise = true;
    job.fastOptions.spatial = cfg.spatial;
    job.fastOptions.noise = true;
    job.fastOptions.frameIndex = frameIndex;
    const int own = std::max(src.linear.width, src.linear.height);
    job.fastOptions.fullLongEdge = cfg.stillLongEdge > own ? cfg.stillLongEdge : 0;
    ImageF f;
    std::unique_ptr<MemorySourceStore> store;
    if (wantFloat) {
      job.outFloat = &f;
    } else if (cfg.allowGpu) {
      store = std::make_unique<MemorySourceStore>(src);
      job.store = store.get();
    }
    FrameResult res;
    if (!es.renderFrame(job, res)) {
      if (err) *err = res.error.empty() ? "engine failure" : res.error;
      return false;
    }
    if (info) {
      info->executor = res.executor;
      info->outBits = res.floatOut ? 32 : 8;
      info->tablesHit = res.bake == FrameResult::Bake::FullHit;
      info->width = res.w;
      info->height = res.h;
    }
    if (res.floatOut) {
      if (outF) *outF = std::move(f);
    } else if (rgba && res.rgba) {
      rgba->assign(res.rgba, res.rgba + std::size_t(res.w) * res.h * 4);
    }
    std::free(res.rgba);
    return true;
  }
};

CameraProfile VideoFrameSession::videoCamera() {
  CameraProfile p;
  p.id = "dapple.video";
  p.make = "";
  p.model = "";
  p.displayName = "Video";
  p.baselineExposureEv = kVideoBaselineEv;
  p.baselineContrast = 0.f;
  return p;
}

VideoFrameSession::VideoFrameSession(Config cfg) : d_(std::make_unique<Impl>()) {
  d_->cfg = std::move(cfg);
  d_->cfg.camera = videoCamera();
}
VideoFrameSession::~VideoFrameSession() = default;

bool VideoFrameSession::hasReference() const { return d_->ready; }
OutputKind VideoFrameSession::outputKind() const { return d_->outKind; }
const char* VideoFrameSession::outputNote() const { return d_->outNote.c_str(); }
void VideoFrameSession::outputTags(int out[4]) const {
  out[3] = 1;
  if (d_->outKind == OutputKind::Sdr709) out[0] = 1, out[1] = 1, out[2] = 1;
  else out[0] = 9, out[1] = d_->outKind == OutputKind::Hlg2020 ? 18 : 16, out[2] = 9;
}
const EditParams& VideoFrameSession::heldParams() const { return d_->held; }
int VideoFrameSession::frameWidth() const { return d_->refW; }
int VideoFrameSession::frameHeight() const { return d_->refH; }
int VideoFrameSession::outputWidth() const { return d_->outW & ~1; }
int VideoFrameSession::outputHeight() const { return d_->outH & ~1; }
EngineSession& VideoFrameSession::engine() { return d_->es; }

bool VideoFrameSession::setReferenceStill(const SourceImage& still, std::string* err) {
  std::lock_guard<std::mutex> lk(d_->mu);
  if (d_->refKind == ColourKind::Standard) d_->refKind = still.sceneReferred ? ColourKind::Slog3 : ColourKind::Standard;
  d_->freezeFrom(still);
  // warm the tables from the reference: the clip's tone analysis / lattice / film tables are this frame's
  ImageF scratch;
  if (!d_->run(still, 0, true, &scratch, nullptr, nullptr, err)) return false;
  d_->outW = scratch.width;
  d_->outH = scratch.height;
  return true;
}

bool VideoFrameSession::setReference(const InputFrame& f, std::string* err) {
  SourceImage src;
  if (!toSource(f, d_->cfg, src, err)) return false;
  d_->refKind = f.kind;
  return setReferenceStill(src, err);
}

bool VideoFrameSession::setEdit(const EditParams& params, std::string* err) {
  (void)err;
  std::lock_guard<std::mutex> lk(d_->mu);
  d_->cfg.params = params;
  if (!d_->ready) return true;
  d_->rematch();
  d_->decideOutput();
  return true;
}

bool VideoFrameSession::renderFloat(const InputFrame& f, std::uint32_t frameIndex, ImageF& out, FrameInfo* info, std::string* err) {
  SourceImage src;
  return renderFloatSrc(f, frameIndex, out, src, info, err);
}

bool VideoFrameSession::renderFloatSrc(const InputFrame& f, std::uint32_t frameIndex, ImageF& out, SourceImage& src, FrameInfo* info, std::string* err) {
  const auto t0 = Clock::now();
  if (!toSource(f, d_->cfg, src, err)) return false;
  const double conv = msSince(t0);
  std::lock_guard<std::mutex> lk(d_->mu);
  if (!d_->ready) {
    d_->refKind = f.kind;
    d_->freezeFrom(src);
  } else if (src.linear.width != d_->refW || src.linear.height != d_->refH) {
    if (err) *err = "the frame size changed inside the clip";
    return false;
  }
  const auto t1 = Clock::now();
  if (!d_->run(src, frameIndex, true, &out, nullptr, info, err)) return false;
  if (d_->outW == 0) d_->outW = out.width, d_->outH = out.height;
  if (info) {
    info->convertMs = conv;
    info->renderMs = msSince(t1);
  }
  return true;
}

bool VideoFrameSession::render(const InputFrame& f, std::uint32_t frameIndex, PackFormat fmt, std::vector<std::uint8_t>& out, FrameInfo* info,
                               std::string* err) {
  if ((fmt == PackFormat::Rgba8 || fmt == PackFormat::I420) && d_->cfg.allowGpu) {
    const auto t0 = Clock::now();
    SourceImage src;
    if (!toSource(f, d_->cfg, src, err)) return false;
    const double conv = msSince(t0);
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->ready) {
      d_->freezeFrom(src);
      ImageF scratch;
      if (!d_->run(src, 0, true, &scratch, nullptr, nullptr, err)) return false;
    } else if (src.linear.width != d_->refW || src.linear.height != d_->refH) {
      if (err) *err = "the frame size changed inside the clip";
      return false;
    }
    const auto t1 = Clock::now();
    FrameInfo fi;
    std::vector<std::uint8_t> rgba8;
    if (!d_->run(src, frameIndex, false, nullptr, &rgba8, &fi, err)) return false;
    fi.convertMs = conv;
    fi.renderMs = msSince(t1);
    if (fmt == PackFormat::Rgba8) {
      out = std::move(rgba8);
    } else {  // 8-bit graph output (GPU executor): the I420 pack of the same values
      const auto t2 = Clock::now();
      ImageF img(fi.width, fi.height);
      for (std::size_t i = 0, n = std::size_t(fi.width) * fi.height; i < n; ++i)
        for (int c = 0; c < 3; ++c) img.data[3 * i + std::size_t(c)] = float(rgba8[4 * i + std::size_t(c)]) * (1.f / 255.f);
      int pw = 0, ph = 0;
      pack(img, fmt, out, &pw, &ph);
      fi.width = pw;
      fi.height = ph;
      fi.packMs = msSince(t2);
    }
    if (info) *info = fi;
    return true;
  }
  ImageF img;
  SourceImage src;
  FrameInfo fi;
  if (!renderFloatSrc(f, frameIndex, img, src, &fi, err)) return false;
  const auto t = Clock::now();
  int w = 0, h = 0;
  if ((fmt == PackFormat::P010Hdr || fmt == PackFormat::Rgb10A2Hdr) && d_->outKind != OutputKind::Sdr709) {
    ImageF hdr;
    if (toHdrEncoded(img, src, d_->held.exposure, d_->outKind, hdr)) {
      pack(hdr, fmt, out, &w, &h);
      fi.packMs = msSince(t);
      fi.width = w;
      fi.height = h;
      if (info) *info = fi;
      return true;
    }
    if (err) *err = "HDR output needs the edit's geometry to be the identity";
    return false;
  }
  if (fmt == PackFormat::P010Hdr) fmt = PackFormat::P010;
  if (fmt == PackFormat::Rgb10A2Hdr) fmt = PackFormat::Rgb10A2;
  pack(img, fmt, out, &w, &h);
  fi.packMs = msSince(t);
  fi.width = w;
  fi.height = h;
  if (info) *info = fi;
  return true;
}

}  // namespace dapple::engine::video
