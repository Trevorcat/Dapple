#pragma once
// GpuScheduler: backend neutral pass graph on top of the HAL (docs/engine/pipeline-architecture.md 5.2, step b1).
//
// Node record functions append passes to a GpuGraph (dispatches with resource uses, copies); `submitGraph` then
//  - orders the passes into dependency levels (independent chains adjacent, so one barrier serves a whole level),
//  - emits a barrier only when a real RAW / WAR / WAW dependency exists, as a class mask (hal::Barrier),
//  - draws transient images from the pool when their first user is recorded and gives them back after their last use,
//  - cuts the recording into several submissions when the estimated work per submission is too high (mobile GPUs
//    reset on submissions of seconds, pipeline-architecture.md 16 rule 1).
// Same algorithm as phase 1's vk::Graph / Engine::submit, expressed on HAL handles.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "engine/hal/hal.h"

namespace dapple::engine::gpu {

using hal::BufferH;
using hal::Format;
using hal::ImageH;
using hal::PipelineH;

// Pooled working images keyed by (format, size); a steady-state frame allocates nothing.
class GpuPool {
 public:
  explicit GpuPool(hal::Device& d) : d_(d) {}
  ~GpuPool();
  GpuPool(const GpuPool&) = delete;
  GpuPool& operator=(const GpuPool&) = delete;
  hal::Status acquire(Format f, std::uint32_t w, std::uint32_t h, ImageH* out);
  void release(const ImageH& im);
  std::size_t pooledBytes() const { return pooled_; }
  std::size_t freeBytes() const;
  // Destroys the least recently used free images beyond `keepFree` bytes (waits for the device: a free image may still
  // be referenced by the last submission).
  void trim(std::size_t keepFree);
  // allocation size for a requested dimension: a multiple of 32 (128 above 1024), within the device limit
  std::uint32_t bucket(std::uint32_t v) const;
  void clear();  // device rebuilt / lost: forget everything without touching the (dead) device

 private:
  struct E {
    ImageH im;
    std::uint64_t lastUse = 0;
    bool free = false;
  };
  hal::Device& d_;
  std::vector<E> all_;
  std::size_t pooled_ = 0;
  std::uint64_t tick_ = 0;
};

using H = int;  // resource handle inside a graph

class GpuGraph {
 public:
  explicit GpuGraph(hal::Device& d, GpuPool& p) : dev_(d), pool_(p) {}
  H external(const ImageH& im);
  H externalBuf(const BufferH& b);
  // Pooled working image of (at least) w x h; the allocation is rounded up (or aw x ah when given) so frames of slightly
  // different size share pool entries. Kernels take the real size from their parameters.
  H transient(Format f, std::uint32_t w, std::uint32_t h, std::uint32_t aw = 0, std::uint32_t ah = 0);
  const ImageH& image(H h) const { return res_[std::size_t(h)].img; }

  struct Use {
    H res;
    bool write = false;
    bool read = true;
  };
  static Use R(H h) { return {h, false, true}; }
  static Use W(H h) { return {h, true, false}; }
  static Use RW(H h) { return {h, true, true}; }

  // `uses` in binding order (hal slot 0...); params go to the uniform block.
  void dispatch(const char* name, PipelineH pl, std::initializer_list<Use> uses, std::uint32_t gx, std::uint32_t gy, const void* params,
                std::size_t paramBytes, std::uint32_t gz = 1, double weight = 1.0);
  void copyBufferToImage(const BufferH& src, H dst, std::uint32_t w, std::uint32_t h, std::uint32_t d = 1, std::uint64_t srcOffset = 0);
  void copyBufferToBuffer(const BufferH& src, H dst, std::uint64_t bytes, std::uint64_t srcOffset = 0);
  void copyImageToBuffer(H src, const BufferH& dst, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
  void copyImage(H src, H dst, std::uint32_t w, std::uint32_t h);
  // Final copy into an image owned by the presenter (HAL ExternalImage): no readback. The caller keeps `ext` alive until submit.
  void copyImageToExternal(H src, const hal::ExternalImage& ext, std::uint32_t w, std::uint32_t h);
  std::size_t passCount() const { return nodes_.size(); }

 private:
  friend hal::Status submitGraph(GpuGraph&, hal::CommandList&, std::string&);
  struct Res {
    ImageH img;
    BufferH buf;
    bool pooled = false;
    bool hasImg = false, hasBuf = false;
    Format fmt = Format::RGBA16F;
    std::uint32_t w = 0, h = 0;
    int last = -1;
  };
  struct Node {
    enum Kind { Dispatch, BufToImg, BufToBuf, ImgToBuf, ImgToImg, ImgToExternal } kind = Dispatch;
    hal::ExternalImage ext;
    std::string name;
    PipelineH pl;
    std::vector<Use> uses;
    std::uint32_t gx = 0, gy = 0, gz = 1;
    double weight = 1.0;
    std::vector<std::uint8_t> params;
    H a = -1, b = -1;
    BufferH buf;
    std::uint32_t x = 0, y = 0, w = 0, h = 0, d = 1;
    std::uint64_t bytes = 0, off = 0;
  };
  hal::Device& dev_;
  GpuPool& pool_;
  std::vector<Res> res_;
  std::vector<Node> nodes_;
};

// Records the graph into `cl` (which the caller obtained from Device::begin() and submits afterwards with
// Device::submit). On failure `err` is set and the status is the first error.
hal::Status submitGraph(GpuGraph& g, hal::CommandList& cl, std::string& err);

// DAPPLE_GPU_PASSLOG=1: one entry per recorded pass of the last submitGraph, in execution order (aligned with the timestamps).
struct PassLog {
  std::string name, desc;  // desc: the bound images as <R|W|M>WxHxBytesPerTexel
  double rdMB = 0, wrMB = 0, groups = 0;
};
const std::vector<PassLog>& lastPassLog();

// Convenience: record + submit + (optionally) wait.
hal::Status runGraph(hal::Device& dev, GpuGraph& g, hal::CommandList& cl, hal::FenceH* fence, std::string& err);

}  // namespace dapple::engine::gpu
