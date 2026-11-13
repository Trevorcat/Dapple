#include "engine/nodes/demosaic/demosaic_gpu.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <memory>
#include <mutex>

#include "engine/exec/gpu_runtime.h"
#include <cstdio>
#include <filesystem>
#include <map>
#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif
#include "engine/hal/select.h"
#include "engine/nodes/demosaic/demosaic_backend.h"
#include "engine/nodes/gpu_common.h"

namespace dapple::demosaic {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
constexpr std::uint64_t kWaitNs = 30ull * 1000 * 1000 * 1000;

struct U {
  std::int32_t dims[4], org[4], io[4];
  std::uint32_t cfa[4];
  float wb[4], sc[4], m0[4], m1[4], m2[4];
};
static_assert(sizeof(U) == 144, "std140 layout of the demosaic uniform block");

inline int pmod(int a, int n) { return ((a % n) + n) % n; }
constexpr int kHalfTab = 40 + 4096 + 576 + 144;     // first float of the half-resolution tables in the table buffer (demosaic_half.comp: HT)
constexpr int kTabFloats = kHalfTab + 1728 * 2 + 576 * 2;

float halfToFloat(std::uint16_t h) {
  const std::uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
  std::uint32_t f;
  if (e == 0) {
    if (m == 0) f = s << 31;
    else {
      int ee = -1;
      std::uint32_t mm = m;
      do { ++ee; mm <<= 1; } while (!(mm & 0x400u));
      f = (s << 31) | (std::uint32_t(127 - 15 - ee) << 23) | ((mm & 0x3FFu) << 13);
    }
  } else if (e == 31) {
    f = (s << 31) | 0x7F800000u | (m << 13);
  } else {
    f = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
  }
  float r;
  std::memcpy(&r, &f, 4);
  return r;
}
}  // namespace

struct GpuDemosaic::Impl {
  hal::Device* dev = nullptr;
  bool half = false;
  int lx = 16, ly = 16, rwg = 256;
  int tile = 0, period = 0;
  hal::BufferH staging, mosaic, tab, S, G, D, Hh, readback, readback2;  // two read-back buffers: the host converts tile n while the device renders tile n + 1
  hal::ImageH out;
  hal::PipelineH pScale, pG, pC, pH, pO;
  Cfa cfa = Cfa::Bayer;
  int xt = 1, extra = 0;
  bool hf = false;  // half-resolution path (Desc::half): window = 2 x tile + 6, two passes (scale, reconstruction)
  void release() {
    for (hal::BufferH* b : {&staging, &mosaic, &tab, &S, &G, &D, &Hh, &readback, &readback2})
      if (*b) dev->destroy(*b);
    if (out) dev->destroy(out);
    tile = 0;
  }
};

std::unique_ptr<GpuDemosaic> GpuDemosaic::create(hal::Device& dev, bool half, std::string& err) {
  std::unique_ptr<GpuDemosaic> g(new GpuDemosaic);
  g->p_ = std::make_unique<Impl>();
  g->p_->dev = &dev;
  g->p_->half = half;
  engine::gpu::workgroupShape(dev.caps(), g->p_->lx, g->p_->ly, g->p_->rwg);
  (void)err;
  return g;
}

GpuDemosaic::~GpuDemosaic() {
  if (p_) {
    p_->dev->waitIdle();
    p_->release();
  }
}

