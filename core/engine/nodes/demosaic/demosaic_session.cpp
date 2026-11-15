#include "engine/nodes/demosaic/demosaic_session.h"

#if defined(__ANDROID__)
#include <malloc.h>
#endif

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "engine/nodes/demosaic/demosaic_backend.h"
#include "parallel.h"
#include "engine/nodes/demosaic/demosaic_desc.h"

namespace dapple::demosaic {

namespace {
GpuBackend* g_backend = nullptr;

// The GPU on `roi` (the GPU is chosen for it), shared with the CPU node: on a phone the GPU node is only 1.5 - 3x the speed of the CPU node (X-Trans), and
// the cores would otherwise wait for it. The rows are dealt by their absolute mosaic row: of every kShareRows rows the last `share` part goes to the CPU
// node, the rest to the GPU, both running at the same time (share = the backend's measured split, so that both finish together). A whole frame, a band of
// the streamed export and a region therefore give every row to the same node: the same pixels whichever way the picture is cut. Both nodes run the same
// arithmetic (the GPU's output rounds to fp16: the parity budget of 32.6). False when a GPU part failed (the caller runs the CPU node over everything).
constexpr int kShareRows = 1024;
bool runGpuShared(GpuBackend* b, const Desc& d, Rect roi, float* out, std::size_t stride, int threads) {
  std::string why;
  const int cpuLen = int(b->cpuShare(d) * (kShareRows / 32) + 0.5) * 32;  // steps of 32 rows: a probe a few % off moves no row (the app caches the probe per device)
  if (cpuLen < 32) return b->run(d, roi, out, stride, why);
  std::vector<Rect> gpuParts, cpuParts;
  for (int p0 = roi.y0 / kShareRows * kShareRows; p0 < roi.y1; p0 += kShareRows) {
    const int split = p0 + kShareRows - cpuLen;
    const int ga = std::max(roi.y0, p0), gb = std::min(roi.y1, split), ca = std::max(roi.y0, split), cb = std::min(roi.y1, p0 + kShareRows);
    if (gb > ga) gpuParts.push_back({roi.x0, ga, roi.x1, gb});
    if (cb > ca) cpuParts.push_back({roi.x0, ca, roi.x1, cb});
  }
  auto at = [&](const Rect& r) { return out + std::size_t(r.y0 - roi.y0) * stride; };
  const int hw = int(std::max(2u, std::thread::hardware_concurrency()));
  // two cores stay with the GPU runner (submission, read-back conversion); at most 4 threads: the CPU part is the smaller one and finishes first anyway,
  // and each X-Trans thread holds ~8 MB of scratch (Redmi, X-S10 export: 6 threads 2319 ms and +12 MB peak over the GPU alone, 4 threads 2258 ms)
  const int cpuThreads = std::max(1, std::min(4, (threads > 0 ? std::min(threads, hw) : hw) - (gpuParts.empty() ? 0 : 2)));
  std::thread cpu([&] {
    for (const Rect& r : cpuParts) demosaic(d, r, at(r), stride, cpuThreads);
  });
  bool ok = true;
  for (const Rect& r : gpuParts) ok = ok && b->run(d, r, at(r), stride, why);
  cpu.join();
#if defined(__ANDROID__) && defined(M_PURGE)
  if (!cpuParts.empty()) mallopt(M_PURGE, 0);  // the CPU part's per-thread scratch stays in the allocator's thread caches otherwise (the export's peak)
#endif
  return ok;
}

// GPU when a backend is installed and usable, else the CPU node. `usedGpu` (optional) says which one ran.
void runNode(const Desc& d, Rect roi, float* out, std::size_t stride, int threads, bool* usedGpu = nullptr) {
  if (usedGpu) *usedGpu = false;
  if (GpuBackend* b = g_backend) {
    std::string why;
    if (b->available(why) && b->prefer(d, roi, why) && runGpuShared(b, d, roi, out, stride, threads)) {
      if (usedGpu) *usedGpu = true;
      return;
    }
  }
  demosaic(d, roi, out, stride, threads);
}

ImageF rotateFlipImg(const ImageF& in, int flip) {
  if (flip != 3 && flip != 5 && flip != 6) return in;
  const int W = in.width, H = in.height;
  ImageF out = flip == 3 ? ImageF(W, H) : ImageF(H, W);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      int dx, dy;
      if (flip == 3) dx = W - 1 - x, dy = H - 1 - y;
      else if (flip == 6) dx = H - 1 - y, dy = x;
      else dx = y, dy = W - 1 - x;
      std::memcpy(out.row(dy) + std::size_t(dx) * 3, in.row(y) + std::size_t(x) * 3, 3 * sizeof(float));
    }
  return out;
}
}  // namespace

