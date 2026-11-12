#include "engine/nodes/demosaic/demosaic_session.h"

#include <algorithm>
#include <cstring>

#include "engine/nodes/demosaic/demosaic_desc.h"

namespace dapple::demosaic {

namespace {
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

ImageF demosaicOrientedRegion(const MosaicImage& m, int x0, int y0, int x1, int y1, int threads) {
  Rect r;
  if (!mosaicRectOf(m, x0, y0, x1, y1, &r)) return {};
  const Desc d = makeDesc(m);
  ImageF tmp(r.w(), r.h());
  demosaic(d, r, tmp.data.data(), std::size_t(r.w()) * 3, threads);
  return rotateFlipImg(tmp, m.flip);
}

ImageF demosaicFull(const MosaicImage& m, int threads) {
  const Desc d = makeDesc(m);
  ImageF rgb(m.width, m.height);
  demosaic(d, {0, 0, m.width, m.height}, rgb.data.data(), std::size_t(m.width) * 3, threads);
  return finishMosaicRgb(m, std::move(rgb));
}

ImageF binnedHalf(const MosaicImage& m) {
  const Desc d = makeDesc(m);
  const int P = m.period;
  const int bs = m.cfa == CfaKind::XTrans ? 3 : 2;
  const int ow = m.width / bs, oh = m.height / bs;
  ImageF out(ow, oh);
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) {
      float s[3] = {0, 0, 0}, n[3] = {0, 0, 0};
      for (int v = 0; v < bs; ++v)
        for (int u = 0; u < bs; ++u) {
          const int sx = x * bs + u, sy = y * bs + v;
          const int c = d.cfa3[(sy % P) * P + (sx % P)];
          s[c] += scaleSample(d, sx, sy), n[c] += 1.f;
        }
      float rgb[3];
      for (int c = 0; c < 3; ++c) rgb[c] = n[c] > 0 ? s[c] / n[c] : 0.f;
      float* px = out.row(y) + 3 * x;
      for (int r = 0; r < 3; ++r) px[r] = std::clamp(d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2], 0.f, 1.f);
    }
  return out;
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

void MosaicCache::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  lru_.clear();
  bytes_ = 0;
}

}  // namespace dapple::demosaic
