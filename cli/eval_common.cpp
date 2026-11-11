#include "eval_common.h"

#include <cmath>
#include <cstring>

namespace dapple::evaltool {

ImageF resizeExact(const ImageF& src, int dw, int dh) {
  if (src.width == dw && src.height == dh) return src;
  ImageF dst(dw, dh);
  const double sx = double(src.width) / dw, sy = double(src.height) / dh;
  std::vector<std::thread> pool;
  std::atomic<int> next{0};
  auto work = [&] {
    for (int y; (y = next.fetch_add(1)) < dh;) {
      const double y0 = y * sy, y1 = (y + 1) * sy;
      const int iy0 = int(y0), iy1 = std::min(src.height, int(std::ceil(y1)));
      float* out = dst.row(y);
      for (int x = 0; x < dw; ++x) {
        const double x0 = x * sx, x1 = (x + 1) * sx;
        const int ix0 = int(x0), ix1 = std::min(src.width, int(std::ceil(x1)));
        double acc[3] = {0, 0, 0}, wsum = 0;
        for (int yy = iy0; yy < iy1; ++yy) {
          const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
          const float* in = src.row(yy);
          for (int xx = ix0; xx < ix1; ++xx) {
            const double w = wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
            acc[0] += w * in[3 * xx];
            acc[1] += w * in[3 * xx + 1];
            acc[2] += w * in[3 * xx + 2];
            wsum += w;
          }
        }
        for (int c = 0; c < 3; ++c) out[3 * x + c] = float(acc[c] / wsum);
      }
    }
  };
  const unsigned n = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned i = 1; i < n; ++i) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  return dst;
}

namespace {

std::vector<float> highpassLuma(const ImageF& im, int r) {
  const int w = im.width, h = im.height;
  std::vector<float> l(std::size_t(w) * h), t(l.size()), o(l.size());
  for (std::size_t i = 0; i < l.size(); ++i)
    l[i] = 0.2126f * im.data[3 * i] + 0.7152f * im.data[3 * i + 1] + 0.0722f * im.data[3 * i + 2];
  // separable box blur
  auto blur = [&](const std::vector<float>& src, std::vector<float>& dst, bool horiz) {
    const int n = horiz ? w : h, m = horiz ? h : w;
    for (int j = 0; j < m; ++j) {
      auto at = [&](int i) { return src[horiz ? std::size_t(j) * w + i : std::size_t(i) * w + j]; };
      double acc = 0;
      int cnt = 0;
      for (int i = 0; i < std::min(n, r); ++i) {
        acc += at(i);
        ++cnt;
      }
      for (int i = 0; i < n; ++i) {
        if (i + r < n) {
          acc += at(i + r);
          ++cnt;
        }
        if (i - r - 1 >= 0) {
          acc -= at(i - r - 1);
          --cnt;
        }
        dst[horiz ? std::size_t(j) * w + i : std::size_t(i) * w + j] = float(acc / cnt);
      }
    }
  };
  blur(l, t, true);
  blur(t, o, false);
  for (std::size_t i = 0; i < l.size(); ++i) l[i] -= o[i];
  return l;
}

struct Pose {
  double s = 1, k = 0, tx = 0, ty = 0;
};

// NCC of A(p) with B(q(p)) over a sparse grid of p; -1 when too few samples fall inside B.
double nccPose(const std::vector<float>& A, const std::vector<float>& B, int w, int h, const Pose& P) {
  const double cx = (w - 1) * 0.5, cy = (h - 1) * 0.5, R2 = cx * cx + cy * cy;
  double sab = 0, saa = 0, sbb = 0;
  long n = 0, tot = 0;
  for (int y = 4; y < h - 4; y += 2)
    for (int x = 4; x < w - 4; x += 2) {
      ++tot;
      const double dx = x - cx, dy = y - cy;
      const double g = (1 + P.k * (dx * dx + dy * dy) / R2) / P.s;
      const double qx = cx + dx * g + P.tx, qy = cy + dy * g + P.ty;
      if (qx < 0 || qy < 0 || qx > w - 1.001 || qy > h - 1.001) continue;
      const int ix = int(qx), iy = int(qy);
      const double fx = qx - ix, fy = qy - iy;
      const float* b = &B[std::size_t(iy) * w + ix];
      const double vb = (b[0] * (1 - fx) + b[1] * fx) * (1 - fy) + (b[w] * (1 - fx) + b[w + 1] * fx) * fy;
      const double va = A[std::size_t(y) * w + x];
      sab += va * vb;
      saa += va * va;
      sbb += vb * vb;
      ++n;
    }
  if (n < tot * 6 / 10) return -1;
  return sab / std::sqrt(std::max(1e-18, saa * sbb));
}

}  // namespace