void setGpuBackend(GpuBackend* b) { g_backend = b; }
GpuBackend* gpuBackend() { return g_backend; }

void orientedSize(const MosaicImage& m, int* w, int* h) {
  const int cw = m.hasCrop ? m.cropW : m.width, ch = m.hasCrop ? m.cropH : m.height;
  const bool swap = (m.flip & 4) != 0;
  *w = swap ? ch : cw;
  *h = swap ? cw : ch;
}

bool mosaicRectOf(const MosaicImage& m, int x0, int y0, int x1, int y1, Rect* out) {
  if (!m.opcodeList3.empty()) return false;
  int W, H;
  orientedSize(m, &W, &H);
  x0 = std::clamp(x0, 0, W), x1 = std::clamp(x1, 0, W), y0 = std::clamp(y0, 0, H), y1 = std::clamp(y1, 0, H);
  if (x1 <= x0 || y1 <= y0) return false;
  const int cw = m.hasCrop ? m.cropW : m.width, ch = m.hasCrop ? m.cropH : m.height;
  Rect r;
  if (m.flip == 3) r = {cw - x1, ch - y1, cw - x0, ch - y0};
  else if (m.flip == 6) r = {y0, ch - x1, y1, ch - x0};  // oriented (dx, dy) = (ch - 1 - y, x)  =>  x = dy, y = ch - 1 - dx
  else if (m.flip == 5) r = {cw - y1, x0, cw - y0, x1};  // oriented (dx, dy) = (y, cw - 1 - x)  =>  y = dx, x = cw - 1 - dy
  else r = {x0, y0, x1, y1};
  if (m.flip == 6) r = {y0, ch - x1, y1, ch - x0};
  if (m.flip == 5) r = {cw - y1, x0, cw - y0, x1};
  const int ox = m.hasCrop ? m.cropX : 0, oy = m.hasCrop ? m.cropY : 0;
  r.x0 += ox, r.x1 += ox, r.y0 += oy, r.y1 += oy;
  *out = r;
  return true;
}

ImageF demosaicOrientedRegion(const MosaicImage& m, int x0, int y0, int x1, int y1, int threads, const std::atomic<std::uint64_t>* cancelCur, std::uint64_t cancelMine) {
  Rect r;
  if (!mosaicRectOf(m, x0, y0, x1, y1, &r)) return {};
  Desc d = makeDesc(m);
  d.cancelCur = cancelCur, d.cancelMine = cancelMine;
  ImageF tmp(r.w(), r.h());
  runNode(d, r, tmp.data.data(), std::size_t(r.w()) * 3, threads);
  if (cancelCur && cancelCur->load() != cancelMine) return {};  // cancelled: the pixels are partial
  return rotateFlipImg(tmp, m.flip);
}

bool fullUsesGpu(const MosaicImage& m) {
  GpuBackend* b = g_backend;
  if (!b) return false;
  const Desc d = makeDesc(m);
  std::string why;
  return b->available(why) && b->prefer(d, {0, 0, m.width, m.height}, why);
}

ImageF demosaicOrientedRegionOn(const MosaicImage& m, int x0, int y0, int x1, int y1, bool gpu, bool* fellBack, int threads) {
  Rect r;
  if (!mosaicRectOf(m, x0, y0, x1, y1, &r)) return {};
  const Desc d = makeDesc(m);
  ImageF tmp(r.w(), r.h());
  bool done = false;
  if (gpu) {
    if (GpuBackend* b = g_backend) done = runGpuShared(b, d, r, tmp.data.data(), std::size_t(r.w()) * 3, threads);
    if (!done && fellBack) *fellBack = true;
  }
  if (!done) demosaic(d, r, tmp.data.data(), std::size_t(r.w()) * 3, threads);
  if (m.flip != 3 && m.flip != 5 && m.flip != 6) return tmp;  // upright: no copy (a band of a 60 MP picture is ~70 MB)
  return rotateFlipImg(tmp, m.flip);
}

