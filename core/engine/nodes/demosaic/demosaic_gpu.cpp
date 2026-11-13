#include "engine/nodes/demosaic/demosaic_gpu.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <memory>
#include <mutex>

#include "engine/exec/gpu_runtime.h"
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
  hal::BufferH staging, mosaic, tab, S, G, D, Hh, readback;
  hal::ImageH out;
  hal::PipelineH pScale, pG, pC, pH, pSel, pO;
  Cfa cfa = Cfa::Bayer;
  int xt = 1, extra = 0;
  void release() {
    for (hal::BufferH* b : {&staging, &mosaic, &tab, &S, &G, &D, &Hh, &readback})
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
  const int H = haloOf(d), per = d.period;
  tile = std::max(16, tile);
  const int E = hotExtra(d);  // extra samples of the staged window for the hot-sample filter
  const int ww = tile + 2 * H, wh = tile + 2 * H, wwE = ww + 2 * E, whE = wh + 2 * E;
  // (re)allocate for this tile size / CFA
  const bool cand = d.cfa == Cfa::XTrans && d.xtMode >= 1;
  if (P.tile != tile || P.cfa != d.cfa || P.period != per || P.xt != (cand ? 1 : 0) || P.extra != E) {
    dev.waitIdle();
    P.release();
    const std::uint64_t planeBytes = std::uint64_t(ww) * wh * 4;
    const std::uint64_t winBytes = (std::uint64_t(wwE) * whE + 1) / 2 * 4;
    auto mk = [&](std::uint64_t bytes, hal::MemKind k, bool storage, hal::BufferH* b) { return dev.createBuffer({bytes, k, storage}, b); };
    Status s = mk(winBytes, hal::MemKind::Upload, true, &P.staging);
    if (s == Status::Ok) s = mk(winBytes, hal::MemKind::DeviceLocal, true, &P.mosaic);
    if (s == Status::Ok) s = mk((40 + 4096) * 4, hal::MemKind::Upload, true, &P.tab);
    if (s == Status::Ok) s = mk(planeBytes, hal::MemKind::DeviceLocal, true, &P.S);
    if (s == Status::Ok) s = mk(cand ? planeBytes * 4 : planeBytes, hal::MemKind::DeviceLocal, true, &P.G);  // candidates: 4 greens per pixel
    const std::uint64_t dBytes = cand ? planeBytes * 8 : std::uint64_t(tile + 2) * (tile + 2) * 8;
    if (s == Status::Ok && d.cfa == Cfa::XTrans) s = mk(dBytes, hal::MemKind::DeviceLocal, true, &P.D);  // candidates: red / blue differences x 4 directions
    if (s == Status::Ok && cand) s = mk(planeBytes * 4, hal::MemKind::DeviceLocal, true, &P.Hh);       // candidates: 4 derivatives
    const hal::Format of = P.half ? hal::Format::RGBA16F : hal::Format::RGBA32F;
    if (s == Status::Ok) s = dev.createImage({of, std::uint32_t(tile), std::uint32_t(tile), 1}, &P.out);
    if (s == Status::Ok) s = mk(std::uint64_t(tile) * tile * hal::bytesPerTexel(of), hal::MemKind::Readback, false, &P.readback);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      P.release();
      return s;
    }
    S_.deviceBytes = planeBytes + (cand ? planeBytes * 4 : planeBytes) + winBytes + std::uint64_t(tile) * tile * hal::bytesPerTexel(of) + (d.cfa == Cfa::XTrans ? dBytes : 0) + (cand ? planeBytes * 4 : 0);
    auto pipe = [&](const char* name, hal::PipelineH* ph, bool wantPeriod) {
      hal::PipelineDesc pd;
      pd.shader = name;
      if (wantPeriod) pd.spec.push_back({0, per});
      pd.spec.push_back({16, P.lx});
      pd.spec.push_back({17, P.ly});
      return dev.createPipeline(pd, ph);
    };
    const bool bay = d.cfa == Cfa::Bayer;
    s = pipe("dm_scale", &P.pScale, true);
    if (s == Status::Ok) s = pipe(bay ? "dm_bg" : (cand ? "dm_x0" : "dm_fg"), &P.pG, false);
    if (s == Status::Ok && !bay) s = pipe(cand ? "dm_x1" : "dm_fc", &P.pC, false);
    if (s == Status::Ok && cand) s = pipe("dm_x2", &P.pH, false);
    if (s == Status::Ok && cand) s = pipe("dm_x3", &P.pSel, false);
    if (s == Status::Ok) s = pipe(bay ? (P.half ? "dm_bo16" : "dm_bo32") : cand ? (P.half ? "dm_x4_16" : "dm_x4_32") : (P.half ? "dm_fo16" : "dm_fo32"), &P.pO, false);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      return s;
    }
    P.tile = tile, P.cfa = d.cfa, P.period = per, P.xt = cand ? 1 : 0, P.extra = E;
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
    dev.flush(P.tab, 0, (40 + std::uint64_t(nb)) * 4);
  }
  std::uint32_t cfaBits[4] = {0, 0, 0, 0};
  for (int i = 0; i < per * per; ++i) cfaBits[i >> 4] |= std::uint32_t(d.cfa3[i] & 3) << ((i & 15) * 2);
  std::vector<int> xs(std::size_t(tile) + 2 * H);
  const bool bay = d.cfa == Cfa::Bayer;
  for (int ty = roi.y0; ty < roi.y1; ty += tile)
    for (int tx = roi.x0; tx < roi.x1; tx += tile) {
      if (d.cancelCur && d.cancelCur->load(std::memory_order_relaxed) != d.cancelMine) return Status::Timeout;  // cancelled
      const Rect t{tx, ty, std::min(tx + tile, roi.x1), std::min(ty + tile, roi.y1)};
      const int rw = t.w(), rh = t.h(), cw = rw + 2 * H, ch = rh + 2 * H;
      auto t0 = Clock::now();
      // window of the mosaic, coordinate mapped
      std::uint16_t* win = static_cast<std::uint16_t*>(P.staging.mapped);
      const int wx0 = t.x0 - H, wy0 = t.y0 - H, cwE = cw + 2 * E, chE = ch + 2 * E;
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
      u.sc[0] = d.invSpan, u.sc[1] = d.clampOut ? 1.f : 0.f, u.sc[2] = d.hot ? 1.f : 0.f;
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
      auto disp = [&](hal::PipelineH ph, std::initializer_list<hal::BufferH> bufs, hal::ImageH* img, int gw, int gh) {
        if ((skipMask >> passNo++) & 1) return;
        cl->bindPipeline(ph);
        std::uint32_t slot = 0;
        for (const hal::BufferH& b : bufs) cl->bind(slot++, b);
        if (img) cl->bind(slot++, *img);
        cl->setParams(&u, sizeof u);
        cl->dispatch(std::uint32_t((gw + P.lx - 1) / P.lx), std::uint32_t((gh + P.ly - 1) / P.ly), 1);
        cl->barrier(hal::kAll);
      };
      cl->copyBufferToBuffer(P.staging, P.mosaic, winBytes, 0);
      cl->barrier(hal::kAll);
      disp(P.pScale, {P.mosaic, P.tab, P.S}, nullptr, cw, ch);
      disp(P.pG, {P.S, P.G}, nullptr, cw, ch);
      if (bay) {
        disp(P.pO, {P.S, P.G}, &P.out, rw, rh);
      } else if (cand) {
        disp(P.pC, {P.S, P.G, P.D}, nullptr, cw, ch);
        disp(P.pH, {P.S, P.G, P.D, P.Hh}, nullptr, cw, ch);
        disp(P.pSel, {P.S, P.G, P.D, P.Hh}, nullptr, rw + 2, rh + 2);
        disp(P.pO, {P.D}, &P.out, rw, rh);
      } else {
        disp(P.pC, {P.S, P.G, P.D}, nullptr, rw + 2, rh + 2);
        disp(P.pO, {P.G, P.D}, &P.out, rw, rh);
      }
      if (out) cl->copyImageToBuffer(P.out, P.readback, hal::Rect2{0, 0, std::uint32_t(rw), std::uint32_t(rh)}, 0);
      hal::FenceH f;
      Status s = dev.submit(*cl, &f);
      if (s == Status::Ok) s = dev.wait(f, kWaitNs);
      if (s != Status::Ok) {
        err_ = dev.lastError();
        return s;
      }
      S_.submit = dev.stats(f);
      S_.gpuMs += msSince(t0);
      if (out) {
        t0 = Clock::now();
        const std::uint64_t texel = hal::bytesPerTexel(P.half ? hal::Format::RGBA16F : hal::Format::RGBA32F);
        dev.invalidate(P.readback, 0, std::uint64_t(rw) * rh * texel);
        const unsigned char* src = static_cast<const unsigned char*>(P.readback.mapped);
        for (int y = 0; y < rh; ++y) {
          float* o = out + std::size_t(t.y0 - roi.y0 + y) * strideFloats + std::size_t(t.x0 - roi.x0) * 3;
          if (P.half) {
            const std::uint16_t* r = reinterpret_cast<const std::uint16_t*>(src) + std::size_t(y) * rw * 4;
            for (int x = 0; x < rw; ++x) o[3 * x] = halfToFloat(r[4 * x]), o[3 * x + 1] = halfToFloat(r[4 * x + 1]), o[3 * x + 2] = halfToFloat(r[4 * x + 2]);
          } else {
            const float* r = reinterpret_cast<const float*>(src) + std::size_t(y) * rw * 4;
            for (int x = 0; x < rw; ++x) o[3 * x] = r[4 * x], o[3 * x + 1] = r[4 * x + 1], o[3 * x + 2] = r[4 * x + 2];
          }
        }
        S_.readbackMs += msSince(t0);
      }
      ++S_.tiles;
    }
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
