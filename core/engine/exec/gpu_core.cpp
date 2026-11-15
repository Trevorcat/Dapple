// gpu::Core: the device, its transient pool, the per-frame-slot host buffers, and the deferred destruction of device resources (one per device
// generation; owned by the Context).
#include "engine/exec/gpu_executor.h"

#include <algorithm>
#include <cstring>

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

std::unique_ptr<Core> Core::create(const hal::DeviceOptions& opt, std::string& err) {
  std::unique_ptr<Core> c(new Core);
#if defined(DAPPLE_HAL_METAL)
  c->own_ = hal::createDevice(hal::Api::Metal, opt, err);
#else
  c->own_ = hal::createDevice(hal::Api::Vulkan, opt, err);
#endif
  if (!c->own_) return nullptr;
  c->dev_ = c->own_.get();
  return c->init(err) ? std::move(c) : nullptr;
}
std::unique_ptr<Core> Core::adopt(std::unique_ptr<hal::Device> dev, std::string& err) {
  std::unique_ptr<Core> c(new Core);
  c->own_ = std::move(dev);
  c->dev_ = c->own_.get();
  return c->init(err) ? std::move(c) : nullptr;
}
std::unique_ptr<Core> Core::view(hal::Device& dev, std::string& err) {
  std::unique_ptr<Core> c(new Core);
  c->dev_ = &dev;
  return c->init(err) ? std::move(c) : nullptr;
}

bool Core::init(std::string& err) {
  pool_ = std::make_unique<GpuPool>(*dev_);
  for (int i = 0; i < hal::Device::kSlots; ++i) {
    if (dev_->createBuffer({16384, hal::MemKind::Upload, true}, &slots_[i].weights) != hal::Status::Ok ||
        dev_->createBuffer({256u << 10, hal::MemKind::Upload, true}, &slots_[i].taps) != hal::Status::Ok ||
        dev_->createBuffer({8u << 20, hal::MemKind::Upload, true}, &slots_[i].staging) != hal::Status::Ok)
      return err = dev_->lastError(), false;
  }
  if (dev_->createBuffer({256, hal::MemKind::DeviceLocal, true}, &gBuf) != hal::Status::Ok ||
      dev_->createBuffer({256 * 1024, hal::MemKind::DeviceLocal, true}, &partBuf) != hal::Status::Ok)
    return err = dev_->lastError(), false;
  return true;
}

Core::~Core() {
  if (!dev_) return;
  dev_->waitIdle();
  for (Retired& r : retired_) {
    if (r.image) dev_->destroy(r.image);
    if (r.buffer) dev_->destroy(r.buffer);
  }
  pool_.reset();
  for (SlotRes& s : slots_) {
    dev_->destroy(s.staging);
    dev_->destroy(s.readback);
    dev_->destroy(s.weights);
    dev_->destroy(s.taps);
  }
  dev_->destroy(gBuf);
  dev_->destroy(partBuf);
}