bool demosaicOrientedRegionInto(const MosaicImage& m, int x0, int y0, int x1, int y1, bool gpu, float* out, std::size_t strideFloats, bool* fellBack, int threads) {
  if (m.flip == 3 || m.flip == 5 || m.flip == 6) {  // turned pictures: the oriented rows are not mosaic rows; demosaic, orient, copy
    const ImageF r = demosaicOrientedRegionOn(m, x0, y0, x1, y1, gpu, fellBack, threads);
    if (r.width != x1 - x0 || r.height != y1 - y0) return false;
    for (int y = 0; y < r.height; ++y) std::memcpy(out + std::size_t(y) * strideFloats, r.row(y), std::size_t(r.width) * 3 * sizeof(float));
    return true;
  }
  Rect r;
  if (!mosaicRectOf(m, x0, y0, x1, y1, &r) || r.w() != x1 - x0 || r.h() != y1 - y0) return false;
  const Desc d = makeDesc(m);
  bool done = false;
  if (gpu) {
    if (GpuBackend* b = g_backend) done = runGpuShared(b, d, r, out, strideFloats, threads);
    if (!done && fellBack) *fellBack = true;
  }
  if (!done) demosaic(d, r, out, strideFloats, threads);
  return true;
}

ImageF demosaicFull(const MosaicImage& m, int threads, std::string* corrections) {
  const Desc d = makeDesc(m);
  ImageF rgb(m.width, m.height);
  runNode(d, {0, 0, m.width, m.height}, rgb.data.data(), std::size_t(m.width) * 3, threads);
  return finishMosaicRgb(m, std::move(rgb), corrections);
}

ImageF binnedHalf(const MosaicImage& m, int step) {
  const Desc d = makeDesc(m);
  const int P = m.period;
  const int bs = m.cfa == CfaKind::XTrans ? 3 : 2;
  step = std::max(step, 1);
  const int ow = (m.width / bs + step - 1) / step, oh = (m.height / bs + step - 1) / step;
  ImageF out(ow, oh);
  parallelRows(oh, [&](int yo) {  // rows are independent: 40 MP took 3 s on one core of a 2019 phone
    const int y = yo * step;
    int rowPh[3], rowBl[3];  // per row of the block: CFA row, black-table row
    for (int v = 0; v < bs; ++v) rowPh[v] = ((y * bs + v) % P) * P, rowBl[v] = ((y * bs + v) % d.blackRows) * d.blackCols;
    int px0 = 0, bx0 = 0;  // x * bs modulo the CFA period / the black table width, kept incrementally (no division per sample)
    for (int xo = 0; xo < ow; ++xo, px0 = (px0 + bs * step) % P, bx0 = (bx0 + bs * step) % d.blackCols) {
      const int x = xo * step;
      float s[3] = {0, 0, 0}, n[3] = {0, 0, 0};
      for (int v = 0; v < bs; ++v) {
        const std::uint16_t* row = d.data + std::size_t(y * bs + v) * d.width + std::size_t(x) * bs;
        for (int u = 0; u < bs; ++u) {  // identical arithmetic and order to scaleSample()
          const int ph = rowPh[v] + (px0 + u >= P ? px0 + u - P : px0 + u);
          const float blk = d.black[rowBl[v] + (bx0 + u) % d.blackCols];
          const float val = std::min(std::max(float(row[u]) - blk, 0.f) * d.wb[d.cfa4[ph]] * d.invSpan, 1.f);
          const int c = d.cfa3[ph];
          s[c] += val, n[c] += 1.f;
        }
      }
      float rgb[3];
      for (int c = 0; c < 3; ++c) rgb[c] = n[c] > 0 ? s[c] / n[c] : 0.f;
      float* px = out.row(yo) + 3 * xo;
      for (int r = 0; r < 3; ++r) px[r] = std::clamp(d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2], 0.f, 1.f);
    }
  });
  return out;
}

