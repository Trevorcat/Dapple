// Streaming (band) export: docs/engine/pipeline-architecture.md section 42.
#include "export_stream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dapple/renderer.h"
#include "engine/nodes/film/film_nodes.h"
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

Budget computeBudget(const BudgetInputs& in) {
  constexpr std::size_t MB = std::size_t(1) << 20, kNone = ~std::size_t(0);
  Budget b;
  char t[200];
  if (in.available == 0) {
    // the query failed (some contexts return 0): a quarter of the device minus half of what the process already holds, in [256 MB, 1 GB]
    std::size_t s = in.total ? in.total / 4 : 384 * MB;
    s = s > in.footprint ? s - std::min(in.footprint, s / 2) : s / 2;
    b.share = std::clamp(s, 256 * MB, 1024 * MB);
    std::snprintf(t, sizeof t, "available memory unknown: from the device size %.0f MB", double(b.share) / MB);
  } else {
    // iOS: os_proc_available_memory is this process's own headroom before jetsam; elsewhere the figure is shared with every other process
    const double share = in.platform == 2 ? 0.6 : 0.5;
    const std::size_t reserve = 96 * MB;  // the app's own growth while the export runs (UI, decode of thumbnails)
    const std::size_t s = std::size_t(double(in.available) * share);
    b.share = s > reserve ? s - reserve : 0;
    std::snprintf(t, sizeof t, "available memory %.0f MB x %.1f - 96 = %.0f", double(in.available) / MB, share, double(b.share) / MB);
  }
  b.how = t;
  b.ceiling = b.room = b.rowCacheCap = kNone;
  b.bytes = b.share;
  if (in.platform != 0 && in.total > 0) {
    b.ceiling = in.lowRam ? std::min(in.total / 8, 384 * MB) : std::min(in.total / 4, 1536 * MB);
    b.rowCacheCap = in.lowRam ? 96 * MB : 256 * MB;
    b.bytes = std::min(b.bytes, b.ceiling);
    std::snprintf(t, sizeof t, ", ceiling %.0f (device %.0f MB%s)", double(b.ceiling) / MB, double(in.total) / MB, in.lowRam ? ", low RAM" : "");
    b.how += t;
    if (in.platform == 1) {
      const std::size_t cap = std::size_t(double(in.total) * (in.lowRam ? 0.25 : 0.35));
      b.room = cap > in.footprint ? cap - in.footprint : 0;
      b.bytes = std::min(b.bytes, b.room);
      std::snprintf(t, sizeof t, ", room %.0f (process cap %.0f - footprint %.0f)", double(b.room) / MB, double(cap) / MB, double(in.footprint) / MB);
      b.how += t;
    }
  }
  std::snprintf(t, sizeof t, " -> %.0f MB", double(b.bytes) / MB);
  b.how += t;
  return b;
}

BudgetInputs currentInputs() {
  BudgetInputs in;
  in.available = std::getenv("DAPPLE_EXPORT_AVAILABLE_FAILS") ? 0 : mem::available();  // (env: the fallback, tests)
  in.total = mem::deviceTotal();
  in.footprint = mem::footprint();
  in.lowRam = mem::deviceClass().lowRam.load(std::memory_order_relaxed);
#if defined(__ANDROID__)
  in.platform = 1;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  in.platform = 2;
#endif
  if (const char* e = std::getenv("DAPPLE_EXPORT_DEVICE_MB"); e && std::atol(e) > 0) {  // the phone rule on any platform (tests)
    in.platform = 1;
    in.total = std::size_t(std::atol(e)) << 20;
  }
  return in;
}

Budget currentBudget() {
  if (const char* e = std::getenv("DAPPLE_EXPORT_BUDGET_MB"); e && e[0]) {
    Budget b;
    b.bytes = b.share = std::size_t(std::max(0L, std::atol(e))) << 20;
    b.ceiling = b.room = b.rowCacheCap = ~std::size_t(0);
    b.how = "DAPPLE_EXPORT_BUDGET_MB";
    return b;
  }
  return computeBudget(currentInputs());
}

std::size_t budgetBytes(std::string* how) {
  Budget b = currentBudget();
  if (how) *how = std::move(b.how);
  return b.bytes;
}

std::size_t predictGpuExtra(int W, int H, bool film, std::size_t poolBytes) {
  const double px = double(std::max(W, 0)) * double(std::max(H, 0));
  return std::size_t(px * (film ? 24.0 : 2.0)) + std::min(poolBytes, std::size_t(320) << 20);
}

std::size_t trimForExport(const std::uint64_t keepMosaic[2]) {
  std::size_t freed = trimMosaicCacheExcept(keepMosaic);  // other photos' mosaics and demosaiced pictures
  freed += engine::filmnode::releaseFrameWorkspaces();     // frame-sized film planes a previous film render left
  return freed;
}