hal::Status GpuDemosaic::run(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int tile, GpuStats* st) {
  using hal::Status;
  Impl& P = *p_;
  hal::Device& dev = *P.dev;
  const auto tAll = Clock::now();
  GpuStats local;
  GpuStats& S_ = st ? *st : local;
  S_ = GpuStats{};
  if (roi.empty()) return Status::Ok;
  const bool hf = d.half && d.cfa == Cfa::XTrans;  // half-resolution: roi and output in half-resolution pixels, window of 2 x tile + 6 samples
  const int H = hf ? 3 : haloOf(d), per = d.period;
  tile = std::max(16, tile);
  const int E = hf ? 0 : hotExtra(d);  // extra samples of the staged window for the hot-sample filter
  const int ww = hf ? 2 * tile + 6 : tile + 2 * H, wh = ww, wwE = ww + 2 * E, whE = wh + 2 * E;
  // (re)allocate for this tile size / CFA
  const bool cand = d.cfa == Cfa::XTrans && d.xtMode >= 1 && !hf;
  if (P.tile != tile || P.cfa != d.cfa || P.period != per || P.xt != (cand ? 1 : 0) || P.extra != E || P.hf != hf) {
    dev.waitIdle();
    P.release();
    const std::uint64_t planeBytes = std::uint64_t(ww) * wh * 4;
    const std::uint64_t winBytes = (std::uint64_t(wwE) * whE + 1) / 2 * 4;
    auto mk = [&](std::uint64_t bytes, hal::MemKind k, bool storage, hal::BufferH* b) { return dev.createBuffer({bytes, k, storage}, b); };
    Status s = mk(winBytes, hal::MemKind::Upload, true, &P.staging);
    if (s == Status::Ok) s = mk(winBytes, hal::MemKind::DeviceLocal, true, &P.mosaic);
    if (s == Status::Ok) s = mk(std::uint64_t(kTabFloats) * 4, hal::MemKind::Upload, true, &P.tab);
    if (s == Status::Ok) s = mk(planeBytes, hal::MemKind::DeviceLocal, true, &P.S);
    if (s == Status::Ok) s = mk(hf ? 256 : (cand ? planeBytes * 4 : planeBytes), hal::MemKind::DeviceLocal, true, &P.G);  // candidates: 4 greens per pixel (the half path has none)
    const std::uint64_t dBytes = cand ? planeBytes * 8 : std::uint64_t(tile + 2) * (tile + 2) * 8;
    if (s == Status::Ok && d.cfa == Cfa::XTrans && !hf) s = mk(dBytes, hal::MemKind::DeviceLocal, true, &P.D);  // candidates: red / blue differences x 4 directions
    if (s == Status::Ok && cand) s = mk(planeBytes * 4, hal::MemKind::DeviceLocal, true, &P.Hh);       // candidates: 4 derivatives
    const hal::Format of = P.half ? hal::Format::RGBA16F : hal::Format::RGBA32F;
    if (s == Status::Ok) s = dev.createImage({of, std::uint32_t(tile), std::uint32_t(tile), 1}, &P.out);
    if (s == Status::Ok) s = mk(std::uint64_t(tile) * tile * hal::bytesPerTexel(of), hal::MemKind::Readback, false, &P.readback);
    if (s == Status::Ok) s = mk(std::uint64_t(tile) * tile * hal::bytesPerTexel(of), hal::MemKind::Readback, false, &P.readback2);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      P.release();
      return s;
    }
    S_.deviceBytes = planeBytes + (hf ? 256 : (cand ? planeBytes * 4 : planeBytes)) + winBytes + std::uint64_t(tile) * tile * hal::bytesPerTexel(of) + (d.cfa == Cfa::XTrans && !hf ? dBytes : 0) + (cand ? planeBytes * 4 : 0);
    auto pipe = [&](const char* name, hal::PipelineH* ph, bool wantPeriod) {
      hal::PipelineDesc pd;
      pd.shader = name;
      if (wantPeriod) pd.spec.push_back({0, per});
      pd.spec.push_back({16, P.lx});
      pd.spec.push_back({17, P.ly});
      pd.spec.push_back({19, P.lx * P.ly});
      return dev.createPipeline(pd, ph);
    };
    const bool bay = d.cfa == Cfa::Bayer;
    s = pipe("dm_scale", &P.pScale, true);
    if (hf) {
      if (s == Status::Ok) s = pipe(P.half ? "dm_h16" : "dm_h32", &P.pO, false);
      if (s != Status::Ok) {
        err_ = dev.lastError();
        return s;
      }
      P.tile = tile, P.cfa = d.cfa, P.period = per, P.xt = 0, P.extra = E, P.hf = true;
    } else {
    if (s == Status::Ok) s = pipe(bay ? "dm_bg" : (cand ? "dm_x0" : "dm_fg"), &P.pG, false);
    if (s == Status::Ok && !bay) s = pipe(cand ? "dm_x1" : "dm_fc", &P.pC, false);
    if (s == Status::Ok && cand) s = pipe("dm_x2", &P.pH, false);
    if (s == Status::Ok) s = pipe(bay ? (P.half ? "dm_bo16" : "dm_bo32") : cand ? (P.half ? "dm_x5_16" : "dm_x5_32") : (P.half ? "dm_fo16" : "dm_fo32"), &P.pO, false);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      return s;
    }
    P.tile = tile, P.cfa = d.cfa, P.period = per, P.xt = cand ? 1 : 0, P.extra = E, P.hf = false;
    }
  } else {
    S_.deviceBytes = 0;
  }
  // tables: colour index per cell, period, black table
  {
    float* t = static_cast<float*>(P.tab.mapped);
    std::memset(t, 0, 40 * 4);
    for (int i = 0; i < per * per; ++i) t[i] = float(d.cfa4[i]);
    t[36] = float(per), t[37] = float(d.blackRows), t[38] = float(d.blackCols);
    const int nb = d.blackRows * d.blackCols;
    if (nb > 4096) {
      err_ = "black table too large";
      return Status::InvalidArgument;
    }
    std::memcpy(t + 40, d.black, std::size_t(nb) * 4);
    if (cand) {  // green neighbour entries per (phase, direction), see demosaic_xtrans.comp MODE 0
      std::uint32_t* ge = reinterpret_cast<std::uint32_t*>(t) + 40 + 4096 + 576;
      for (int ph = 0; ph < 36; ++ph)
        for (int dir = 0; dir < 4; ++dir) {
          const int ex = dir == 0 ? 1 : (dir == 1 ? 0 : 1), ey = dir == 0 ? 0 : (dir == 1 ? 1 : (dir == 2 ? 1 : -1));
          auto colorAt = [&](int du, int dv) { return int(d.cfa3[pmod(ph / 6 + dv, 6) * 6 + pmod(ph % 6 + du, 6)]); };
          const int c = colorAt(0, 0);
          std::uint32_t e = 0;
          if (c == 1) e = 1u << 10;
          else {
            for (int sg = -1; sg <= 1; sg += 2) {
              int q = 1;
              for (; q <= 3; ++q)
                if (colorAt(sg * q * ex, sg * q * ey) == 1) break;
              if (q > 3) q = 0;
              e |= std::uint32_t(q) << (sg < 0 ? 0 : 2);
            }
            for (int qi = 0; qi < 6; ++qi) {
              const int q = qi < 3 ? qi - 3 : qi - 2;
              if (colorAt(q * ex, q * ey) == c) e |= 1u << (4 + qi);
            }
          }
          ge[ph * 4 + dir] = e;
        }
    }
    if (cand) {  // tap masks of the colour differences: [phase][direction][R, B] x 2 words, bit (v + 3) * 7 + (u + 3) of the 7 x 7 neighbourhood (see demosaic_xtrans.comp MODE 1)
      std::uint32_t* mk4 = reinterpret_cast<std::uint32_t*>(t) + 40 + 4096;
      std::memset(mk4, 0, 576 * 4);
      for (int ph = 0; ph < 36; ++ph)
        for (int dir = 0; dir < 4; ++dir) {
          const int ex = dir == 0 ? 1 : (dir == 1 ? 0 : 1), ey = dir == 0 ? 0 : (dir == 1 ? 1 : (dir == 2 ? 1 : -1));
          const float ln = dir >= 2 ? 1.41421356f : 1.0f;
          for (int tt = 0; tt < 2; ++tt)
            for (int v = -3; v <= 3; ++v)
              for (int uu = -3; uu <= 3; ++uu) {
                const int r2 = uu * uu + v * v;
                if (r2 == 0 || r2 > 10) continue;
                const float perp = std::abs(float(uu * ey - v * ex)) / ln;
                if (perp > 1.01f) continue;
                if (d.cfa3[pmod(ph / 6 + v, 6) * 6 + pmod(ph % 6 + uu, 6)] != tt * 2) continue;
                const int b = (v + 3) * 7 + (uu + 3);
                mk4[((ph * 4 + dir) * 2 + tt) * 2 + (b >> 5)] |= 1u << (b & 31);
              }
        }
    }
    if (hf) {  // the half-resolution tables (demosaic_half.comp): weights, fallback, support
      const HalfTables& T = halfTables(d);
      float* h = t + kHalfTab;
      std::memcpy(h, T.w, sizeof T.w);
      std::memcpy(h + 1728, T.g, sizeof T.g);
      std::memcpy(h + 3456, T.sup, sizeof T.sup);
      std::memcpy(h + 4032, T.cen, sizeof T.cen);
    }
    dev.flush(P.tab, 0, std::uint64_t(kTabFloats) * 4);
  }
  std::uint32_t cfaBits[4] = {0, 0, 0, 0};
  for (int i = 0; i < per * per; ++i) cfaBits[i >> 4] |= std::uint32_t(d.cfa3[i] & 3) << ((i & 15) * 2);
  std::vector<int> xs(std::size_t(tile) + 2 * H);
  struct Joiner {
    std::thread* t;
    ~Joiner() {
      for (int k = 0; k < 2; ++k)
        if (t[k].joinable()) t[k].join();
    }
  };
  std::thread pend[2];
  Joiner joiner{pend};
  const bool bay = d.cfa == Cfa::Bayer;
  for (int ty = roi.y0; ty < roi.y1; ty += tile)
    for (int tx = roi.x0; tx < roi.x1; tx += tile) {
      yieldWhileInteracting(d.cancelCur, d.cancelMine);
      if (d.cancelCur && d.cancelCur->load(std::memory_order_relaxed) != d.cancelMine) return Status::Timeout;  // cancelled
      const Rect t{tx, ty, std::min(tx + tile, roi.x1), std::min(ty + tile, roi.y1)};
      const int rw = t.w(), rh = t.h(), cw = hf ? 2 * rw + 6 : rw + 2 * H, ch = hf ? 2 * rh + 6 : rh + 2 * H;
      auto t0 = Clock::now();
      // window of the mosaic, coordinate mapped
      std::uint16_t* win = static_cast<std::uint16_t*>(P.staging.mapped);
      const int wx0 = hf ? 2 * t.x0 - 3 : t.x0 - H, wy0 = hf ? 2 * t.y0 - 3 : t.y0 - H, cwE = cw + 2 * E, chE = ch + 2 * E;
      const bool xin = wx0 - E >= 0 && wx0 - E + cwE <= d.width;
      xs.resize(std::size_t(cwE));
      for (int i = 0; i < cwE; ++i) xs[std::size_t(i)] = mapCoord(wx0 - E + i, d.width, d.cfa);
      for (int j = 0; j < chE; ++j) {
        const std::uint16_t* srow = d.data + std::size_t(mapCoord(wy0 - E + j, d.height, d.cfa)) * d.width;
        std::uint16_t* drow = win + std::size_t(j) * cwE;
        if (xin) std::memcpy(drow, srow + wx0 - E, std::size_t(cwE) * 2);
        else
          for (int i = 0; i < cwE; ++i) drow[i] = srow[xs[std::size_t(i)]];
      }
      const std::uint64_t winBytes = (std::uint64_t(cwE) * chE + 1) / 2 * 4;
      dev.flush(P.staging, 0, winBytes);
      U u{};
      u.dims[0] = cw, u.dims[1] = ch, u.dims[2] = rw, u.dims[3] = rh;
      u.org[0] = pmod(wx0, per), u.org[1] = pmod(wy0, per), u.org[2] = pmod(wx0, d.blackCols), u.org[3] = pmod(wy0, d.blackRows);
      u.io[0] = cwE, u.io[1] = 0, u.io[2] = 0, u.io[3] = E;
      std::memcpy(u.cfa, cfaBits, 16);
      for (int c = 0; c < 4; ++c) u.wb[c] = d.wb[c];
      u.sc[0] = d.invSpan, u.sc[1] = d.clampOut ? 1.f : 0.f, u.sc[2] = d.hot && !hf ? 1.f : 0.f;
      for (int c = 0; c < 3; ++c) u.m0[c] = d.m[c], u.m1[c] = d.m[3 + c], u.m2[c] = d.m[6 + c];
      S_.prepMs += msSince(t0);
      t0 = Clock::now();
      auto cl = dev.begin();
      if (!cl) {
        err_ = dev.lastError();
        return Status::DeviceLost;
      }
      static const int skipMask = std::getenv("DEMO_SKIP") ? std::atoi(std::getenv("DEMO_SKIP")) : 0;  // diagnostics: skip dispatch n
      int passNo = 0;
      auto disp = [&](const char* nm, hal::PipelineH ph, std::initializer_list<hal::BufferH> bufs, hal::ImageH* img, int gw, int gh, bool lds = false) {
        if ((skipMask >> passNo++) & 1) return;
        cl->bindPipeline(ph);
        std::uint32_t slot = 0;
        for (const hal::BufferH& b : bufs) cl->bind(slot++, b);
        if (img) cl->bind(slot++, *img);
        cl->setParams(&u, sizeof u);
        if (lds) cl->dispatch(std::uint32_t((gw + 15) / 16), std::uint32_t((gh + 15) / 16), 1);
        else cl->dispatch(std::uint32_t((gw + P.lx - 1) / P.lx), std::uint32_t((gh + P.ly - 1) / P.ly), 1);
        cl->barrier(hal::kAll);
        cl->timestamp(nm);
      };
      cl->timestamp("start");
      cl->copyBufferToBuffer(P.staging, P.mosaic, winBytes, 0);
      cl->barrier(hal::kAll);
      disp("scale", P.pScale, {P.mosaic, P.tab, P.S}, nullptr, cw, ch);
      if (hf) {
        disp("half", P.pO, {P.S, P.tab}, &P.out, rw, rh);
      } else if (cand) disp("green", P.pG, {P.S, P.G, P.tab}, nullptr, cw, ch, true);
      else disp("green", P.pG, {P.S, P.G}, nullptr, cw, ch);
      if (hf) {
      } else if (bay) {
        disp("out", P.pO, {P.S, P.G}, &P.out, rw, rh);
      } else if (cand) {
        disp("chroma", P.pC, {P.S, P.G, P.D, P.tab}, nullptr, cw, ch, true);
        disp("deriv", P.pH, {P.S, P.G, P.D, P.Hh}, nullptr, cw, ch, true);
        disp("select+out", P.pO, {P.S, P.G, P.D, P.Hh}, &P.out, rw, rh, true);
      } else {
        disp("chroma", P.pC, {P.S, P.G, P.D}, nullptr, rw + 2, rh + 2);
        disp("out", P.pO, {P.G, P.D}, &P.out, rw, rh);
      }
      const int rbi = int(S_.tiles & 1);
      hal::BufferH& rbuf = rbi ? P.readback2 : P.readback;
      if (out) {
        if (pend[rbi].joinable()) pend[rbi].join();  // the conversion of the tile two before must have finished reading this buffer
        cl->copyImageToBuffer(P.out, rbuf, hal::Rect2{0, 0, std::uint32_t(rw), std::uint32_t(rh)}, 0);
      }
      hal::FenceH f;
      Status s = dev.submit(*cl, &f);
      if (s == Status::Ok) s = dev.wait(f, kWaitNs);
      if (s != Status::Ok) {
        err_ = dev.lastError();
        return s;
      }
      S_.submit = dev.stats(f);
      for (const hal::PassTime& pt : dev.passTimes(f)) {
        auto it = std::find_if(S_.passTimes.begin(), S_.passTimes.end(), [&](const hal::PassTime& q) { return q.name == pt.name; });
        if (it == S_.passTimes.end()) S_.passTimes.push_back(pt);
        else it->ms += pt.ms;
      }
      S_.gpuMs += msSince(t0);
      if (out) {
        t0 = Clock::now();
        const std::uint64_t texel = hal::bytesPerTexel(P.half ? hal::Format::RGBA16F : hal::Format::RGBA32F);
        dev.invalidate(rbuf, 0, std::uint64_t(rw) * rh * texel);
        const unsigned char* src = static_cast<const unsigned char*>(rbuf.mapped);
        // the read-back memory is slow to read from one core (a phone measured 0.5 - 7 s for 40 MP on one thread): rows are split over a few threads
        const bool halfOut = P.half;
        const int ty0 = t.y0 - roi.y0, tx0 = t.x0 - roi.x0;
        auto convert = [src, out, strideFloats, rw, rh, halfOut, ty0, tx0]() {
        auto rows = [&](int ya, int yb) {
          for (int y = ya; y < yb; ++y) {
            float* o = out + std::size_t(ty0 + y) * strideFloats + std::size_t(tx0) * 3;
            if (halfOut) {
              const std::uint16_t* r = reinterpret_cast<const std::uint16_t*>(src) + std::size_t(y) * rw * 4;
#if defined(__aarch64__)
              const __fp16* h = reinterpret_cast<const __fp16*>(r);  // the hardware conversion
              for (int x = 0; x < rw; ++x) o[3 * x] = float(h[4 * x]), o[3 * x + 1] = float(h[4 * x + 1]), o[3 * x + 2] = float(h[4 * x + 2]);
#else
              for (int x = 0; x < rw; ++x) o[3 * x] = halfToFloat(r[4 * x]), o[3 * x + 1] = halfToFloat(r[4 * x + 1]), o[3 * x + 2] = halfToFloat(r[4 * x + 2]);
#endif
            } else {
              const float* r = reinterpret_cast<const float*>(src) + std::size_t(y) * rw * 4;
              for (int x = 0; x < rw; ++x) o[3 * x] = r[4 * x], o[3 * x + 1] = r[4 * x + 1], o[3 * x + 2] = r[4 * x + 2];
            }
          }
        };
        const int nth = rh >= 128 ? int(std::min(4u, std::max(1u, std::thread::hardware_concurrency()))) : 1;
        if (nth <= 1) rows(0, rh);
        else {
          std::vector<std::thread> th;
          for (int k = 1; k < nth; ++k) th.emplace_back(rows, rh * k / nth, rh * (k + 1) / nth);
          rows(0, rh / nth);
          for (std::thread& x : th) x.join();
        }
        };
        pend[rbi] = std::thread(convert);  // overlaps the next tile
        S_.readbackMs += msSince(t0);
      }
      ++S_.tiles;
    }
  for (std::thread& th : pend)
    if (th.joinable()) th.join();
  S_.totalMs = msSince(tAll);
  return Status::Ok;
}