ImageF halfOriented(const MosaicImage& m, int threads, bool* usedGpu) {
  if (m.cfa != CfaKind::XTrans) return {};
  Desc d = makeDesc(m);
  d.half = true;
  // output pixel (x, y) = sensor pixels 2x..2x+1: the crop (DNG default crop, active-area pixels) maps to half-resolution pixels like binnedOriented's
  const int cx = m.hasCrop ? m.cropX / 2 : 0, cy = m.hasCrop ? m.cropY / 2 : 0;
  const int cw = std::min(m.hasCrop ? m.cropW / 2 : m.width / 2, m.width / 2 - cx), ch = std::min(m.hasCrop ? m.cropH / 2 : m.height / 2, m.height / 2 - cy);
  if (cw <= 0 || ch <= 0) return {};
  ImageF tmp(cw, ch);
  runNode(d, {cx, cy, cx + cw, cy + ch}, tmp.data.data(), std::size_t(cw) * 3, threads, usedGpu);
  return rotateFlipImg(tmp, m.flip);
}

namespace {
// binnedOriented(m, step)'s crop and orientation in one parallel pass, no intermediate copies. `h` is binnedHalf(m, step / sub): with sub > 1 the picture
// is its every sub-th pixel in x and y (a step-k binning is every k-th block of a finer one: the same values).
ImageF cropOrientBinned(const MosaicImage& m, const ImageF& h, int step, int sub) {
  const int bs = m.cfa == CfaKind::XTrans ? 3 : 2;
  const int sw = (h.width + sub - 1) / sub, sh = (h.height + sub - 1) / sub;  // binnedHalf(m, step)'s size
  int cx = 0, cy = 0, cw = sw, ch = sh;
  if (m.hasCrop) {
    const int ax = m.cropX / bs / step, ay = m.cropY / bs / step, aw = std::min(m.cropW / bs / step, sw - ax), ah = std::min(m.cropH / bs / step, sh - ay);
    if (aw > 0 && ah > 0) cx = ax, cy = ay, cw = aw, ch = ah;
  }
  const int flip = m.flip == 3 || m.flip == 5 || m.flip == 6 ? m.flip : 0;
  ImageF out = (flip == 5 || flip == 6) ? ImageF(ch, cw) : ImageF(cw, ch);
  parallelRows(ch, [&](int y) {
    const float* src = h.row((cy + y) * sub) + std::size_t(cx) * sub * 3;
    if (flip == 0) {
      float* o = out.row(y);
      if (sub == 1) std::memcpy(o, src, std::size_t(cw) * 3 * sizeof(float));
      else
        for (int x = 0; x < cw; ++x) std::memcpy(o + 3 * x, src + std::size_t(x) * sub * 3, 3 * sizeof(float));
      return;
    }
    for (int x = 0; x < cw; ++x) {  // as rotateFlipImg
      int dx, dy;
      if (flip == 3) dx = cw - 1 - x, dy = ch - 1 - y;
      else if (flip == 6) dx = ch - 1 - y, dy = x;
      else dx = y, dy = cw - 1 - x;
      std::memcpy(out.row(dy) + std::size_t(dx) * 3, src + std::size_t(x) * sub * 3, 3 * sizeof(float));
    }
  });
  return out;
}
}  // namespace

ImageF binnedOriented(const MosaicImage& m, int step) {
  step = std::max(step, 1);
  return cropOrientBinned(m, binnedHalf(m, step), step, 1);
}

void binnedOrientedPair(const MosaicImage& m, int anchorStep, ImageF* preview, ImageF* anchor) {
  anchorStep = std::max(anchorStep, 1);
  ImageF h = binnedHalf(m, 1);
  if (anchor) *anchor = cropOrientBinned(m, h, anchorStep, anchorStep);
  if (!preview) return;
  if (m.flip == 3 || m.flip == 5 || m.flip == 6) {  // a turned picture needs its own buffer
    *preview = cropOrientBinned(m, h, 1, 1);
    return;
  }
  // upright: the crop in place (rows move up and left, never onto rows not yet moved), no second picture-sized buffer
  const int bs = m.cfa == CfaKind::XTrans ? 3 : 2;
  if (m.hasCrop) {
    const int cx = m.cropX / bs, cy = m.cropY / bs, cw = std::min(m.cropW / bs, h.width - cx), ch = std::min(m.cropH / bs, h.height - cy);
    if (cw > 0 && ch > 0) {
      for (int y = 0; y < ch; ++y) std::memmove(h.data.data() + std::size_t(y) * cw * 3, h.row(cy + y) + std::size_t(cx) * 3, std::size_t(cw) * 3 * sizeof(float));
      h.width = cw, h.height = ch;
      h.data.resize(std::size_t(cw) * ch * 3);
    }
  }
  *preview = std::move(h);
}

