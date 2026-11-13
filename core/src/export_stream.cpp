// Streaming (band) export: docs/engine/pipeline-architecture.md section 42.
#include "export_stream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dapple/renderer.h"
#include "engine/nodes/demosaic/demosaic_session.h"
#include "mem_probe.h"
#include "parallel.h"
#include "resize_rows.h"

namespace dapple::xstream {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
}  // namespace

Mode envMode() {
  const char* e = std::getenv("DAPPLE_EXPORT_STREAM");
  if (!e || !e[0] || std::strcmp(e, "auto") == 0) return Mode::Auto;
  if (std::strcmp(e, "on") == 0 || std::strcmp(e, "1") == 0) return Mode::On;
  if (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0) return Mode::Off;
  return Mode::Auto;
}

std::size_t budgetBytes() {
  if (const char* e = std::getenv("DAPPLE_EXPORT_BUDGET_MB"); e && e[0]) return std::size_t(std::max(0L, std::atol(e))) << 20;
  const std::size_t avail = mem::available();
  if (avail == 0) return std::size_t(384) << 20;  // unknown: assume a phone
#if defined(__APPLE__)
  // os_proc_available_memory is this process's own headroom before jetsam (iOS); macOS reports half the machine
  const double share = 0.6;
#else
  const double share = 0.5;  // MemAvailable / available physical memory is shared with every other process
#endif
  const std::size_t reserve = std::size_t(96) << 20;  // the app's own growth while the export runs (UI, decode of thumbnails)
  const std::size_t b = std::size_t(double(avail) * share);
  return b > reserve ? b - reserve : 0;
}

std::size_t predictWholePeak(int W, int H, bool film) {
  const double px = double(std::max(W, 0)) * double(std::max(H, 0));
  return std::size_t(px * (12.0 + 3.0 + 4.0 + (film ? 55.0 : 0.0)));  // calibrated on the desktop runs (6 threads): 61 MP std +1.05 GB, film +4.3 GB
}

bool openLazy(int W, int H, std::string* why) {
  const Mode m = envMode();
  char b[160];
  if (m == Mode::Off) {
    if (why) *why = "DAPPLE_EXPORT_STREAM=off";
    return false;
  }
  if (m == Mode::On) {
    if (why) *why = "DAPPLE_EXPORT_STREAM=on";
    return true;
  }
  const std::size_t need = predictWholePeak(W, H, false), budget = budgetBytes();
  std::snprintf(b, sizeof b, "whole-frame peak %.0f MB vs budget %.0f MB", double(need) / 1048576.0, double(budget) / 1048576.0);
  if (why) *why = b;
  return need > budget;
}

// ---- BandSource ----------------------------------------------------------------------------------------------------------------------------
BandSource::BandSource(std::shared_ptr<const MosaicImage> mosaic) : m_(std::move(mosaic)) {
  if (!m_) return;
  demosaic::orientedSize(*m_, &W_, &H_);
  gpu_ = demosaic::fullUsesGpu(*m_);
}

const ImageF* BandSource::window(int x0, int y0, int x1, int y1) {
  if (!m_ || x0 < 0 || y0 < 0 || x1 > W_ || y1 > H_ || x1 <= x0 || y1 <= y0) return nullptr;
  if (cancelCur_ && cancelCur_->load(std::memory_order_relaxed) != cancelMine_) return nullptr;
  const auto t0 = Clock::now();
  bool fb = false;
  ImageF().data.swap(win_.data);  // the previous window goes before the next one is made (never two bands alive)
  // the CPU node spawns its own threads (all cores by default): follow the pool's limit when one is set (dapple_perf_configure), its per-thread
  // scratch (X-Trans: ~8 MB a thread) is part of the export's peak
  const ThreadPool& pool = ThreadPool::instance();
  const int threads = pool.limit() < pool.size() ? std::max(1, pool.limit()) : 0;
  win_ = demosaic::demosaicOrientedRegionOn(*m_, x0, y0, x1, y1, gpu_, &fb, threads);
  fellBack_ |= fb;
  if (win_.width != x1 - x0 || win_.height != y1 - y0) return nullptr;
  if (shadow_) shadowChromaFloor(win_);
  ++windows_;
  ms_ += msSince(t0);
  peakBytes_ = std::max(peakBytes_, win_.data.size() * sizeof(float));
  return &win_;
}

std::uint64_t BandSourceStore::next() {
  static std::atomic<std::uint64_t> n{1ull << 40};
  return n.fetch_add(1);
}
const float* BandSourceStore::window(int x0, int y0, int w, int h, std::size_t& pitchFloats) {
  const ImageF* im = src_.window(x0, y0, x0 + w, y0 + h);
  if (!im) return nullptr;
  pitchFloats = std::size_t(w) * 3;
  return im->data.data();
}

SourceImage sizeOnly(const SourceImage& meta, int W, int H) {
  SourceImage s;
  s.meta = meta.meta;
  s.sceneReferred = meta.sceneReferred;
  s.linear.width = W;
  s.linear.height = H;
  return s;
}

