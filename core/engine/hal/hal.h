#pragma once
// Compute HAL (docs/engine/pipeline-architecture.md 6.2). Compute only: buffers, images, pipelines, bindings, dispatch,
// barriers, fences, readback. The Vulkan backend lives in hal/vk; GLES 3.1 / Metal map onto the same interface.
//
// Differences from the sketch in 6.2 (all additive, decided while extracting the Vulkan backend):
//  - handles are small value types that carry the metadata the scheduler needs (id, size, format), so the scheduler never
//    calls back into the device to ask "what is this";
//  - a CommandList owns one frame slot (command buffer, fence, uniform ring, timestamp pool); `Device::begin()` waits for
//    the slot's previous submission, `submit()` returns a FenceH, `slot()` lets the executor index its own per-slot
//    staging / readback buffers (the slot is idle once begin() returned);
//  - `CommandList::flushPart()` submits what is recorded so far without a fence and goes on recording (mobile GPUs reset
//    on submissions of a few seconds, pixel-architecture.md 16 rule 1); the scheduler decides where.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dapple::hal {

enum class Api : std::uint8_t { Vulkan, Gles31, Metal, Null };
enum class Status : std::uint8_t { Ok, OutOfMemory, DeviceLost, Unsupported, Timeout, InvalidArgument, Internal };
enum class Format : std::uint8_t { R8, RGBA8, R16F, RGBA16F, R32F, RG32F, RGBA32F, R32UI, Count };
enum class MemKind : std::uint8_t { DeviceLocal, Upload, Readback };
enum class DeviceState : std::uint8_t { Ok, Lost };
constexpr int kFormatCount = int(Format::Count);

inline const char* statusName(Status s) {
  static const char* n[] = {"ok", "out of memory", "device lost", "unsupported", "timeout", "invalid argument", "internal"};
  return n[int(s)];
}
std::uint32_t bytesPerTexel(Format f);

struct FormatCaps {
  bool sampled = false, filterable = false, storageRead = false, storageWrite = false, storageReadWrite = false;
};

struct DeviceCaps {
  Api api = Api::Null;
  std::uint32_t apiVersion = 0, driverVersion = 0, vendorId = 0, deviceId = 0, deviceType = 0;
  std::string name, driver;
  std::string memoryTypes;  // diagnostics: one line per memory type
  bool software = false;    // SwiftShader / llvmpipe
  std::uint32_t maxImage2D = 0, maxImage3D = 0, maxWorkgroupInvocations = 0, maxSharedBytes = 0, uniformOffsetAlign = 256;
  std::uint32_t maxUniformBytes = 256;
  std::uint64_t deviceLocalBytes = 0, maxBufferBytes = 0;
  bool unifiedMemory = false;
  FormatCaps format[kFormatCount];
  bool timestamps = false, specConstants = true, programBinaryCache = true;
  bool fp16Arithmetic = false, storage16 = false, subgroups = false;
  std::uint32_t subgroupSize = 0, subgroupOps = 0;
  bool linearFilterRgba32f = false;
  bool externalMemoryImport = false, surfacePresent = false;
  bool externalImageWrite = false;  // CommandList::copyImageToExternal works (Vulkan)
  // vendor:device:driver (blocklist / self-test cache key)
  std::string key() const;
  std::string text() const;
};

struct BufferH {
  std::uint64_t id = 0;
  void* impl = nullptr;
  std::uint64_t bytes = 0;
  void* mapped = nullptr;  // Upload / Readback buffers, persistent
  explicit operator bool() const { return impl != nullptr; }
};
struct ImageH {
  std::uint64_t id = 0;
  void* impl = nullptr;
  Format fmt = Format::RGBA16F;
  std::uint32_t w = 0, h = 0, d = 1;
  std::uint64_t bytes = 0;
  explicit operator bool() const { return impl != nullptr; }
};
struct PipelineH {
  void* impl = nullptr;
  const char* bindings = nullptr;  // signature after the uniform block: I storage image, S sampled image, B storage buffer
  explicit operator bool() const { return impl != nullptr; }
};
struct FenceH {
  std::uint32_t slot = 0xFFFFFFFFu;
  std::uint64_t gen = 0;
  explicit operator bool() const { return slot != 0xFFFFFFFFu; }
};