// ---- cache ---------------------------------------------------------------------------------------------------------------------

MosaicCache::Entry* MosaicCache::find(std::uint64_t k0, std::uint64_t k1, int kind) {
  for (auto it = lru_.begin(); it != lru_.end(); ++it)
    if (it->k0 == k0 && it->k1 == k1 && it->kind == kind) {
      if (it != lru_.begin()) lru_.splice(lru_.begin(), lru_, it);
      return &lru_.front();
    }
  return nullptr;
}

void MosaicCache::insert(Entry e) {
  bytes_ += e.bytes;
  lru_.push_front(std::move(e));
  while (bytes_ > budget_ && lru_.size() > 1) {  // the newest entry always stays
    bytes_ -= lru_.back().bytes;
    lru_.pop_back();
    ++st_.evictions;
  }
}

Result<std::shared_ptr<const MosaicImage>> MosaicCache::mosaic(const std::filesystem::path& path, const DecodeOptions& opt, bool* hit) {
  std::uint64_t k[2];
  fileContentKey(path, k);
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (Entry* e = find(k[0], k[1], 0)) {
      ++st_.mosaicHits;
      if (hit) *hit = true;
      return e->mos;
    }
    ++st_.mosaicMisses;
  }
  if (hit) *hit = false;
  auto r = decodeRawMosaic(path, opt);
  if (!r) return fail(r.error().message);
  auto sp = std::make_shared<const MosaicImage>(std::move(*r));
  std::lock_guard<std::mutex> lk(mu_);
  insert({k[0], k[1], 0, sp->data.size() * 2 + 4096, sp, nullptr});
  return sp;
}

std::shared_ptr<const ImageF> MosaicCache::full(const std::shared_ptr<const MosaicImage>& m, int threads, bool* hit) {
  const std::uint64_t k0 = m->key[0] ^ (std::uint64_t(kAlgoVersion) * 0x9E3779B97F4A7C15ull), k1 = m->key[1];
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (Entry* e = find(k0, k1, 1)) {
      ++st_.fullHits;
      if (hit) *hit = true;
      return e->img;
    }
    ++st_.fullMisses;
  }
  if (hit) *hit = false;
  auto img = std::make_shared<const ImageF>(demosaicFull(*m, threads));
  std::lock_guard<std::mutex> lk(mu_);
  insert({k0, k1, 1, img->data.size() * sizeof(float), nullptr, img});
  return img;
}

CacheStats MosaicCache::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  CacheStats s = st_;
  s.bytes = bytes_;
  return s;
}

std::size_t MosaicCache::trimExcept(std::uint64_t k0, std::uint64_t k1) {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t freed = 0;
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->k0 == k0 && it->k1 == k1 && it->kind == 0) {
      ++it;
      continue;
    }
    freed += it->bytes;
    bytes_ -= it->bytes;
    it = lru_.erase(it);
  }
  return freed;
}
void MosaicCache::setBudget(std::size_t bytes) {
  std::lock_guard<std::mutex> lk(mu_);
  budget_ = bytes;
  while (bytes_ > budget_ && lru_.size() > 1) {  // the newest entry always stays (as insert())
    bytes_ -= lru_.back().bytes;
    lru_.pop_back();
    ++st_.evictions;
  }
}
std::size_t MosaicCache::budget() const {
  std::lock_guard<std::mutex> lk(mu_);
  return budget_;
}
std::size_t MosaicCache::trimTo(std::size_t keepBytes) {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t freed = 0;
  while (bytes_ > keepBytes && !lru_.empty()) {
    freed += lru_.back().bytes;
    bytes_ -= lru_.back().bytes;
    lru_.pop_back();
    ++st_.evictions;
  }
  return freed;
}
int MosaicCache::entries() const {
  std::lock_guard<std::mutex> lk(mu_);
  return int(lru_.size());
}
void MosaicCache::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  lru_.clear();
  bytes_ = 0;
}

}  // namespace dapple::demosaic
