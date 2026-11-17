#include "film_skin.h"

#include <algorithm>
#include <mutex>

#include "local_eval.h"

namespace dapple::film {

namespace {
struct Entry {
  std::string prefix;
  std::uint64_t key = 0;
  std::shared_ptr<const SkinMask> mask;
};
std::mutex g_m;
std::vector<Entry> g_cache;  // newest last, at most 4

// three box passes of radius r (a close match of a gaussian), edge replicated
void boxBlur3(std::vector<float>& a, int w, int h, int r) {
  std::vector<float> t(a.size());
  // n samples along an axis with `stride` between them, `count` lines `step` apart
  auto pass = [&](const float* in, float* out, int n, int stride, int count, int step) {
    for (int k = 0; k < count; ++k) {
      const float* s = in + std::size_t(k) * step;
      float* d = out + std::size_t(k) * step;
      double acc = 0;
      for (int i = -r; i <= r; ++i) acc += s[std::size_t(std::clamp(i, 0, n - 1)) * stride];
      for (int i = 0; i < n; ++i) {
        d[std::size_t(i) * stride] = float(acc / double(2 * r + 1));
        acc += s[std::size_t(std::min(i + r + 1, n - 1)) * stride] - s[std::size_t(std::max(i - r, 0)) * stride];
      }
    }
  };
  for (int it = 0; it < 3; ++it) {
    pass(a.data(), t.data(), w, 1, h, w);  // rows
    pass(t.data(), a.data(), h, w, w, 1);  // columns
  }
}
}  // namespace

std::uint64_t skinKeyFor(const std::string& prefix) {
  if (prefix.empty()) return 0;
  const std::uint64_t a = local::rasterContentHash(skinFaceId(prefix)), b = local::rasterContentHash(skinBodyId(prefix));
  if (!a && !b) return 0;
  return (a * 0x9E3779B97F4A7C15ull) ^ ((b + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full) ^ 0x5eed1ull;
}

std::shared_ptr<const SkinMask> skinMaskFor(const std::string& prefix) {
  if (prefix.empty()) return nullptr;
  const std::shared_ptr<const local::Raster> fa = local::rasterGet(skinFaceId(prefix)), bo = local::rasterGet(skinBodyId(prefix));
  if (!fa && !bo) return nullptr;
  const std::uint64_t key = skinKeyFor(prefix);
  {
    std::lock_guard<std::mutex> lk(g_m);
    for (const Entry& e : g_cache)
      if (e.prefix == prefix && e.key == key) return e.mask;
  }
  auto m = std::make_shared<SkinMask>();
  m->key = key;
  const std::shared_ptr<const local::Raster> big = (fa && bo) ? (std::max(fa->w, fa->h) >= std::max(bo->w, bo->h) ? fa : bo) : (fa ? fa : bo);
  m->w = big->w, m->h = big->h;
  if (m->w <= 0 || m->h <= 0) return nullptr;
  m->a.assign(std::size_t(m->w) * m->h, 0.f);
  for (const std::shared_ptr<const local::Raster>& r : {fa, bo}) {
    if (!r || r->w <= 0 || r->h <= 0) continue;
    for (int y = 0; y < m->h; ++y)
      for (int x = 0; x < m->w; ++x) {
        // the two planes are produced at the same size; the nearest sample is a safety net for any other
        const int sx = std::min(r->w - 1, x * r->w / m->w), sy = std::min(r->h - 1, y * r->h / m->h);
        float& d = m->a[std::size_t(y) * m->w + x];
        d = std::max(d, float(r->px[std::size_t(sy) * r->w + sx]) * (1.f / 255.f));
      }
  }
  boxBlur3(m->a, m->w, m->h, std::max(2, int(std::lround(0.012 * std::max(m->w, m->h)))));
  std::shared_ptr<const SkinMask> out = std::move(m);
  std::lock_guard<std::mutex> lk(g_m);
  g_cache.push_back({prefix, key, out});
  if (g_cache.size() > 4) g_cache.erase(g_cache.begin());
  return out;
}

}  // namespace dapple::film