bool Core::ensureStaging(int slot, std::uint64_t bytes, std::string& err) {
  SlotRes& s = slots_[slot];
  if (s.staging.bytes >= bytes) return true;
  dev_->destroy(s.staging);
  if (dev_->createBuffer({std::max<std::uint64_t>(bytes, 8u << 20), hal::MemKind::Upload, true}, &s.staging) != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}
std::size_t Core::trimStaging() {
  std::size_t freed = 0;
  dev_->waitIdle();
  collect(true);
  for (SlotRes& s : slots_) {
    if (s.readback) freed += std::size_t(s.readback.bytes), dev_->destroy(s.readback), s.readback = hal::BufferH{};
    if (s.staging && s.staging.bytes > (8u << 20)) {
      hal::BufferH small;
      if (dev_->createBuffer({8u << 20, hal::MemKind::Upload, true}, &small) != hal::Status::Ok) continue;  // keep the large one
      freed += std::size_t(s.staging.bytes) - (8u << 20);
      dev_->destroy(s.staging);
      s.staging = small;
    }
  }
  return freed;
}
bool Core::ensureReadback(int slot, std::uint64_t bytes, std::string& err) {
  SlotRes& s = slots_[slot];
  if (s.readback.bytes >= bytes) return true;
  dev_->destroy(s.readback);
  if (dev_->createBuffer({bytes, hal::MemKind::Readback, false}, &s.readback) != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}

Core::Stats Core::statsOf(hal::FenceH f) {
  Stats s;
  if (!f) return s;
  const auto st = dev_->stats(f);
  s.passes = st.passes;
  s.barriers = st.barriers;
  s.parts = st.parts;
  s.times = dev_->passTimes(f);
  for (const auto& t : s.times) s.gpuMs += t.ms;
  return s;
}

bool Core::uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, const hal::ImageH& dst, int dx, int dy, std::string& err,
                        const std::function<bool()>& between) {
  const std::size_t rowBytes = std::size_t(w) * 12;
  const std::size_t chunkBytes = 24u << 20;
  const int rows = int(std::max<std::size_t>(1, std::min<std::size_t>(std::size_t(h), chunkBytes / rowBytes)));
  for (int y = 0; y < h; y += rows) {
    const int n = std::min(rows, h - y);
    auto cl = dev_->begin();
    if (!cl) return err = dev_->lastError().empty() ? "no frame slot" : dev_->lastError(), false;
    if (!ensureStaging(cl->slot(), rowBytes * std::size_t(n), err)) return false;
    SlotRes& s = slots_[cl->slot()];
    auto* dstp = static_cast<std::uint8_t*>(s.staging.mapped);
    if (pitchFloats == std::size_t(w) * 3) {
      std::memcpy(dstp, rgb + std::size_t(y) * pitchFloats, rowBytes * std::size_t(n));
    } else {
      for (int j = 0; j < n; ++j) std::memcpy(dstp + std::size_t(j) * rowBytes, rgb + std::size_t(y + j) * pitchFloats, rowBytes);
    }
    GpuGraph g(*dev_, *pool_);
    std::string e2;
    GpuRecorder rec(*dev_, g, e2);
    const H out = g.external(dst);
    // the convert pass reads the staging buffer: build the dispatch by hand (the uniform carries the chunk placement)
    if (!recordConvert(rec, s.staging, out, w, n, dx, dy + y)) return err = e2, false;
    if (runGraph(*dev_, g, *cl, nullptr, err) != hal::Status::Ok) return false;
    if (between && y + n < h && !between()) return err = "upload stopped between two submissions", false;
  }
  if (dev_->waitAll() != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}
// ---- deferred destruction --------------------------------------------------------------------------------------------------------------------
// A released handle may still be read by work already submitted (another session's frame, the frame in flight). It waits for the first fenced
// submission after its release: once that has completed, every submission before it has (one queue, in order), so nothing can still read it.
void Core::retire(hal::ImageH h) {
  if (!h) return;
  std::lock_guard<std::mutex> lk(retireMtx_);
  retired_.push_back(Retired{h, {}, {}, false});
}
void Core::retire(hal::BufferH h) {
  if (!h) return;
  std::lock_guard<std::mutex> lk(retireMtx_);
  retired_.push_back(Retired{{}, h, {}, false});
}
void Core::submitted(hal::FenceH f) {
  if (!f) return;
  std::lock_guard<std::mutex> lk(retireMtx_);
  for (Retired& r : retired_)
    if (!r.fenced) r.after = f, r.fenced = true;
}
std::size_t Core::collect(bool all) {
  std::vector<Retired> done;
  {
    std::lock_guard<std::mutex> lk(retireMtx_);
    for (auto it = retired_.begin(); it != retired_.end();) {
      if (all || (it->fenced && dev_->poll(it->after))) {
        done.push_back(*it);
        it = retired_.erase(it);
      } else {
        ++it;
      }
    }
  }
  std::size_t bytes = 0;
  for (Retired& r : done) {
    if (r.image) bytes += std::size_t(r.image.bytes), dev_->destroy(r.image);
    if (r.buffer) bytes += std::size_t(r.buffer.bytes), dev_->destroy(r.buffer);
  }
  return bytes;
}
std::size_t Core::retiredCount() {
  std::lock_guard<std::mutex> lk(retireMtx_);
  return retired_.size();
}

}  // namespace dapple::engine::gpu