bool renderBands(engine::EngineSession& es, const EditParams& params, const Renderer& renderer, const SourceImage& virt, BandSource& bs, int tile, bool hasProxy,
                 const std::function<void(int, int)>& onSize, const std::function<bool(int, int, const std::uint8_t*)>& rows, std::string* err) {
  engine::EngineSession::ExactOptions o;
  o.mode = engine::ExecMode::TiledExport;
  o.tile = std::max(64, tile / 4 * 4);
  o.hasProxy = hasProxy;
  bool srcFailed = false;
  o.sourceWindow = [&](int sx0, int sy0, int sx1, int sy1) -> const ImageF* {
    const ImageF* w = bs.window(sx0, sy0, sx1, sy1);
    srcFailed |= w == nullptr;
    return w;
  };
  int fw = 0, fh = 0;
  renderer.fastSize(virt, RenderRegion{}, fw, fh);  // the frame of the exact render (geometry applied)
  bool begun = false;
  o.rows = [&](int y0, int n, const std::uint8_t* rgb) {
    if (!begun) {
      begun = true;
      onSize(fw, fh);
    }
    return rows(y0, n, rgb);
  };
  Image8 sizeOut;
  std::string e;
  const bool ok = es.renderExact(params, renderer, virt, o, sizeOut, &e);
  if (!ok && err) *err = srcFailed && e == "cancelled" ? "streaming export: a source window could not be read" : e;
  return ok;
}

// ---- the analysis pass ---------------------------------------------------------------------------------------------------------------------
namespace {

// resizeToLongEdge fed with rows in order: output row y is computed as soon as its whole footprint is in the band buffer (same code, same order).
struct RowResizer {
  resize::Geom g;
  ImageF dst;
  int next = 0;
  int footprint() const { return int(std::ceil(g.sy)) + 2; }
  // rows [b0, b1) of the picture are in `buf` (row b0 at buf row 0); rows up to `upTo` are final (all rows of the picture below it are in)
  void feed(const ImageF& buf, int b0, int b1) {
    int first = next, last = next;
    while (last < g.dh) {
      int y0, y1;
      resize::rowSpan(g, last, y0, y1);
      if (y1 > b1) break;
      ++last;
    }
    if (last == first) return;
    parallelFor(last - first, [&](int k) {
      const int y = first + k;
      resize::row(g, y, [&](int yy) { return buf.row(yy - b0); }, dst.row(y));
    });
    next = last;
  }
};

}  // namespace