Alignment alignCamera(const ImageF& ours, const ImageF& cam) {
  // Work at half resolution (fast, and the sharpening / noise differences matter less).
  const int w = std::max(8, ours.width / 2), h = std::max(8, ours.height / 2);
  const std::vector<float> A = highpassLuma(resizeExact(ours, w, h), 5);
  const std::vector<float> B = highpassLuma(resizeExact(cam, w, h), 5);
  Alignment out;
  out.ncc0 = float(nccPose(A, B, w, h, Pose{}));
  Pose best;
  double bestN = out.ncc0;
  for (double s = 0.94; s <= 1.0601; s += 0.004)
    for (double ty = -8; ty <= 8; ty += 2)
      for (double tx = -8; tx <= 8; tx += 2) {
        Pose p;
        p.s = s;
        p.tx = tx;
        p.ty = ty;
        const double n = nccPose(A, B, w, h, p);
        if (n > bestN) {
          bestN = n;
          best = p;
        }
      }
  // Coordinate descent with shrinking steps.
  double steps[4] = {0.002, 0.02, 1.0, 1.0};
  for (int round = 0; round < 6; ++round) {
    bool moved = true;
    int guard = 0;
    while (moved && guard++ < 20) {
      moved = false;
      for (int d = 0; d < 4; ++d)
        for (int sign = -1; sign <= 1; sign += 2) {
          Pose p = best;
          double* f = d == 0 ? &p.s : d == 1 ? &p.k : d == 2 ? &p.tx : &p.ty;
          *f += sign * steps[d];
          const double n = nccPose(A, B, w, h, p);
          if (n > bestN + 1e-6) {
            bestN = n;
            best = p;
            moved = true;
          }
        }
    }
    for (double& s : steps) s *= 0.5;
  }
  const double up = double(ours.width) / w;
  out.scale = float(best.s);
  out.k = float(best.k);
  out.tx = float(best.tx * up);
  out.ty = float(best.ty * up);
  out.ncc = float(bestN);

  // Margin: smallest border such that the whole inner rectangle maps inside the camera image.
  const int W = ours.width, H = ours.height;
  const double cx = (W - 1) * 0.5, cy = (H - 1) * 0.5, R2 = cx * cx + cy * cy;
  auto inside = [&](int x, int y) {
    const double dx = x - cx, dy = y - cy;
    const double g = (1 + out.k * (dx * dx + dy * dy) / R2) / out.scale;
    const double qx = cx + dx * g + out.tx, qy = cy + dy * g + out.ty;
    return qx >= 1 && qy >= 1 && qx <= W - 2 && qy <= H - 2;
  };
  int m = 0;
  for (; m < std::min(W, H) / 4; ++m) {
    bool ok = true;
    for (int x = m; x < W - m && ok; x += 4) ok = inside(x, m) && inside(x, H - 1 - m);
    for (int y = m; y < H - m && ok; y += 4) ok = inside(m, y) && inside(W - 1 - m, y);
    if (ok) break;
  }
  out.margin = m + 2;
  return out;
}