// ---- the backend the decoder uses (demosaic_backend.h) -----------------------------------------------------------------------------------------
// Its own device, created on the first use through the same selection as the executor (Vulkan -> GLES 3.1, DAPPLE_GPU_API forces one). The provider of
// the executor (gpu_iface.h) hands out executors and Vulkan handles but no hal::Device; a second device in the process is cheap (the node needs 30 - 80 MB
// of buffers) and keeps this file out of the executor's files. The runtime switch (dapple_gpu_set_enabled / DAPPLE_GPU / debug.dapple.gpu) is honoured.
namespace {
class DeviceBackend final : public GpuBackend {
 public:
  bool available(std::string& why) override {
    if (!engine::gpu::runtimeEnabled()) return why = "GPU runtime switch off", false;
    std::lock_guard<std::mutex> lk(mu_);
    return ensure(why);
  }
  // The verdict (docs/engine/pipeline-architecture.md 32.11): GPU node or CPU node, for a whole frame and for a region, by measurement on this device. Probes of
  // 2048 x 1536 (frame) and 1024 x 576 (region) synthetic samples with the file's pattern / matrices, GPU with the read-back the decoder does, CPU on every
  // core; the faster one wins, a tie goes to the CPU (no device work, no read-back). Measured on the first use in the process, cached on disk next to the
  // other verdicts (build fingerprint + app version + API + device key + pattern); DAPPLE_DEMOSAIC_NODE=gpu|cpu forces one.
  bool prefer(const Desc& d, Rect roi, std::string& why) override {
    static const int forced = [] {
      const char* e = std::getenv("DAPPLE_DEMOSAIC_NODE");
      return e && std::strcmp(e, "gpu") == 0 ? 1 : (e && std::strcmp(e, "cpu") == 0 ? 0 : -1);
    }();
    if (forced >= 0) return forced == 1 ? true : (why = "DAPPLE_DEMOSAIC_NODE=cpu", false);
    const bool hf = d.half && d.cfa == Cfa::XTrans;  // half-resolution path: its own verdict kind (docs 32.12), the roi is in half-resolution pixels
    const bool region = std::int64_t(roi.w()) * roi.h() * 2 < std::int64_t(d.width) * d.height / (hf ? 4 : 1);
    std::lock_guard<std::mutex> lk(mu_);
    std::string w2;
    if (!ensure(w2)) return true;  // run() reports the failure
    const int key = int(d.cfa) * 8 + (d.xtMode >= 1 ? 4 : 0) + (d.hot ? 2 : 0) + (hf ? 64 : 0);
    auto it = verdicts_.find(key);
    if (it == verdicts_.end()) it = verdicts_.emplace(key, measure(d)).first;
    const bool gpu = region ? it->second.region : it->second.full;
    if (!gpu) why = std::string("slower than the CPU node here (") + it->second.note + ")";
    return gpu;
  }
  bool run(const Desc& d, Rect roi, float* out, std::size_t stride, std::string& why) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure(why)) return false;
    const hal::Status s = gpu_->run(d, roi, out, stride, tile_);
    if (s != hal::Status::Ok) {
      why = gpu_->error();
      if (s == hal::Status::DeviceLost) failed_ = "device lost";  // no retry in this process: the CPU node takes over
      return false;
    }
    return true;
  }

 private:
  struct Verdict {
    bool full = true, region = true;
    double ms[4] = {0, 0, 0, 0};  // GPU frame, CPU frame, GPU region, CPU region
    std::string note;
  };
  static void logLine(const std::string& l) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "dapple_demosaic", "%s", l.c_str());