std::size_t predictWholeDecodePeak(int W, int H) { return std::size_t(double(std::max(W, 0)) * double(std::max(H, 0)) * 30.0); }

std::size_t predictWholePeak(int W, int H, bool film) {
  const double px = double(std::max(W, 0)) * double(std::max(H, 0));
  return std::size_t(px * (12.0 + 3.0 + 4.0 + (film ? 55.0 : 0.0)));  // calibrated on the desktop runs (6 threads): 61 MP std +1.05 GB, film +4.3 GB
}

bool openLazy(int W, int H, std::string* why, std::size_t extraBytes, bool film) {
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
  std::string how;
  const std::size_t need = predictWholePeak(W, H, film) + extraBytes, budget = budgetBytes(&how);
  std::snprintf(b, sizeof b, "whole-frame peak %.0f MB (%sGPU %.0f) vs budget %.0f MB", double(need) / 1048576.0, film ? "film, " : "", double(extraBytes) / 1048576.0,
                double(budget) / 1048576.0);
  if (why) *why = std::string(b) + " (" + how + ")";
  return need > budget;
}

// ---- BandSource ----------------------------------------------------------------------------------------------------------------------------
BandSource::BandSource(std::shared_ptr<const MosaicImage> mosaic) : m_(std::move(mosaic)) {
  if (!m_) return;
  demosaic::orientedSize(*m_, &W_, &H_);
  gpu_ = demosaic::fullUsesGpu(*m_);
}

int BandSource::cachedRows() const {
  int n = 0;
  for (const Block& b : cache_) n += b.y1 - b.y0;
  return n;
}
void BandSource::dropCache() {
  std::vector<Block>().swap(cache_);
  cacheBytes_ = 0;
}
void BandSource::keep(const ImageF& win, int y0, int ya, int yb) {
  if (cacheBudget_ == 0 || win.width != W_ || yb <= ya) return;
  // only rows not cached yet (the blocks stay disjoint)
  for (const Block& b : cache_) {
    if (b.y0 <= ya && ya < b.y1) ya = b.y1;
    if (b.y0 < yb && yb <= b.y1) yb = b.y0;
  }
  if (yb <= ya) return;
  const std::size_t rowBytes = std::size_t(W_) * 3 * sizeof(float);
  // room: the lowest blocks give way to rows above them (the render starts at the top)
  while (cacheBytes_ + std::size_t(yb - ya) * rowBytes > cacheBudget_ && !cache_.empty()) {
    auto low = std::max_element(cache_.begin(), cache_.end(), [](const Block& a, const Block& b) { return a.y0 < b.y0; });
    if (low->y0 < ya) break;  // what is cached lies above: keep it, drop the new rows instead
    cacheBytes_ -= low->px.size() * sizeof(float);
    cache_.erase(low);
  }
  if (cacheBytes_ + std::size_t(yb - ya) * rowBytes > cacheBudget_) return;
  Block b;
  b.y0 = ya, b.y1 = yb;
  b.px.assign(win.row(ya - y0), win.row(ya - y0) + std::size_t(yb - ya) * W_ * 3);
  cacheBytes_ += b.px.size() * sizeof(float);
  cache_.push_back(std::move(b));
}
void BandSource::applyShadowFloorToCache() {
  for (Block& b : cache_) {
    ImageF v;
    v.width = W_, v.height = b.y1 - b.y0;
    v.data.swap(b.px);
    shadowChromaFloor(v);
    v.data.swap(b.px);
  }
}
bool BandSource::fromCache(int x0, int y0, int x1, int y1) {
  if (cache_.empty()) return false;
  std::vector<const Block*> parts;
  for (int y = y0; y < y1;) {  // every row must be cached
    const Block* hit = nullptr;
    for (const Block& b : cache_)
      if (b.y0 <= y && y < b.y1) hit = &b;
    if (!hit) return false;
    parts.push_back(hit);
    y = hit->y1;
  }
  ImageF().data.swap(win_.data);
  win_ = ImageF(x1 - x0, y1 - y0);
  parallelFor(y1 - y0, [&](int k) {
    const int y = y0 + k;
    for (const Block* b : parts)
      if (b->y0 <= y && y < b->y1) {
        std::memcpy(win_.row(k), b->px.data() + (std::size_t(y - b->y0) * W_ + x0) * 3, std::size_t(x1 - x0) * 3 * sizeof(float));
        break;
      }
  });
  ++cacheHits_;
  return true;
}