bool analyse(BandSource& src, const SourceImage& meta, const Wants& want, Analyses& out, const std::function<bool(float)>& progress, std::string* err) {
  const auto t0 = Clock::now();
  const int W = src.width(), H = src.height();
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (W < 2 || H < 2) return fail("empty picture");
  auto mkSrc = [&](ImageF&& im) {
    auto s = std::make_shared<SourceImage>();
    s->meta = meta.meta;
    s->sceneReferred = meta.sceneReferred;
    s->linear = std::move(im);
    return s;
  };
  for (int pass = 0; pass < 2; ++pass) {
    const bool decide = pass == 0;
    std::vector<RowResizer> rz;
    std::vector<int> edges;  // long edge of each resizer
    auto addRz = [&](int le) {
      for (int e : edges)
        if (e == le) return;
      RowResizer r;
      r.g = resize::geom(W, H, le);
      if (r.g.copy) return;  // the picture is not larger: it would be its own reduction (never streamed)
      r.dst = ImageF(r.g.dw, r.g.dh);
      rz.push_back(std::move(r));
      edges.push_back(le);
    };
    addRz(256);
    addRz(512);
    for (int le : want.longEdges) addRz(le);
    // modern end-point samples
    int gw = 0, gh = 0;
    Renderer::modernGridSize(W, H, gw, gh);
    ImageF grid(gw, gh);
    std::vector<int> gridRowY(static_cast<std::size_t>(gh)), gridColX(static_cast<std::size_t>(gw));
    for (int j = 0; j < gh; ++j) {
      int x, y;
      Renderer::modernGridPixel(W, H, 0, j, x, y);
      gridRowY[std::size_t(j)] = y;
    }
    for (int i = 0; i < gw; ++i) {
      int x, y;
      Renderer::modernGridPixel(W, H, i, 0, x, y);
      gridColX[std::size_t(i)] = x;
    }
    // raster guide
    std::shared_ptr<local::GuideCapture> gc;
    std::vector<std::vector<double>> gacc;
    std::vector<int> keyRowY(64), keyColX(64);
    if (want.guide || !want.guideGrids.empty()) {
      gc = std::make_shared<local::GuideCapture>();
      gc->W = W, gc->H = H;
      gc->samples.assign(64 * 64 * 3, 0.f);
      for (int k = 0; k < 64; ++k) {
        int x, y;
        local::guideSamplePos(W, H, k, k, x, y);
        keyRowY[std::size_t(k)] = y, keyColX[std::size_t(k)] = x;
      }
      for (const auto& s : want.guideGrids) {
        local::GuideCapture::Grid g;
        g.gw = s.first, g.gh = s.second;
        gc->grids.push_back(std::move(g));
        gacc.emplace_back(std::size_t(s.first) * s.second * 3, 0.0);
      }
    }
    std::vector<float> luma;
    if (decide) luma.reserve(std::size_t(W / 4 + 1) * std::size_t(H / 4 + 1));

    int K = 2;
    for (const RowResizer& r : rz) K = std::max(K, r.footprint());
    const int B = std::max(256, K);
    for (int y0 = 0; y0 < H; y0 += B) {
      const int y1 = std::min(H, y0 + B), b0 = std::max(0, y0 - K);
      const ImageF* buf = src.window(0, b0, W, y1);
      if (!buf) return fail("cancelled");
      // shadow-floor decision samples: every 4th row / column (before the floor)
      if (decide)
        for (int y = (y0 + 3) / 4 * 4; y < y1; y += 4) {
          const float* p = buf->row(y - b0);
          for (int x = 0; x < W; x += 4) luma.push_back(shadowChromaLuma(p + 3 * x));
        }
      for (RowResizer& r : rz) r.feed(*buf, b0, y1);
      for (int j = 0; j < gh; ++j) {
        const int y = gridRowY[std::size_t(j)];
        if (y < y0 || y >= y1) continue;
        const float* p = buf->row(y - b0);
        for (int i = 0; i < gw; ++i) std::memcpy(grid.row(j) + 3 * i, p + 3 * gridColX[std::size_t(i)], 3 * sizeof(float));
      }
      if (gc) {
        for (int j = 0; j < 64; ++j) {
          const int y = keyRowY[std::size_t(j)];
          if (y < y0 || y >= y1) continue;
          const float* p = buf->row(y - b0);
          for (int i = 0; i < 64; ++i) std::memcpy(gc->samples.data() + 3 * std::size_t(j * 64 + i), p + 3 * keyColX[std::size_t(i)], 3 * sizeof(float));
        }
        for (std::size_t gi = 0; gi < gc->grids.size(); ++gi) {
          const int ggw = gc->grids[gi].gw, ggh = gc->grids[gi].gh;
          std::vector<double>& acc = gacc[gi];
          parallelFor(ggh, [&](int j) {
            int ca, cb;
            local::guideCellSpan(j, ggh, H, ca, cb);
            const int ya = std::max(ca, y0), yb = std::min(cb, y1);
            if (ya >= yb) return;
            for (int i = 0; i < ggw; ++i) {
              int xa, xb;
              local::guideCellSpan(i, ggw, W, xa, xb);
              double* a = acc.data() + (std::size_t(j) * ggw + i) * 3;
              for (int y = ya; y < yb; ++y) {  // rows ascending, then columns, then channels: guideGrid's order
                const float* r = buf->row(y - b0);
                for (int x = xa; x < xb; ++x)
                  for (int c = 0; c < 3; ++c) a[c] += r[3 * x + c];
              }
            }
          });
        }
      }
      if (progress && !progress(float(y1) / float(H) * (pass == 0 ? 1.f : 0.5f))) return fail("cancelled");
    }
    for (const RowResizer& r : rz)
      if (r.next != r.g.dh) return fail("reduction incomplete");
    ++out.passes;
    if (decide && shadowChromaWanted(luma)) {  // rare (near-black frames): the reductions must see the floored picture, run again
      src.setShadowFloor(true);
      out.shadow = true;
      continue;
    }
    // results
    out.reduced.clear();
    for (std::size_t k = 0; k < rz.size(); ++k) {
      auto s = mkSrc(std::move(rz[k].dst));
      if (edges[k] == 256) out.p256 = s;
      else if (edges[k] == 512) out.p512 = s;
      out.reduced.emplace_back(edges[k], s);
    }
    out.grid = mkSrc(std::move(grid));
    if (gc) {
      for (std::size_t gi = 0; gi < gc->grids.size(); ++gi) {
        local::GuideCapture::Grid& g = gc->grids[gi];
        g.mean.assign(std::size_t(g.gw) * g.gh * 3, 0.f);
        for (int j = 0; j < g.gh; ++j) {
          int ya, yb;
          local::guideCellSpan(j, g.gh, H, ya, yb);
          for (int i = 0; i < g.gw; ++i) {
            int xa, xb;
            local::guideCellSpan(i, g.gw, W, xa, xb);
            const double n = double((yb - ya) * (xb - xa));
            const double* a = gacc[gi].data() + (std::size_t(j) * g.gw + i) * 3;
            float* o = g.mean.data() + (std::size_t(j) * g.gw + i) * 3;
            for (int c = 0; c < 3; ++c) o[c] = float(a[c] / n);
          }
        }
      }
      out.guide = gc;
    }
    break;
  }
  out.ms = msSince(t0);
  return true;
}

}  // namespace dapple::xstream