#elif defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "dapple_demosaic: %{public}s", l.c_str());  // stderr of a release app on a device is not collected
#endif
    std::fprintf(stderr, "dapple_demosaic: %s\n", l.c_str());
  }
  static std::string noteOf(const Verdict& v, const char* tail) {
    char nt[240];
    std::snprintf(nt, sizeof nt, "frame GPU %.0f ms vs CPU %.0f ms, region GPU %.0f ms vs CPU %.0f ms%s", v.ms[0], v.ms[1], v.ms[2], v.ms[3], tail);
    return nt;
  }
  Verdict measure(const Desc& d) {
    Verdict v;
    std::string path;
    std::string dir, app, fp;
    engine::gpu::cacheConfig(&dir, &app, &fp);
    char key[200];
    const bool hf = d.half && d.cfa == Cfa::XTrans;
    std::snprintf(key, sizeof key, "%s|%s|%s|%s|%d|%d|%d|%d%s", fp.c_str(), app.c_str(), hal::apiName(dev_->caps().api), dev_->caps().key().c_str(), int(d.cfa), d.xtMode, d.hot ? 1 : 0, 1, hf ? "|half1" : "");
    if (!dir.empty()) {
      char nm[96];
      std::snprintf(nm, sizeof nm, "demosaic_verdict_%016llx.txt", (unsigned long long)std::hash<std::string>{}(key));
      path = (std::filesystem::path(dir) / nm).string();
      if (FILE* f = std::fopen(path.c_str(), "r")) {
        int a = -1, b = -1;
        const int n = std::fscanf(f, "%d %d %lf %lf %lf %lf", &a, &b, &v.ms[0], &v.ms[1], &v.ms[2], &v.ms[3]);
        std::fclose(f);
        if (n == 6 && (a == 0 || a == 1) && (b == 0 || b == 1)) {
          v.full = a == 1, v.region = b == 1;
          v.note = noteOf(v, " (cached)");
          logLine(std::string("verdict frame ") + (v.full ? "GPU" : "CPU") + ", region " + (v.region ? "GPU" : "CPU") + ": " + v.note);
          return v;
        }
      }
    }
    const int sizes[2][2] = {{2048, 1536}, {1024, 576}};
    for (int k = 0; k < 2; ++k) {
      const int pw = sizes[k][0], ph = sizes[k][1];
      std::vector<std::uint16_t> data(std::size_t(pw) * ph);
      std::uint32_t seed = 12345u;
      const float span = 1.f / d.invSpan;
      float blk = 1e30f;
      for (int i = 0; i < d.blackRows * d.blackCols; ++i) blk = std::min(blk, d.black[i]);
      for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
          seed = seed * 1664525u + 1013904223u;
          const float t = 0.05f + 0.6f * (0.5f + 0.5f * std::sin(float(x) * 0.013f + float(y) * 0.007f)) + 0.1f * float(seed >> 8) / 16777216.f;
          data[std::size_t(y) * pw + x] = std::uint16_t(std::min(65535.f, blk + t * span));
        }
      Desc p = d;
      p.width = pw, p.height = ph, p.data = data.data(), p.cancelCur = nullptr;
      const Rect all = hf ? Rect{0, 0, pw / 2, ph / 2} : Rect{0, 0, pw, ph};  // the half path outputs half-resolution pixels
      std::vector<float> out(std::size_t(all.w()) * all.h() * 3);
      using Ck = std::chrono::steady_clock;
      auto ms = [](Ck::time_point t) { return std::chrono::duration<double, std::milli>(Ck::now() - t).count(); };
      double g = 1e30, c = 1e30;
      if (gpu_->run(p, all, out.data(), std::size_t(all.w()) * 3, tile_) == hal::Status::Ok)  // warm-up: pipelines, allocation
        for (int rep = 0; rep < 3; ++rep) {
          const auto t0 = Ck::now();
          if (gpu_->run(p, all, out.data(), std::size_t(all.w()) * 3, tile_) != hal::Status::Ok) { g = 1e30; break; }
          g = std::min(g, ms(t0));
        }
      for (int rep = 0; rep < (k == 0 ? 1 : 2); ++rep) {
        const auto t0 = Ck::now();
        demosaic(p, all, out.data(), std::size_t(all.w()) * 3, 0);
        c = std::min(c, ms(t0));
      }
      v.ms[k * 2] = g < 1e29 ? g : -1.0, v.ms[k * 2 + 1] = c;
      (k == 0 ? v.full : v.region) = hf ? g * 1.3 < c : g < c;  // half-resolution path: the GPU must win clearly (the probe is small, the real frame adds read-back and window upload per tile)
    }
    v.note = noteOf(v, "");
    logLine(std::string("verdict frame ") + (v.full ? "GPU" : "CPU") + ", region " + (v.region ? "GPU" : "CPU") + ": " + v.note);
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "%d %d %.2f %.2f %.2f %.2f\n", v.full ? 1 : 0, v.region ? 1 : 0, v.ms[0], v.ms[1], v.ms[2], v.ms[3]);
        std::fclose(f);
      }
    }
    return v;
  }
  std::map<int, Verdict> verdicts_;

  bool ensure(std::string& why) {
    if (gpu_) return true;
    if (!failed_.empty()) return why = failed_, false;
    hal::DeviceOptions o;
    hal::Selection sel = hal::selectDevice(o);
    if (!sel.device) {
      for (const hal::Attempt& a : sel.attempts) failed_ += (failed_.empty() ? "" : "; ") + a.detail;
      if (failed_.empty()) failed_ = "no GPU device";
      return why = failed_, false;
    }
    dev_ = std::move(sel.device);
    std::string err;
    gpu_ = GpuDemosaic::create(*dev_, true, err);
#if defined(__ANDROID__) || defined(__APPLE__)
    tile_ = 512;  // planes of a tile stay small on a phone (candidate planes are 68 B per pixel)
#endif
    return true;
  }
  std::mutex mu_;
  std::unique_ptr<hal::Device> dev_;
  std::unique_ptr<GpuDemosaic> gpu_;
  std::string failed_;
  int tile_ = 1024;
};
}  // namespace

void installGpuBackend() {
  static DeviceBackend backend;
  setGpuBackend(&backend);
}

}  // namespace dapple::demosaic
