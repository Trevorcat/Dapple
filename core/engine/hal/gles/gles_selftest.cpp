// GLES backend self-test (docs/engine/pipeline-architecture.md 6.6): one numeric case through the real HAL path
// (upload buffer -> convert.comp -> RGBA16F image -> readback pass -> host) + one build per embedded program.
#include <chrono>
#include <cmath>
#include <cstring>

#include "engine/hal/gles/gles_device.h"

namespace dapple::hal::gles {

namespace {
float halfToFloat(std::uint16_t h) {
  const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(float(m), -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp(float(m + 1024), int(e) - 25);
  return s ? -v : v;
}
// the shader's h16(): round to nearest even on the 10 bit mantissa
float h16(float x) {
  std::uint32_t u;
  std::memcpy(&u, &x, 4);
  u += 0xFFFu + ((u >> 13) & 1u);
  u &= ~0x1FFFu;
  std::memcpy(&x, &u, 4);
  return x;
}
}  // namespace

SelfTestReport selfTest(Device& dev) {
  const auto t0 = std::chrono::steady_clock::now();
  SelfTestReport r;
  const std::uint32_t W = 37, H = 21;  // not multiples of the group size: exercises the bounds guard
  BufferH src, dst;
  ImageH img;
  PipelineH pipe;
  std::string why;
  auto fin = [&] {
    dev.destroy(src);
    dev.destroy(dst);
    dev.destroy(img);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
  };
  if (dev.createBuffer({W * H * 3 * 4, MemKind::Upload, true}, &src) != Status::Ok ||
      dev.createBuffer({W * H * 8, MemKind::Readback, true}, &dst) != Status::Ok || dev.createImage({Format::RGBA16F, W, H, 1}, &img) != Status::Ok) {
    r.text = "self-test: resource creation failed: " + dev.lastError();
    return fin();
  }
  auto* in = static_cast<float*>(src.mapped);
  for (std::uint32_t i = 0; i < W * H * 3; i++) in[i] = float((i * 2654435761u) % 4099u) / 4099.0f;
  dev.flush(src, 0, W * H * 3 * 4);
  PipelineDesc pd;
  pd.shader = "convert";
  pd.bindings = "BI";
  if (dev.createPipeline(pd, &pipe) != Status::Ok) {
    r.text = "self-test: convert program failed: " + dev.lastError();
    return fin();
  }
  auto cl = dev.begin();
  if (!cl) {
    r.text = "self-test: begin() failed";
    return fin();
  }
  cl->bindPipeline(pipe);
  cl->bind(0, src);
  cl->bind(1, img);
  const std::int32_t ps[4] = {std::int32_t(W), std::int32_t(H), 0, 0};
  cl->setParams(ps, sizeof ps);
  cl->dispatch((W + 15) / 16, (H + 15) / 16, 1);
  cl->barrier(kImgStoreToSampled | kImgStoreToImgLoad | kComputeToTransfer);
  cl->copyImageToBuffer(img, dst, Rect2{0, 0, W, H}, 0);
  FenceH f;
  if (dev.submit(*cl, &f) != Status::Ok || dev.wait(f, 10ull * 1000 * 1000 * 1000) != Status::Ok) {
    r.text = "self-test: submit/wait failed: " + dev.lastError();
    return fin();
  }
  dev.invalidate(dst, 0, W * H * 8);
  const auto* h = static_cast<const std::uint16_t*>(dst.mapped);
  double worst = 0;
  std::uint32_t bad = 0;
  for (std::uint32_t y = 0; y < H; y++)
    for (std::uint32_t x = 0; x < W; x++) {
      const std::uint32_t i = 3 * (y * W + x);
      for (int c = 0; c < 4; c++) {
        const float want = c < 3 ? h16(in[i + std::uint32_t(c)]) : 1.0f;
        const float got = halfToFloat(h[4 * (y * W + x) + std::uint32_t(c)]);
        const double d = std::fabs(double(want) - double(got));
        worst = std::max(worst, d);
        if (!(d <= 1e-6)) bad++;  // exact: the shader rounds explicitly, whatever the driver's float -> half mode
      }
    }
  char b[160];
  std::snprintf(b, sizeof b, "numeric case: %u of %u values off, worst %.3g", bad, W * H * 4, worst);
  r.text = b;
  if (bad) return fin();
  // buffer -> device-local buffer -> readback buffer (the LUT / tone table upload path of the executor)
  {
    const std::uint32_t N = 30728;
    BufferH up, mid, rb;
    bool ok = dev.createBuffer({1u << 20, MemKind::Upload, true}, &up) == Status::Ok && dev.createBuffer({N, MemKind::DeviceLocal, true}, &mid) == Status::Ok &&
              dev.createBuffer({N, MemKind::Readback, true}, &rb) == Status::Ok;
    bool match = false;
    if (ok) {
      auto* p = static_cast<std::uint8_t*>(up.mapped);
      for (std::uint32_t i = 0; i < N; i++) p[i] = std::uint8_t(i * 31u + 7u);  // no flush(): the Vulkan backend's host-coherent contract
      auto c2 = dev.begin();
      c2->copyBufferToBuffer(up, mid, N, 0);
      c2->barrier(kComputeToTransfer | kTransferToCompute);
      c2->copyBufferToBuffer(mid, rb, N, 0);
      c2->barrier(kComputeToHost);
      FenceH f2;
      if (dev.submit(*c2, &f2) == Status::Ok && dev.wait(f2, 10ull * 1000 * 1000 * 1000) == Status::Ok) {
        dev.invalidate(rb, 0, N);
        match = std::memcmp(up.mapped, rb.mapped, N) == 0;
      }
    }
    dev.destroy(up);
    dev.destroy(mid);
    dev.destroy(rb);
    r.text += std::string("\nbuffer copy chain: ") + (ok && match ? "ok" : "FAILED");
    if (!ok || !match) {
      r.failed.push_back("buffer-copy");
      return fin();
    }
  }
  // every program, default variant, isolated
  for (const std::string& n : shaderNames()) {
    PipelineH p;
    PipelineDesc d;
    d.shader = n;
    if (dev.createPipeline(d, &p) != Status::Ok) {
      r.failed.push_back(n);
      r.text += "\n  program " + n + ": " + dev.lastError();
    }
  }
  r.ok = true;
  return fin();
}

}  // namespace dapple::hal::gles