struct BufferDesc { std::uint64_t bytes = 0; MemKind kind = MemKind::DeviceLocal; bool storage = true; };
struct ImageDesc { Format fmt = Format::RGBA16F; std::uint32_t w = 1, h = 1, d = 1; };
struct SpecConst { std::uint32_t id; std::int32_t value; };
// shader = name in the embedded table; the binding signature (I storage image, S sampled image, B storage buffer, in
// binding order after the uniform block) comes from the table and is checked against `bindings` when that is not empty.
struct PipelineDesc {
  std::string shader;
  std::vector<SpecConst> spec;
  std::string bindings;
};

struct Rect2 { std::uint32_t x = 0, y = 0, w = 0, h = 0; };

// ---- zero-readback seam (stage d, docs/engine/present.md) ------------------------------------------------------------------
// An image created and owned by someone else (the presenter's swapchain image) that the final pass of a frame is copied into.
// Vulkan: `handle` is a VkImage of format R8G8B8A8_UNORM (or any 32 bit format with the same texel size) created with
// TRANSFER_DST usage on THIS device (see Device::nativeHandles) and a queue family sharing; the commands bring it from
// `layoutIn` to TRANSFER_DST_OPTIMAL for the copy and to `layoutOut` afterwards (values of VkImageLayout; 0 = UNDEFINED /
// 1000001002 = PRESENT_SRC_KHR for a swapchain image). `waitSemaphore` (a VkSemaphore, e.g. the acquire semaphore; 0 = none) is
// waited for before the copy, `signalSemaphore` (the render-finished semaphore the present waits on; 0 = none) is signalled
// after it, both by the submission that ends the frame. Unsupported on backends without caps.externalImageWrite (GLES).
struct ExternalImage {
  void* handle = nullptr;
  Format fmt = Format::RGBA8;
  std::uint32_t w = 0, h = 0;
  std::uint32_t layoutIn = 0, layoutOut = 0;
  std::uint64_t waitSemaphore = 0, signalSemaphore = 0;
};
// The handles of the device behind a Device, so a presenter can adopt it instead of creating a second one (adopted-device
// mode): the swapchain, the semaphores and the external images must live on the same VkDevice.
struct NativeHandles {
  void* instance = nullptr;          // VkInstance
  void* physicalDevice = nullptr;    // VkPhysicalDevice
  void* device = nullptr;            // VkDevice
  void* queue = nullptr;             // VkQueue (graphics + compute + transfer family used by the HAL; serialise with the device)
  std::uint32_t queueFamily = 0;
  void* getInstanceProcAddr = nullptr;  // PFN_vkGetInstanceProcAddr of the loader the device was created through
  bool swapchainEnabled = false;     // VK_KHR_swapchain (+ VK_KHR_surface / VK_KHR_android_surface) were enabled at creation
};

// Barrier classes, derived by the scheduler from RAW / WAR / WAW analysis (consumer oriented).
enum Barrier : std::uint32_t {
  kImgStoreToImgLoad = 1, kImgStoreToSampled = 2, kBufWriteToBufRead = 4, kComputeToTransfer = 8, kTransferToCompute = 16,
  kComputeToHost = 32, kAll = 63
};

struct PassTime {
  std::string name;
  double ms = 0;
};
struct SubmitStats {
  int passes = 0, barriers = 0, parts = 1;  // dispatches+copies recorded, barriers emitted, submissions it was cut into
};

class CommandList {
 public:
  virtual ~CommandList() = default;
  virtual int slot() const = 0;  // frame slot index [0, Device::kSlots)
  virtual void bindPipeline(PipelineH) = 0;
  // slots follow the binding order of the pipeline (slot 0 = first resource after the uniform block)
  virtual void bind(std::uint32_t slot, BufferH) = 0;
  virtual void bind(std::uint32_t slot, ImageH) = 0;
  virtual Status setParams(const void* std140, std::size_t bytes) = 0;  // <= 256 B per dispatch, ring allocated
  virtual Status dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) = 0;
  virtual void barrier(std::uint32_t mask) = 0;
  virtual void copyBufferToImage(BufferH, ImageH, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t srcOffset) = 0;
  virtual void copyBufferToBuffer(BufferH src, BufferH dst, std::uint64_t bytes, std::uint64_t srcOffset) = 0;
  virtual void copyImageToBuffer(ImageH, BufferH, const Rect2&, std::uint64_t dstOffset) = 0;
  virtual void copyImage(ImageH, ImageH, std::uint32_t w, std::uint32_t h) = 0;
  virtual void timestamp(const char* label) = 0;  // no-op unless profiling is on and caps.timestamps
  // Submit what has been recorded and continue in a fresh buffer of the same slot (no fence).
  // Copies the top-left w x h of `src` (an RGBA8 image) into an externally owned image (see ExternalImage) and attaches its
  // semaphores to the submission that ends the command list. Unsupported where caps.externalImageWrite is false.
  virtual Status copyImageToExternal(ImageH src, const ExternalImage& dst, std::uint32_t w, std::uint32_t h) { (void)src; (void)dst; (void)w; (void)h; return Status::Unsupported; }
  virtual Status flushPart() = 0;
  virtual Status status() const = 0;  // first error of the recording (sticky)
};

