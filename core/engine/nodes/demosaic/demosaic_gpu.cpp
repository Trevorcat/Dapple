#include "engine/nodes/demosaic/demosaic_gpu.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

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
  hal::BufferH staging, mosaic, tab, S, G, D, readback;
  hal::ImageH out;
  hal::PipelineH pScale, pG, pC, pO;
  Cfa cfa = Cfa::Bayer;
  void release() {
    for (hal::BufferH* b : {&staging, &mosaic, &tab, &S, &G, &D, &readback})
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
  const int ww = tile + 2 * H, wh = tile + 2 * H;
  // (re)allocate for this tile size / CFA
  if (P.tile != tile || P.cfa != d.cfa || P.period != per) {
    dev.waitIdle();
    P.release();
    const std::uint64_t planeBytes = std::uint64_t(ww) * wh * 4;
    const std::uint64_t winBytes = (std::uint64_t(ww) * wh + 1) / 2 * 4;
    auto mk = [&](std::uint64_t bytes, hal::MemKind k, bool storage, hal::BufferH* b) { return dev.createBuffer({bytes, k, storage}, b); };
    Status s = mk(winBytes, hal::MemKind::Upload, true, &P.staging);
    if (s == Status::Ok) s = mk(winBytes, hal::MemKind::DeviceLocal, true, &P.mosaic);
    if (s == Status::Ok) s = mk((40 + 4096) * 4, hal::MemKind::Upload, true, &P.tab);
    if (s == Status::Ok) s = mk(planeBytes, hal::MemKind::DeviceLocal, true, &P.S);
    if (s == Status::Ok) s = mk(planeBytes, hal::MemKind::DeviceLocal, true, &P.G);
    if (s == Status::Ok && d.cfa == Cfa::XTrans) s = mk(std::uint64_t(tile + 2) * (tile + 2) * 8, hal::MemKind::DeviceLocal, true, &P.D);
    const hal::Format of = P.half ? hal::Format::RGBA16F : hal::Format::RGBA32F;
    if (s == Status::Ok) s = dev.createImage({of, std::uint32_t(tile), std::uint32_t(tile), 1}, &P.out);
    if (s == Status::Ok) s = mk(std::uint64_t(tile) * tile * hal::bytesPerTexel(of), hal::MemKind::Readback, false, &P.readback);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      P.release();
      return s;
    }
    S_.deviceBytes = planeBytes * 2 + winBytes + std::uint64_t(tile) * tile * hal::bytesPerTexel(of) + (d.cfa == Cfa::XTrans ? std::uint64_t(tile + 2) * (tile + 2) * 8 : 0);
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
    if (s == Status::Ok) s = pipe(bay ? "dm_bg" : "dm_xg", &P.pG, false);
    if (s == Status::Ok && !bay) s = pipe("dm_xc", &P.pC, false);
    if (s == Status::Ok) s = pipe(bay ? (P.half ? "dm_bo16" : "dm_bo32") : (P.half ? "dm_xo16" : "dm_xo32"), &P.pO, false);
    if (s != Status::Ok) {
      err_ = dev.lastError();
      return s;
    }
    P.tile = tile, P.cfa = d.cfa, P.period = per;
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
      const Rect t{tx, ty, std::min(tx + tile, roi.x1), std::min(ty + tile, roi.y1)};
      const int rw = t.w(), rh = t.h(), cw = rw + 2 * H, ch = rh + 2 * H;
      auto t0 = Clock::now();
      // window of the mosaic, coordinate mapped
      std::uint16_t* win = static_cast<std::uint16_t*>(P.staging.mapped);
      const int wx0 = t.x0 - H, wy0 = t.y0 - H;
      const bool xin = wx0 >= 0 && wx0 + cw <= d.width;
      for (int i = 0; i < cw; ++i) xs[std::size_t(i)] = mapCoord(wx0 + i, d.width, d.cfa);
      for (int j = 0; j < ch; ++j) {
        const std::uint16_t* srow = d.data + std::size_t(mapCoord(wy0 + j, d.height, d.cfa)) * d.width;
        std::uint16_t* drow = win + std::size_t(j) * cw;
        if (xin) std::memcpy(drow, srow + wx0, std::size_t(cw) * 2);
        else
          for (int i = 0; i < cw; ++i) drow[i] = srow[xs[std::size_t(i)]];
      }
      const std::uint64_t winBytes = (std::uint64_t(cw) * ch + 1) / 2 * 4;
      dev.flush(P.staging, 0, winBytes);
      U u{};
      u.dims[0] = cw, u.dims[1] = ch, u.dims[2] = rw, u.dims[3] = rh;
      u.org[0] = pmod(wx0, per), u.org[1] = pmod(wy0, per), u.org[2] = pmod(wx0, d.blackCols), u.org[3] = pmod(wy0, d.blackRows);
      u.io[0] = cw, u.io[1] = 0, u.io[2] = 0;
      std::memcpy(u.cfa, cfaBits, 16);
      for (int c = 0; c < 4; ++c) u.wb[c] = d.wb[c];
      u.sc[0] = d.invSpan, u.sc[1] = d.clampOut ? 1.f : 0.f;
      for (int c = 0; c < 3; ++c) u.m0[c] = d.m[c], u.m1[c] = d.m[3 + c], u.m2[c] = d.m[6 + c];
      S_.prepMs += msSince(t0);
      t0 = Clock::now();
      auto cl = dev.begin();
      if (!cl) {
        err_ = dev.lastError();
        return Status::DeviceLost;
      }
      auto disp = [&](hal::PipelineH ph, std::initializer_list<hal::BufferH> bufs, hal::ImageH* img, int gw, int gh) {
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

}  // namespace dapple::demosaic