ImageF warpCamera(const ImageF& cam, const Alignment& a) {
  ImageF out(cam.width, cam.height);
  const int W = cam.width, H = cam.height;
  const double cx = (W - 1) * 0.5, cy = (H - 1) * 0.5, R2 = cx * cx + cy * cy;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const double dx = x - cx, dy = y - cy;
      const double g = (1 + a.k * (dx * dx + dy * dy) / R2) / a.scale;
      double qx = std::clamp(cx + dx * g + a.tx, 0.0, W - 1.001), qy = std::clamp(cy + dy * g + a.ty, 0.0, H - 1.001);
      const int ix = int(qx), iy = int(qy);
      const double fx = qx - ix, fy = qy - iy;
      for (int c = 0; c < 3; ++c) {
        const float* p = &cam.data[(std::size_t(iy) * W + ix) * 3 + c];
        const double v = (p[0] * (1 - fx) + p[3] * fx) * (1 - fy) + (p[3 * W] * (1 - fx) + p[3 * W + 3] * fx) * fy;
        out.data[(std::size_t(y) * W + x) * 3 + c] = float(v);
      }
    }
  return out;
}

namespace {

constexpr std::uint32_t kMagic = 0x44504332;  // "DPC2"

std::string cacheKey(const fs::path& raw, int edge) {
  std::error_code ec;
  const auto size = fs::file_size(raw, ec);
  const std::size_t h = std::hash<std::string>{}(u8(raw));
  char buf[96];
  std::snprintf(buf, sizeof buf, "_%zx_%llu_%d.bin", h, static_cast<unsigned long long>(size), edge);
  return u8(raw.stem()) + buf;
}

void writeStr(std::ofstream& f, const std::string& s) {
  const std::uint32_t n = std::uint32_t(s.size());
  f.write(reinterpret_cast<const char*>(&n), 4);
  f.write(s.data(), n);
}
bool readStr(std::ifstream& f, std::string& s) {
  std::uint32_t n = 0;
  if (!f.read(reinterpret_cast<char*>(&n), 4) || n > 4096) return false;
  s.resize(n);
  return bool(f.read(s.data(), n));
}

bool readCache(const fs::path& file, Case& c) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  std::uint32_t magic = 0;
  std::int32_t w = 0, h = 0;
  f.read(reinterpret_cast<char*>(&magic), 4);
  f.read(reinterpret_cast<char*>(&w), 4);
  f.read(reinterpret_cast<char*>(&h), 4);
  if (!f || magic != kMagic || w <= 0 || h <= 0) return false;
  if (!readStr(f, c.src.meta.make) || !readStr(f, c.src.meta.model)) return false;
  f.read(reinterpret_cast<char*>(&c.align), sizeof c.align);
  std::vector<std::uint16_t> lin(std::size_t(w) * h * 3);
  std::vector<std::uint8_t> cam(lin.size());
  f.read(reinterpret_cast<char*>(lin.data()), std::streamsize(lin.size() * 2));
  f.read(reinterpret_cast<char*>(cam.data()), std::streamsize(cam.size()));
  if (!f) return false;
  c.src.linear = ImageF(w, h);
  c.cam = ImageF(w, h);
  for (std::size_t i = 0; i < lin.size(); ++i) {
    c.src.linear.data[i] = lin[i] / 65535.f;
    c.cam.data[i] = cam[i] / 255.f;
  }
  c.src.sceneReferred = true;
  return true;
}