struct DeviceOptions {
  bool allowSoftware = false;
  int deviceIndex = -1;
  bool profile = false;
  std::string blocklist;
  std::string pipelineCachePath;
  std::size_t poolBudgetBytes = 0;
  // Enable VK_KHR_surface + VK_KHR_android_surface (win32 on the desktop) at instance creation and VK_KHR_swapchain at device
  // creation when the implementation offers them, so a presenter can adopt the device (Device::nativeHandles).
  bool presentExtensions = false;
  // Weighted workgroups per submission before the scheduler splits a frame (0 = device default, env
  // DAPPLE_GPU_SPLIT_GROUPS overrides, "0" there = never split).
  double splitGroups = 0;
};

class Device {
 public:
  static constexpr int kSlots = 3;
  virtual ~Device() = default;
  virtual const DeviceCaps& caps() const = 0;
  virtual DeviceState state() const = 0;
  virtual const std::string& lastError() const = 0;

  virtual Status createBuffer(const BufferDesc&, BufferH*) = 0;
  virtual Status createImage(const ImageDesc&, ImageH*) = 0;
  virtual Status createPipeline(const PipelineDesc&, PipelineH*) = 0;  // cached per (shader, spec)
  virtual void destroy(BufferH&) = 0;
  virtual void destroy(ImageH&) = 0;
  // 1x1 image for bindings a shader variant never touches
  virtual ImageH dummyImage(Format) = 0;
  virtual void flush(BufferH, std::uint64_t off, std::uint64_t n) = 0;
  virtual void invalidate(BufferH, std::uint64_t off, std::uint64_t n) = 0;

  // Waits for the slot's previous work; null on timeout / device loss. `pinned` slots are skipped (their readback
  // buffer is still being consumed by the host).
  virtual std::unique_ptr<CommandList> begin() = 0;
  virtual void pinSlot(int slot) = 0;  // -1 = none
  virtual Status submit(CommandList&, FenceH* signal) = 0;
  virtual Status wait(FenceH, std::uint64_t timeoutNs) = 0;
  virtual bool poll(FenceH) = 0;
  virtual Status waitAll() = 0;  // every submitted slot finished
  virtual void waitIdle() = 0;
  // timestamps of the finished submission behind the fence (profile only)
  virtual const std::vector<PassTime>& passTimes(FenceH) const = 0;
  virtual SubmitStats stats(FenceH) const = 0;
  virtual double gpuAccumMs() const = 0;

  // Handles of the underlying device for the presenter (adopted-device mode). False where there is no such thing (GLES).
  virtual bool nativeHandles(NativeHandles* out) const { (void)out; return false; }
  // The VkImage behind an image this device created (tests of the external-image seam; a presenter normally owns its images).
  virtual bool nativeImage(const ImageH& im, void** vkImage) const { (void)im; (void)vkImage; return false; }
  virtual std::vector<std::uint8_t> pipelineCacheBlob() const = 0;
  virtual void trim() = 0;  // release descriptor caches etc.
  virtual double splitGroups() const = 0;
  virtual std::size_t poolBudget() const = 0;
};

// Null + `err` set when there is no usable device.
std::unique_ptr<Device> createDevice(Api, const DeviceOptions&, std::string& err);

// blocklist (same text format on every backend, docs 6.6)
struct BlockRule {
  std::uint32_t vendor = 0;
  std::string name, driver;
  std::uint32_t driverMin = 0, driverMax = 0xffffffffu;
  std::string reason;
};
std::vector<BlockRule> parseBlocklist(const std::string& text);
std::string blockReason(const DeviceCaps& di, const std::vector<BlockRule>& rules, bool allowSoftware);

}  // namespace dapple::hal