const ImageF* BandSource::window(int x0, int y0, int x1, int y1) {
  if (!m_ || x0 < 0 || y0 < 0 || x1 > W_ || y1 > H_ || x1 <= x0 || y1 <= y0) return nullptr;
  if (cancelCur_ && cancelCur_->load(std::memory_order_relaxed) != cancelMine_) return nullptr;
  if (fromCache(x0, y0, x1, y1)) {
    rowsY0_ = y0;
    peakBytes_ = std::max(peakBytes_, win_.data.size() * sizeof(float));
    return &win_;
  }
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
  rowsY0_ = y0;  // rows() reuses a full-width window
  ++windows_;
  ms_ += msSince(t0);
  peakBytes_ = std::max(peakBytes_, win_.data.size() * sizeof(float));
  return &win_;
}

std::uint64_t BandSourceStore::next() {
  static std::atomic<std::uint64_t> n{1ull << 40};
  return n.fetch_add(1);
}
const float* BandSource::rows(int x0, int y0, int x1, int y1, std::size_t& pitchFloats) {
  // tiles of a row of tiles share their rows: one full-width band serves them all (one demosaic call instead of one per tile). A narrow rect
  // (a quarter turn: a row of output tiles reads a column of the source) is read as it is.
  const bool inBand = win_.width == W_ && y0 >= rowsY0_ && y1 <= rowsY0_ + win_.height && !win_.data.empty();
  if (!inBand && 2 * (x1 - x0) < W_) {
    if (!window(x0, y0, x1, y1)) return nullptr;
    pitchFloats = std::size_t(x1 - x0) * 3;
    return win_.data.data();
  }
  if (!inBand) {
    const int b1 = std::min(H_, std::max(y1, y0 + kBandRows));
    if (!window(0, y0, W_, b1)) return nullptr;
  }
  if (x0 < 0 || x1 > W_) return nullptr;
  pitchFloats = std::size_t(W_) * 3;
  return win_.data.data() + std::size_t(y0 - rowsY0_) * pitchFloats + std::size_t(x0) * 3;
}

const float* BandSourceStore::window(int x0, int y0, int w, int h, std::size_t& pitchFloats) { return src_.rows(x0, y0, x0 + w, y0 + h, pitchFloats); }

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
  std::vector<char> done;
  int count = 0;
  int footprint() const { return int(std::ceil(g.sy)) + 2; }
  // rows [b0, b1) of the picture are in `buf` (row b0 at buf row 0): every output row whose footprint lies inside is computed (once), in any
  // order of the bands
  void feed(const ImageF& buf, int b0, int b1) {
    if (done.empty()) done.assign(std::size_t(g.dh), 0);
    std::vector<int> ys;
    for (int y = std::max(0, int(b0 / g.sy) - 1); y < g.dh; ++y) {
      int y0, y1;
      resize::rowSpan(g, y, y0, y1);
      if (y0 >= b1) break;
      if (done[std::size_t(y)] || y0 < b0 || y1 > b1) continue;
      ys.push_back(y);
    }
    if (ys.empty()) return;
    parallelFor(int(ys.size()), [&](int k) {
      const int y = ys[std::size_t(k)];
      resize::row(g, y, [&](int yy) { return buf.row(yy - b0); }, dst.row(y));
    });
    for (int y : ys) done[std::size_t(y)] = 1;
    count += int(ys.size());
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
    const int B = std::max(512, K);  // one demosaic call per 512 rows (fewer GPU round trips)
    // bottom to top when the rows may be kept for the render (it starts at the top); the raster guide's area sums need top to bottom
    const bool up = !gc && pass == 0;
    const int nb = (H + B - 1) / B;
    for (int bi = 0; bi < nb; ++bi) {
      const int y0 = (up ? nb - 1 - bi : bi) * B, y1 = std::min(H, y0 + B);
      const int b0 = up ? y0 : std::max(0, y0 - K), b1 = up ? std::min(H, y1 + K) : y1;  // the footprint overlap below (up) / above (down)
      const ImageF* buf = src.window(0, b0, W, b1);
      if (!buf) return fail("cancelled");
      src.keep(*buf, b0, y0, y1);
      // shadow-floor decision samples: every 4th row / column (before the floor)
      if (decide)
        for (int y = (y0 + 3) / 4 * 4; y < y1; y += 4) {
          const float* p = buf->row(y - b0);
          for (int x = 0; x < W; x += 4) luma.push_back(shadowChromaLuma(p + 3 * x));
        }
      for (RowResizer& r : rz) r.feed(*buf, b0, b1);
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
      if (progress && !progress(float(bi + 1) / float(nb) * (pass == 0 ? 1.f : 0.5f))) return fail("cancelled");
    }
    for (const RowResizer& r : rz)
      if (r.count != r.g.dh) return fail("reduction incomplete");
    ++out.passes;
    if (decide && shadowChromaWanted(luma)) {  // rare (near-black frames): the reductions must see the floored picture, run again
      src.setShadowFloor(true);
      src.applyShadowFloorToCache();
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