void writeCache(const fs::path& file, const Case& c) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    const std::uint32_t magic = kMagic;
    const std::int32_t w = c.src.linear.width, h = c.src.linear.height;
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&w), 4);
    f.write(reinterpret_cast<const char*>(&h), 4);
    writeStr(f, c.src.meta.make);
    writeStr(f, c.src.meta.model);
    f.write(reinterpret_cast<const char*>(&c.align), sizeof c.align);
    std::vector<std::uint16_t> lin(c.src.linear.data.size());
    std::vector<std::uint8_t> cam(lin.size());
    for (std::size_t i = 0; i < lin.size(); ++i) {
      lin[i] = std::uint16_t(std::clamp(c.src.linear.data[i], 0.f, 1.f) * 65535.f + 0.5f);
      cam[i] = std::uint8_t(std::clamp(c.cam.data[i], 0.f, 1.f) * 255.f + 0.5f);
    }
    f.write(reinterpret_cast<const char*>(lin.data()), std::streamsize(lin.size() * 2));
    f.write(reinterpret_cast<const char*>(cam.data()), std::streamsize(cam.size()));
  }
  fs::rename(tmp, file, ec);
}

}  // namespace

bool loadCase(const Developer& dev, const fs::path& raw, int edge, const fs::path& cacheDir, Case& out,
              std::string& err) {
  out.name = u8(raw.filename());
  const fs::path cf = cacheDir.empty() ? fs::path() : cacheDir / fromU8(cacheKey(raw, edge));
  if (!cf.empty() && readCache(cf, out)) return true;

  const ImageDecoder* decoder = dev.decoders().find(raw);
  if (!decoder) {
    err = "unsupported file";
    return false;
  }
  auto src = decoder->decode(raw, DecodeOptions{DecodeQuality::Full});
  if (!src) {
    err = src.error().message;
    return false;
  }
  auto prev = extractEmbeddedPreview(raw, 0);
  if (!prev) {
    err = prev.error().message;
    return false;
  }
  out.src = std::move(*src);
  out.src.linear = resizeToLongEdge(out.src.linear, edge);
  ImageF cam(prev->width, prev->height);
  for (std::size_t i = 0; i < cam.data.size(); ++i) cam.data[i] = prev->data[i] / 255.f;
  // Same framing check: the preview may differ from the RAW by a pixel row (rounding of the
  // 3:2 crop); map both onto one grid.
  cam = resizeExact(cam, out.src.linear.width, out.src.linear.height);
  // Rough render of the RAW (camera baseline gain, sRGB curve) only to register the two images.
  ImageF approx(cam.width, cam.height);
  for (std::size_t i = 0; i < approx.data.size() / 3; ++i) {
    const float* p = &out.src.linear.data[3 * i];
    const float y = std::clamp((0.2880402f * p[0] + 0.7118741f * p[1]) * 2.64f, 0.f, 1.f);
    const float e = y <= 0.0031308f ? 12.92f * y : 1.055f * std::pow(y, 1.f / 2.4f) - 0.055f;
    approx.data[3 * i] = approx.data[3 * i + 1] = approx.data[3 * i + 2] = e;
  }
  out.align = alignCamera(approx, cam);
  out.cam = warpCamera(cam, out.align);
  if (!cf.empty()) writeCache(cf, out);
  return true;
}

std::vector<Case> loadCases(const Developer& dev, const std::vector<fs::path>& files, int edge,
                            const fs::path& cacheDir, int workers) {
  std::vector<Case> cases(files.size());
  std::vector<char> ok(files.size(), 0);
  std::atomic<std::size_t> next{0};
  auto work = [&] {
    for (std::size_t i; (i = next.fetch_add(1)) < files.size();) {
      std::string err;
      ok[i] = loadCase(dev, files[i], edge, cacheDir, cases[i], err);
      if (!ok[i]) std::fprintf(stderr, "skip %s: %s\n", u8(files[i]).c_str(), err.c_str());
      if ((i + 1) % 20 == 0) {
        std::fprintf(stderr, "  loaded ~%zu/%zu\n", i + 1, files.size());
      }
    }
  };
  std::vector<std::thread> pool;
  for (int i = 1; i < workers; ++i) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  std::vector<Case> out;
  for (std::size_t i = 0; i < cases.size(); ++i)
    if (ok[i]) out.push_back(std::move(cases[i]));
  return out;
}

}  // namespace dapple::evaltool
