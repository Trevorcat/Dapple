// Metal backend of the compute HAL (Apple GPUs: macOS arm64, iOS, iOS simulator). docs/ios/metal-backend.md.
//
// Mapping (docs/engine/pipeline-architecture.md 6.3):
//   Buffer      MTLBuffer: Upload / Readback = Shared (unified memory, CPU-mapped, coherent once the command buffer has completed),
//               DeviceLocal = Private.
//   Image       MTLTexture (ShaderRead | ShaderWrite | PixelFormatView), Private.
//   Pipeline    MTLComputePipelineState from the build-time metallib (dapple_shaderc --msl -> xcrun metal -> metallib); GLSL spec
//               constants are function constants with the same id; threads per threadgroup come from the (spec) local size.
//   Binding     setTexture / setSamplerState / setBuffer at the Vulkan binding number of the resource (slot i = binding i + 1),
//               the std140 parameter block is setBytes at buffer(0).
//   Barrier     one serial compute encoder per command-buffer part: dispatches complete in order and see each other's writes, so
//               barrier() only counts; blit copies sit between encoders (hazard tracking of the default resource options).
//   Fence       one MTLCommandBuffer per part, completion handlers count down a per-slot counter (all parts of a slot are waited).
//   Submission  flushPart() commits the current command buffer and goes on in a new one of the same queue.
//   External    copyImageToExternal: id<MTLTexture> (e.g. IOSurface / CVPixelBuffer backed, RGBA8 or BGRA8), blit or swizzle kernel.
// Precision: the metallib is built without fast math (-fno-fast-math), the GLSL sources round explicitly (h16 ...).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/hal/hal.h"
#include "engine/hal/metal/metal_device.h"

namespace dapple::hal::metal {

struct MslBin {
  const char* name;
  const char* msl;  // source (only when built with DAPPLE_METAL_EMBED_SOURCE), else null
  const char* sig;  // I storage image, S sampled image, B storage buffer, in binding order after the uniform block
  int specId[12];
  int specType[12];  // 0 int, 1 uint, 2 bool, 3 float
  long long specDef[12];
  int nspec;
  int lsz[3];
  int wscId[3];  // spec constant ids of the local size, -1 = literal
  unsigned sharedBytes;
};
#include "metal_shader_table.inc"

namespace {

MTLPixelFormat toMtl(Format f) {
  switch (f) {
    case Format::R8: return MTLPixelFormatR8Unorm;
    case Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
    case Format::R16F: return MTLPixelFormatR16Float;
    case Format::RGBA16F: return MTLPixelFormatRGBA16Float;
    case Format::R32F: return MTLPixelFormatR32Float;
    case Format::RG32F: return MTLPixelFormatRG32Float;
    case Format::RGBA32F: return MTLPixelFormatRGBA32Float;
    case Format::R32UI: return MTLPixelFormatR32Uint;
    default: return MTLPixelFormatInvalid;
  }
}

struct MBuffer {
  id<MTLBuffer> buf = nil;
  std::uint64_t id = 0, size = 0;
  void* mapped = nullptr;
};
struct MImage {
  id<MTLTexture> tex = nil;
  std::uint64_t id = 0;
  Format fmt = Format::RGBA16F;
};
struct MPipeline {
  id<MTLComputePipelineState> pso = nil;
  std::string bindings, name;
  MTLSize tg{16, 16, 1};
};

constexpr int kMaxBind = 8;
constexpr int kMaxSegments = 120;  // compute segments per command buffer when profiling (2 samples each, 256 sample buffer)

// One command buffer's pass timing (profile only): the compute encoders of the part, one per labelled pass.
struct PartTimes {
  id<MTLCounterSampleBuffer> sb = nil;
  std::vector<std::string> labels;  // one per timestamp() call, in order
  std::vector<int> segment;         // per label: index of the compute segment it closes, -1 = none (blit / copy)
  int segments = 0;
};

struct Slot {
  std::mutex m;
  std::condition_variable cv;
  int outstanding = 0;     // committed command buffers of this slot not yet completed
  bool inFlight = false;   // submit() happened and wait() has not consumed it
  std::uint64_t gen = 0;
  SubmitStats st;
  std::vector<PassTime> times;
  std::atomic<bool> failed{false};
  std::atomic<int> failCode{0};  // Status of the first failed command buffer
};

class MetalDev;

class MetalCmd final : public CommandList {
 public:
  MetalCmd(MetalDev& d, int slot);
  ~MetalCmd() override;
  int slot() const override { return slot_; }
  void bindPipeline(PipelineH p) override { pl_ = static_cast<MPipeline*>(p.impl); }
  void bind(std::uint32_t s, BufferH b) override {
    if (s < kMaxBind) bound_[s] = {nullptr, static_cast<MBuffer*>(b.impl)};
  }
  void bind(std::uint32_t s, ImageH i) override {
    if (s < kMaxBind) bound_[s] = {static_cast<MImage*>(i.impl), nullptr};
  }
  Status setParams(const void* p, std::size_t n) override;
  Status dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) override;
  void barrier(std::uint32_t) override { ++st_.barriers; }
  void copyBufferToImage(BufferH, ImageH, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t off) override;
  void copyBufferToBuffer(BufferH, BufferH, std::uint64_t bytes, std::uint64_t off) override;
  void copyImageToBuffer(ImageH, BufferH, const Rect2&, std::uint64_t off) override;
  void copyImage(ImageH, ImageH, std::uint32_t w, std::uint32_t h) override;
  Status copyImageToExternal(ImageH src, const ExternalImage& dst, std::uint32_t w, std::uint32_t h) override;
  void timestamp(const char* label) override;
  Status flushPart() override;
  Status status() const override { return err_; }

 private:
  friend class MetalDev;
  void endEncoders();
  id<MTLComputeCommandEncoder> compute();
  id<MTLBlitCommandEncoder> blit();
  void fail(Status s) {
    if (err_ == Status::Ok) err_ = s;
  }

  MetalDev& d_;
  int slot_;
  id<MTLCommandBuffer> cb_ = nil;
  id<MTLComputeCommandEncoder> ce_ = nil;
  id<MTLBlitCommandEncoder> be_ = nil;
  MPipeline* pl_ = nullptr;
  MPipeline* encPl_ = nullptr;
  struct B {
    MImage* img = nullptr;
    MBuffer* buf = nullptr;
  } bound_[kMaxBind];
  std::uint8_t params_[256];
  std::size_t paramBytes_ = 0;
  bool haveParams_ = false;
  SubmitStats st_;
  Status err_ = Status::Ok;
  std::shared_ptr<PartTimes> pt_;  // profile only
  bool segOpen_ = false;           // the open compute encoder is a measured segment
  void newPart();
};

class MetalDev final : public Device {
 public:
  ~MetalDev() override;
  bool init(const DeviceOptions& opt, std::string& err);

  const DeviceCaps& caps() const override { return caps_; }
  DeviceState state() const override { return lost_ ? DeviceState::Lost : DeviceState::Ok; }
  const std::string& lastError() const override { return lastErr_; }
  Status createBuffer(const BufferDesc&, BufferH*) override;
  Status createImage(const ImageDesc&, ImageH*) override;
  Status createPipeline(const PipelineDesc&, PipelineH*) override;
  void destroy(BufferH&) override;
  void destroy(ImageH&) override;
  ImageH dummyImage(Format) override;
  void flush(BufferH, std::uint64_t, std::uint64_t) override {}
  void invalidate(BufferH, std::uint64_t, std::uint64_t) override {}
  std::unique_ptr<CommandList> begin() override;
  void pinSlot(int s) override { pinned_ = s; }
  Status submit(CommandList&, FenceH*) override;
  Status wait(FenceH, std::uint64_t timeoutNs) override;
  bool poll(FenceH) override;
  Status waitAll() override;
  void waitIdle() override;
  const std::vector<PassTime>& passTimes(FenceH f) const override { return slots_[f.slot < kSlots ? f.slot : 0].times; }
  SubmitStats stats(FenceH f) const override { return slots_[f.slot < kSlots ? f.slot : 0].st; }
  double gpuAccumMs() const override { return gpuAccum_.load(); }
  // The device and queue behind this HAL device, for a presenter that shares them (Metal objects are retained by the device; the lease holds the context alive).
  bool nativeHandles(NativeHandles* out) const override {
    if (!out) return false;
    *out = NativeHandles{};
    out->device = (__bridge void*)dev_;
    out->queue = (__bridge void*)queue_;
    out->swapchainEnabled = true;  // "can present": the presenter draws into textures created on this device
    return true;
  }
  std::vector<std::uint8_t> pipelineCacheBlob() const override;
  void trim() override { waitIdle(); }
  double splitGroups() const override { return splitGroups_; }
  std::size_t poolBudget() const override { return budget_; }

  void* nativeDevice() const { return (__bridge void*)dev_; }

 private:
  friend class MetalCmd;
  Status waitSlot(Slot& s, std::uint64_t ns);
  void fail(const std::string& e, Status s) {
    lastErr_ = e;
    if (s == Status::DeviceLost) lost_ = true;
  }
  void commit(MetalCmd& c, id<MTLCommandBuffer> cb, std::shared_ptr<PartTimes> pt);
  id<MTLComputePipelineState> swizzlePso();

  DeviceOptions opt_;
  DeviceCaps caps_;
  id<MTLDevice> dev_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLLibrary> lib_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  id<MTLComputePipelineState> swizzle_ = nil;
  id<MTLCounterSet> tsSet_ = nil;       // timestamp counter set (stage boundary sampling), profile only
  id<MTLBinaryArchive> archive_ = nil;  // persisted pipeline cache
  std::string archivePath_;
  bool archiveDirty_ = false;
  MTLTimestamp cpuT0_ = 0, gpuT0_ = 0;  // calibration of the GPU tick against the CPU clock
  mach_timebase_info_data_t tb_{1, 1};
  double nsPerTick() const;
  std::unordered_map<std::string, std::unique_ptr<MPipeline>> pipelines_;
  std::unordered_map<std::string, id<MTLLibrary>> srcLibs_;
  std::unordered_map<int, MImage*> dummies_;
  std::mutex pipeMtx_;
  Slot slots_[kSlots];
  int nextSlot_ = 0, pinned_ = -1;
  std::atomic<std::uint64_t> nextId_{1};
  std::atomic<bool> lost_{false};
  std::atomic<double> gpuAccum_{0};
  double splitGroups_ = 250000;
  std::size_t budget_ = 0;
  std::string lastErr_;
};

// ---- device ----
bool MetalDev::init(const DeviceOptions& opt, std::string& err) {
  opt_ = opt;
  @autoreleasepool {
#if TARGET_OS_OSX
    NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
    int want = opt.deviceIndex;
    if (const char* e = std::getenv("DAPPLE_GPU_DEVICE")) want = std::atoi(e);
    if (want >= 0 && want < int(all.count)) dev_ = all[want];
    else {
      // prefer a unified-memory (Apple silicon) device, then the system default
      for (id<MTLDevice> d in all)
        if (d.hasUnifiedMemory && !d.isRemovable) { dev_ = d; break; }
      if (!dev_) dev_ = MTLCreateSystemDefaultDevice();
    }
#else
    dev_ = MTLCreateSystemDefaultDevice();
#endif
    if (!dev_) return err = "no Metal device", false;
    queue_ = [dev_ newCommandQueue];
    if (!queue_) return err = "newCommandQueue failed", false;

    caps_.api = Api::Metal;
    caps_.apiVersion = 0;
    caps_.vendorId = 0x106B;
    caps_.deviceId = 0;
    caps_.driverVersion = 0;
    caps_.deviceType = dev_.hasUnifiedMemory ? 1 : 2;  // VkPhysicalDeviceType: integrated / discrete
    caps_.name = dev_.name.UTF8String;
    caps_.driver = "Metal";
    caps_.software = false;
    const bool apple3 = [dev_ supportsFamily:MTLGPUFamilyApple3];
    caps_.maxImage2D = apple3 || [dev_ supportsFamily:MTLGPUFamilyMac2] ? 16384 : 8192;
    caps_.maxImage3D = 2048;
    caps_.maxWorkgroupInvocations = std::uint32_t(std::min<NSUInteger>(dev_.maxThreadsPerThreadgroup.width, 1024));
    caps_.maxSharedBytes = std::uint32_t(dev_.maxThreadgroupMemoryLength);
    caps_.uniformOffsetAlign = 256;
    caps_.maxUniformBytes = 256;
    caps_.deviceLocalBytes = dev_.recommendedMaxWorkingSetSize ? dev_.recommendedMaxWorkingSetSize : (4ull << 30);
    caps_.maxBufferBytes = dev_.maxBufferLength;
    caps_.unifiedMemory = dev_.hasUnifiedMemory;
    caps_.memoryTypes = std::string("Metal: Shared (CPU+GPU, ") + (dev_.hasUnifiedMemory ? "unified" : "managed") + "), Private (GPU)";
    const bool rw2 = dev_.readWriteTextureSupport == MTLReadWriteTextureTier2;
    const bool rw1 = rw2 || dev_.readWriteTextureSupport == MTLReadWriteTextureTier1;
    const bool f32filter = dev_.supports32BitFloatFiltering;
    for (int i = 0; i < kFormatCount - 1; ++i) {
      FormatCaps& c = caps_.format[i];
      const Format f = Format(i);
      c.sampled = c.storageRead = c.storageWrite = true;
      c.filterable = !(f == Format::R32F || f == Format::RG32F || f == Format::RGBA32F || f == Format::R32UI) || f32filter;
      if (f == Format::R32UI) c.filterable = false;
      c.storageReadWrite = f == Format::R32F || f == Format::R32UI ? rw1 : (f == Format::RG32F ? false : rw2);
    }
    caps_.timestamps = false;
    mach_timebase_info(&tb_);
    if (opt.profile && [dev_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
      for (id<MTLCounterSet> cs in dev_.counterSets)
        if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) tsSet_ = cs;
      if (tsSet_) {
        [dev_ sampleTimestamps:&cpuT0_ gpuTimestamp:&gpuT0_];
        caps_.timestamps = true;
      }
    }
    caps_.specConstants = true;
    caps_.programBinaryCache = false;
    caps_.fp16Arithmetic = true;
    caps_.storage16 = true;
    caps_.subgroups = true;
    caps_.subgroupSize = 32;
    caps_.linearFilterRgba32f = f32filter;
    caps_.externalMemoryImport = false;
    caps_.surfacePresent = false;
    caps_.externalImageWrite = true;

    {
      std::vector<BlockRule> rules;
      for (const std::string& extra : {opt.blocklist, std::string(std::getenv("DAPPLE_GPU_BLOCKLIST") ? std::getenv("DAPPLE_GPU_BLOCKLIST") : "")})
        for (BlockRule& b : parseBlocklist(extra)) rules.push_back(std::move(b));
      const std::string why = blockReason(caps_, rules, opt.allowSoftware);
      if (!why.empty()) return err = why, false;
    }
    // shader library
    if (kMetallibSize) {
      NSError* e = nil;
      dispatch_data_t dd = dispatch_data_create(kMetallib, kMetallibSize, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      lib_ = [dev_ newLibraryWithData:dd error:&e];
      if (!lib_) return err = std::string("metallib load failed: ") + (e ? e.localizedDescription.UTF8String : "?"), false;
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [dev_ newSamplerStateWithDescriptor:sd];
#if !TARGET_OS_SIMULATOR  // the simulator's Metal cannot serialize a binary archive (MTLLoader sliceID assertion)
    if (@available(macOS 11.0, iOS 14.0, *)) {
      // persisted pipeline cache (MTLBinaryArchive): the file the caller keeps at opt.pipelineCachePath; a missing / stale / foreign file starts an empty archive
      archivePath_ = opt.pipelineCachePath;
      MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
      NSError* e = nil;
      if (!archivePath_.empty() && [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:archivePath_.c_str()]])
        ad.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archivePath_.c_str()]];
      archive_ = [dev_ newBinaryArchiveWithDescriptor:ad error:&e];
      if (!archive_ && ad.url) {
        ad.url = nil;
        archive_ = [dev_ newBinaryArchiveWithDescriptor:ad error:&e];
      }
    }
#endif

    budget_ = opt.poolBudgetBytes ? opt.poolBudgetBytes
                                  : std::size_t(std::min<std::uint64_t>(std::max<std::uint64_t>(caps_.deviceLocalBytes / 4, 256u << 20),
                                                                         std::uint64_t(caps_.unifiedMemory ? 512u : 1024u) << 20));
    if (opt.splitGroups > 0) splitGroups_ = opt.splitGroups;
    if (const char* sg = std::getenv("DAPPLE_GPU_SPLIT_GROUPS")) splitGroups_ = std::atof(sg) > 0 ? std::atof(sg) : 1e30;
  }
  return true;
}

double MetalDev::nsPerTick() const {
  // GPU ticks -> ns through two (cpu, gpu) timestamp pairs, the first taken at start-up
  MTLTimestamp c1 = 0, g1 = 0;
  [dev_ sampleTimestamps:&c1 gpuTimestamp:&g1];
  if (g1 > gpuT0_ && c1 > cpuT0_) {
    const double cpuNs = double(c1 - cpuT0_);  // MTLDevice sampleTimestamps: both clocks in ns-scale units; the ratio gives ns per GPU tick
    return cpuNs / double(g1 - gpuT0_);
  }
  return 1.0;
}

std::vector<std::uint8_t> MetalDev::pipelineCacheBlob() const {
  std::vector<std::uint8_t> out;
  if (!archive_ || !archiveDirty_) return out;
  @autoreleasepool {
    NSString* tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"dapple_%p.metalarchive", (void*)this]];
    NSError* e = nil;
    if ([archive_ serializeToURL:[NSURL fileURLWithPath:tmp] error:&e]) {
      NSData* d = [NSData dataWithContentsOfFile:tmp];
      if (d) out.assign(static_cast<const std::uint8_t*>(d.bytes), static_cast<const std::uint8_t*>(d.bytes) + d.length);
      [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
    }
  }
  return out;
}

MetalDev::~MetalDev() {
  waitIdle();
  for (auto& kv : dummies_) delete kv.second;
}

Status MetalDev::createBuffer(const BufferDesc& d, BufferH* out) {
  if (d.bytes == 0 || d.bytes > caps_.maxBufferBytes) return fail("buffer size outside the device limits", Status::InvalidArgument), Status::InvalidArgument;
  auto* b = new MBuffer;
  @autoreleasepool {
    const MTLResourceOptions o = d.kind == MemKind::DeviceLocal ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
    b->buf = [dev_ newBufferWithLength:d.bytes options:o];
  }
  if (!b->buf) {
    delete b;
    return fail("newBufferWithLength failed", Status::OutOfMemory), Status::OutOfMemory;
  }
  b->size = d.bytes;
  b->id = nextId_++;
  b->mapped = d.kind == MemKind::DeviceLocal ? nullptr : b->buf.contents;
  out->id = b->id;
  out->impl = b;
  out->bytes = d.bytes;
  out->mapped = b->mapped;
  return Status::Ok;
}
void MetalDev::destroy(BufferH& h) {
  if (!h.impl) return;
  delete static_cast<MBuffer*>(h.impl);
  h = {};
}

Status MetalDev::createImage(const ImageDesc& d, ImageH* out) {
  if (d.w == 0 || d.h == 0 || d.w > caps_.maxImage2D || d.h > caps_.maxImage2D)
    return fail("image size outside the device limits", Status::InvalidArgument), Status::InvalidArgument;
  auto* im = new MImage;
  im->fmt = d.fmt;
  std::uint64_t bytes = 0;
  @autoreleasepool {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = d.d > 1 ? MTLTextureType3D : MTLTextureType2D;
    td.pixelFormat = toMtl(d.fmt);
    td.width = d.w;
    td.height = d.h;
    td.depth = d.d;
    td.mipmapLevelCount = 1;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    im->tex = [dev_ newTextureWithDescriptor:td];
    if (im->tex) bytes = [dev_ heapTextureSizeAndAlignWithDescriptor:td].size;
  }
  if (!im->tex) {
    delete im;
    return fail("newTextureWithDescriptor failed", Status::OutOfMemory), Status::OutOfMemory;
  }
  im->id = nextId_++;
  out->id = im->id;
  out->impl = im;
  out->fmt = d.fmt;
  out->w = d.w;
  out->h = d.h;
  out->d = d.d;
  out->bytes = bytes;
  return Status::Ok;
}
void MetalDev::destroy(ImageH& h) {
  if (!h.impl) return;
  delete static_cast<MImage*>(h.impl);
  h = {};
}
ImageH MetalDev::dummyImage(Format f) {
  auto it = dummies_.find(int(f));
  ImageH h;
  if (it != dummies_.end()) {
    h.impl = it->second;
    h.id = it->second->id;
    h.fmt = f;
    h.w = h.h = h.d = 1;
    return h;
  }
  if (createImage({f, 1, 1, 1}, &h) != Status::Ok) return {};
  dummies_[int(f)] = static_cast<MImage*>(h.impl);
  return h;
}

// ---- pipelines ----
Status MetalDev::createPipeline(const PipelineDesc& pd, PipelineH* out) {
  std::lock_guard<std::mutex> lk(pipeMtx_);
  std::string key = pd.shader;
  for (auto& s : pd.spec) key += "|" + std::to_string(s.id) + "=" + std::to_string(s.value);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    out->impl = it->second.get();
    out->bindings = it->second->bindings.c_str();
    return Status::Ok;
  }
  const MslBin* bin = nullptr;
  for (std::size_t i = 0; i < kShaderCount; ++i)
    if (pd.shader == kShaders[i].name) bin = &kShaders[i];
  if (!bin) return fail("unknown shader " + pd.shader, Status::Unsupported), Status::Unsupported;
  if (!pd.bindings.empty() && pd.bindings != bin->sig)
    return fail("binding signature of " + pd.shader + " differs from the shader table", Status::InvalidArgument), Status::InvalidArgument;

  auto p = std::make_unique<MPipeline>();
  @autoreleasepool {
    NSError* e = nil;
    id<MTLLibrary> lib = lib_;
    if (!lib) {
      if (!bin->msl) return fail("no Metal library embedded", Status::Unsupported), Status::Unsupported;
      auto li = srcLibs_.find(pd.shader);
      if (li != srcLibs_.end()) lib = li->second;
      else {
        MTLCompileOptions* co = [MTLCompileOptions new];
        co.fastMathEnabled = NO;
        co.languageVersion = MTLLanguageVersion2_3;
        lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:bin->msl] options:co error:&e];
        if (!lib) return fail(std::string("MSL compile of ") + pd.shader + " failed: " + (e ? e.localizedDescription.UTF8String : "?"), Status::Internal), Status::Internal;
        srcLibs_[pd.shader] = lib;
      }
    }
    MTLFunctionConstantValues* cv = [MTLFunctionConstantValues new];
    std::int32_t tg[3] = {bin->lsz[0], bin->lsz[1], bin->lsz[2]};
    NSString* fn = [NSString stringWithFormat:@"k_%s", bin->name];
    id<MTLFunction> proto = [lib newFunctionWithName:fn];
    if (!proto) return fail(std::string("function k_") + bin->name + " not found in the Metal library", Status::Internal), Status::Internal;
    for (const SpecConst& sc : pd.spec) {
      // the function's own declaration decides the type (SPIRV-Cross may declare a GLSL uint constant as int)
      for (MTLFunctionConstant* fc in proto.functionConstantsDictionary.allValues) {
        if (std::uint32_t(fc.index) != sc.id) continue;
        const std::int32_t v = sc.value;
        switch (fc.type) {
          case MTLDataTypeUInt: { std::uint32_t u = std::uint32_t(v); [cv setConstantValue:&u type:MTLDataTypeUInt atIndex:sc.id]; break; }
          case MTLDataTypeBool: { bool b = v != 0; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:sc.id]; break; }
          case MTLDataTypeFloat: { float f = float(v); [cv setConstantValue:&f type:MTLDataTypeFloat atIndex:sc.id]; break; }
          default: { std::int32_t i = v; [cv setConstantValue:&i type:MTLDataTypeInt atIndex:sc.id]; break; }
        }
      }
      for (int a = 0; a < 3; ++a)
        if (bin->wscId[a] >= 0 && std::uint32_t(bin->wscId[a]) == sc.id) tg[a] = sc.value;
    }
    id<MTLFunction> f = [lib newFunctionWithName:fn constantValues:cv error:&e];
    if (!f) return fail(std::string("function k_") + bin->name + ": " + (e ? e.localizedDescription.UTF8String : "?"), Status::Internal), Status::Internal;
    MTLComputePipelineDescriptor* cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = f;
    cd.label = [NSString stringWithUTF8String:key.c_str()];
    if (archive_) cd.binaryArchives = @[ archive_ ];
    id<MTLComputePipelineState> pso = [dev_ newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:&e];
    if (!pso) return fail("pipeline " + key + ": " + (e ? e.localizedDescription.UTF8String : "?"), Status::Internal), Status::Internal;
    p->pso = pso;
    if (archive_) {
      NSError* ae = nil;
      if ([archive_ addComputePipelineFunctionsWithDescriptor:cd error:&ae]) archiveDirty_ = true;
    }
    p->tg = MTLSizeMake(std::max(tg[0], 1), std::max(tg[1], 1), std::max(tg[2], 1));
    if (p->tg.width * p->tg.height * p->tg.depth > pso.maxTotalThreadsPerThreadgroup)
      return fail("pipeline " + key + ": threadgroup " + std::to_string(p->tg.width * p->tg.height * p->tg.depth) + " exceeds the pipeline limit " +
                      std::to_string(pso.maxTotalThreadsPerThreadgroup),
                  Status::Unsupported),
             Status::Unsupported;
  }
  p->bindings = bin->sig;
  p->name = key;
  out->impl = p.get();
  out->bindings = p->bindings.c_str();
  pipelines_[key] = std::move(p);
  return Status::Ok;
}

id<MTLComputePipelineState> MetalDev::swizzlePso() {
  if (swizzle_) return swizzle_;
  @autoreleasepool {
    static const char* src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void k_swz(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],"
        " constant uint2& wh [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= wh.x || g.y >= wh.y) return;\n  d.write(s.read(g), g);  // the BGRA8 texture stores the channels as B,G,R,A by itself\n}\n";
    NSError* e = nil;
    MTLCompileOptions* co = [MTLCompileOptions new];
    co.fastMathEnabled = NO;
    id<MTLLibrary> l = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src] options:co error:&e];
    if (!l) return nil;
    id<MTLFunction> f = [l newFunctionWithName:@"k_swz"];
    swizzle_ = [dev_ newComputePipelineStateWithFunction:f error:&e];
  }
  return swizzle_;
}

// ---- command lists ----
MetalCmd::MetalCmd(MetalDev& d, int slot) : d_(d), slot_(slot) {
  cb_ = [d.queue_ commandBuffer];
  cb_.label = @"dapple";
  std::memset(params_, 0, sizeof params_);
  newPart();
}
MetalCmd::~MetalCmd() {
  if (ce_ || be_) endEncoders();
}
void MetalCmd::endEncoders() {
  if (ce_) {
    [ce_ endEncoding];
    ce_ = nil;
    encPl_ = nullptr;
  }
  if (be_) {
    [be_ endEncoding];
    be_ = nil;
  }
}
id<MTLComputeCommandEncoder> MetalCmd::compute() {
  if (be_) {
    [be_ endEncoding];
    be_ = nil;
  }
  if (!ce_) {
    if (pt_ && pt_->segments < kMaxSegments) {
      MTLComputePassDescriptor* pd = [MTLComputePassDescriptor computePassDescriptor];
      pd.dispatchType = MTLDispatchTypeSerial;
      pd.sampleBufferAttachments[0].sampleBuffer = pt_->sb;
      pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(pt_->segments * 2);
      pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = NSUInteger(pt_->segments * 2 + 1);
      ce_ = [cb_ computeCommandEncoderWithDescriptor:pd];
      segOpen_ = true;
    } else {
      ce_ = [cb_ computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
      segOpen_ = false;
    }
  }
  return ce_;
}
id<MTLBlitCommandEncoder> MetalCmd::blit() {
  if (ce_) {
    [ce_ endEncoding];
    ce_ = nil;
    encPl_ = nullptr;
  }
  if (!be_) be_ = [cb_ blitCommandEncoder];
  return be_;
}

void MetalCmd::newPart() {
  pt_.reset();
  if (!d_.tsSet_) return;
  MTLCounterSampleBufferDescriptor* sd = [MTLCounterSampleBufferDescriptor new];
  sd.counterSet = d_.tsSet_;
  sd.storageMode = MTLStorageModeShared;
  sd.sampleCount = NSUInteger(kMaxSegments) * 2;
  sd.label = @"dapple ts";
  NSError* e = nil;
  id<MTLCounterSampleBuffer> sb = [d_.dev_ newCounterSampleBufferWithDescriptor:sd error:&e];
  if (!sb) return;  // out of counter sample buffers: this part is not timed
  pt_ = std::make_shared<PartTimes>();
  pt_->sb = sb;
}

// The label closes the pass recorded since the previous timestamp: a compute pass is its own encoder (sampled at both ends), a
// copy has no compute encoder open and is reported as 0 ms (the entries stay aligned with the scheduler's passes).
void MetalCmd::timestamp(const char* label) {
  if (!pt_) return;
  pt_->labels.push_back(label ? label : "");
  if (ce_ && segOpen_) {
    pt_->segment.push_back(pt_->segments++);
    [ce_ endEncoding];
    ce_ = nil;
    encPl_ = nullptr;
    segOpen_ = false;
  } else {
    pt_->segment.push_back(-1);
  }
}

Status MetalCmd::setParams(const void* p, std::size_t n) {
  if (n > sizeof params_) return fail(Status::InvalidArgument), d_.fail("parameter block too large", Status::InvalidArgument), Status::InvalidArgument;
  std::memcpy(params_, p, n);
  paramBytes_ = (n + 15) & ~std::size_t(15);
  haveParams_ = true;
  return Status::Ok;
}

Status MetalCmd::dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) {
  if (err_ != Status::Ok) return err_;
  if (!pl_ || !haveParams_) return fail(Status::InvalidArgument), Status::InvalidArgument;
  const int nb = int(pl_->bindings.size());
  for (int i = 0; i < nb; ++i)
    if (!bound_[i].img && !bound_[i].buf)
      return fail(Status::InvalidArgument), d_.fail("unbound slot in dispatch of " + pl_->name, Status::InvalidArgument), Status::InvalidArgument;
  id<MTLComputeCommandEncoder> e = compute();
  if (encPl_ != pl_) {
    [e setComputePipelineState:pl_->pso];
    encPl_ = pl_;
  }
  [e setBytes:params_ length:paramBytes_ atIndex:0];
  for (int i = 0; i < nb; ++i) {
    const char c = pl_->bindings[std::size_t(i)];
    const NSUInteger idx = NSUInteger(i + 1);
    if (c == 'B') {
      [e setBuffer:bound_[i].buf->buf offset:0 atIndex:idx];
    } else {
      [e setTexture:bound_[i].img->tex atIndex:idx];
      if (c == 'S') [e setSamplerState:d_.sampler_ atIndex:idx];
    }
  }
  [e dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:pl_->tg];
  ++st_.passes;
  haveParams_ = false;
  return Status::Ok;
}

void MetalCmd::copyBufferToImage(BufferH src, ImageH dst, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t off) {
  auto* im = static_cast<MImage*>(dst.impl);
  const NSUInteger bpt = bytesPerTexel(im->fmt);
  [blit() copyFromBuffer:static_cast<MBuffer*>(src.impl)->buf
            sourceOffset:off
       sourceBytesPerRow:w * bpt
     sourceBytesPerImage:NSUInteger(w) * h * bpt
              sourceSize:MTLSizeMake(w, h, d)
               toTexture:im->tex
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
  ++st_.passes;
}
void MetalCmd::copyBufferToBuffer(BufferH src, BufferH dst, std::uint64_t bytes, std::uint64_t off) {
  [blit() copyFromBuffer:static_cast<MBuffer*>(src.impl)->buf
            sourceOffset:off
                toBuffer:static_cast<MBuffer*>(dst.impl)->buf
       destinationOffset:0
                    size:bytes];
  ++st_.passes;
}
void MetalCmd::copyImageToBuffer(ImageH src, BufferH dst, const Rect2& r, std::uint64_t off) {
  auto* im = static_cast<MImage*>(src.impl);
  const NSUInteger bpt = bytesPerTexel(im->fmt);
  [blit() copyFromTexture:im->tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(r.x, r.y, 0)
               sourceSize:MTLSizeMake(r.w, r.h, 1)
                 toBuffer:static_cast<MBuffer*>(dst.impl)->buf
        destinationOffset:off
   destinationBytesPerRow:NSUInteger(r.w) * bpt
 destinationBytesPerImage:NSUInteger(r.w) * r.h * bpt];
  ++st_.passes;
}
void MetalCmd::copyImage(ImageH src, ImageH dst, std::uint32_t w, std::uint32_t h) {
  [blit() copyFromTexture:static_cast<MImage*>(src.impl)->tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:static_cast<MImage*>(dst.impl)->tex
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  ++st_.passes;
}

Status MetalCmd::copyImageToExternal(ImageH src, const ExternalImage& dst, std::uint32_t w, std::uint32_t h) {
  if (err_ != Status::Ok) return err_;
  auto* im = static_cast<MImage*>(src.impl);
  id<MTLTexture> t = (__bridge id<MTLTexture>)dst.handle;
  if (!im || !t || im->fmt != Format::RGBA8) return Status::InvalidArgument;
  if (w > t.width || h > t.height) return Status::InvalidArgument;
  if (t.pixelFormat == MTLPixelFormatRGBA8Unorm || t.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
    [blit() copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                  toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
  } else if (t.pixelFormat == MTLPixelFormatBGRA8Unorm || t.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB) {
    id<MTLComputePipelineState> pso = d_.swizzlePso();
    if (!pso || !(t.usage & MTLTextureUsageShaderWrite)) return Status::Unsupported;
    id<MTLComputeCommandEncoder> e = compute();
    encPl_ = nullptr;
    const std::uint32_t wh[2] = {w, h};
    [e setComputePipelineState:pso];
    [e setTexture:im->tex atIndex:0];
    [e setTexture:t atIndex:1];
    [e setBytes:wh length:sizeof wh atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
  } else {
    return Status::Unsupported;
  }
  ++st_.passes;
  return Status::Ok;
}

Status MetalCmd::flushPart() {
  if (err_ != Status::Ok) return err_;
  endEncoders();
  id<MTLCommandBuffer> old = cb_;
  cb_ = [d_.queue_ commandBuffer];
  cb_.label = @"dapple";
  auto pt = pt_;
  newPart();
  d_.commit(*this, old, std::move(pt));
  ++st_.parts;
  return Status::Ok;
}

// ---- submission ----
void MetalDev::commit(MetalCmd& c, id<MTLCommandBuffer> cb, std::shared_ptr<PartTimes> pt) {
  Slot& s = slots_[c.slot_];
  {
    std::lock_guard<std::mutex> lk(s.m);
    ++s.outstanding;
  }
  Slot* sp = &s;
  MetalDev* self = this;
  static const bool trace = std::getenv("DAPPLE_METAL_TRACE") != nullptr;
  const double tCommit = CACurrentMediaTime();
  [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      Status st = Status::Internal;
      std::string msg = "command buffer failed";
      if (done.error) {
        msg = done.error.localizedDescription.UTF8String;
        switch (done.error.code) {
          case MTLCommandBufferErrorOutOfMemory: st = Status::OutOfMemory; break;
          case MTLCommandBufferErrorTimeout:
          case MTLCommandBufferErrorPageFault:
          case MTLCommandBufferErrorAccessRevoked:
          case MTLCommandBufferErrorNotPermitted:
#if TARGET_OS_OSX
          case MTLCommandBufferErrorDeviceRemoved:
#endif
          st = Status::DeviceLost; break;
          default: st = Status::Internal; break;
        }
      }
      if (st == Status::DeviceLost) self->lost_ = true;
      int exp = 0;
      sp->failCode.compare_exchange_strong(exp, int(st) + 1);
      sp->failed = true;
      std::fprintf(stderr, "[dapple metal] %s\n", msg.c_str());
    }
    if (pt && pt->sb && !pt->labels.empty() && done.status == MTLCommandBufferStatusCompleted) {
      NSData* raw = [pt->sb resolveCounterRange:NSMakeRange(0, NSUInteger(pt->segments) * 2)];
      const double nsT = self->nsPerTick();
      std::vector<PassTime> add;
      for (std::size_t i = 0; i < pt->labels.size(); ++i) {
        double ms = 0;
        const int sg = pt->segment[i];
        if (sg >= 0 && raw && raw.length >= NSUInteger(sg * 2 + 2) * sizeof(MTLCounterResultTimestamp)) {
          const auto* r = static_cast<const MTLCounterResultTimestamp*>(raw.bytes);
          const std::uint64_t t0 = r[sg * 2].timestamp, t1 = r[sg * 2 + 1].timestamp;
          if (t0 != MTLCounterErrorValue && t1 != MTLCounterErrorValue && t1 >= t0) ms = double(t1 - t0) * nsT * 1e-6;
        }
        add.push_back({pt->labels[i], ms});
      }
      std::lock_guard<std::mutex> lk(sp->m);
      for (auto& a : add) sp->times.push_back(std::move(a));
    }
    if (pt) pt->sb = nil;
    if (trace)
      std::fprintf(stderr, "[dapple metal] cb: commit->kernelStart %.2f ms, kernel %.2f ms (GPU %.2f ms), kernelEnd->completed %.2f ms\n",
                   (done.kernelStartTime - tCommit) * 1e3, (done.kernelEndTime - done.kernelStartTime) * 1e3, (done.GPUEndTime - done.GPUStartTime) * 1e3,
                   (CACurrentMediaTime() - done.kernelEndTime) * 1e3);
    const double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
    if (ms > 0) {
      double a = self->gpuAccum_.load();
      while (!self->gpuAccum_.compare_exchange_weak(a, a + ms)) {}
    }
    std::lock_guard<std::mutex> lk(sp->m);
    --sp->outstanding;
    sp->cv.notify_all();
  }];
  [cb commit];
}

std::unique_ptr<CommandList> MetalDev::begin() {
  if (lost_) return nullptr;
  int idx = nextSlot_;
  nextSlot_ = (nextSlot_ + 1) % kSlots;
  if (idx == pinned_) {
    idx = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
  }
  Slot& s = slots_[idx];
  if (s.inFlight && waitSlot(s, 30ull * 1000 * 1000 * 1000) != Status::Ok) return nullptr;
  s.failed = false;
  s.failCode = 0;
  { std::lock_guard<std::mutex> lk(s.m); s.times.clear(); }
  return std::make_unique<MetalCmd>(*this, idx);
}

Status MetalDev::submit(CommandList& cl, FenceH* signal) {
  if (lost_) return Status::DeviceLost;
  auto& c = static_cast<MetalCmd&>(cl);
  c.endEncoders();
  if (c.err_ != Status::Ok) return c.err_;
  Slot& s = slots_[c.slot_];
  s.st = c.st_;
  commit(c, c.cb_, std::move(c.pt_));
  c.cb_ = nil;
  s.inFlight = true;
  ++s.gen;
  if (signal) {
    signal->slot = std::uint32_t(c.slot_);
    signal->gen = s.gen;
  }
  return Status::Ok;
}

Status MetalDev::waitSlot(Slot& s, std::uint64_t ns) {
  if (!s.inFlight) return Status::Ok;
  {
    std::unique_lock<std::mutex> lk(s.m);
    if (!s.cv.wait_for(lk, std::chrono::nanoseconds(ns), [&] { return s.outstanding == 0; }))
      return fail("fence wait timed out", Status::Timeout), Status::Timeout;
  }
  s.inFlight = false;
  if (s.failed) {
    const Status st = Status(s.failCode.load() - 1);
    fail("Metal command buffer error", st);
    return st;
  }
  return Status::Ok;
}
Status MetalDev::wait(FenceH f, std::uint64_t ns) {
  if (!f || f.slot >= std::uint32_t(kSlots)) return Status::InvalidArgument;
  Slot& s = slots_[f.slot];
  if (s.gen != f.gen) return Status::Ok;
  return waitSlot(s, ns);
}
bool MetalDev::poll(FenceH f) {
  if (!f || f.slot >= std::uint32_t(kSlots)) return true;
  Slot& s = slots_[f.slot];
  if (s.gen != f.gen || !s.inFlight) return true;
  std::lock_guard<std::mutex> lk(s.m);
  return s.outstanding == 0;
}
Status MetalDev::waitAll() {
  Status res = Status::Ok;
  for (Slot& s : slots_) {
    const Status st = waitSlot(s, 30ull * 1000 * 1000 * 1000);
    if (st != Status::Ok) res = st;
  }
  return res;
}
void MetalDev::waitIdle() {
  if (!queue_) return;
  for (Slot& s : slots_) {
    std::unique_lock<std::mutex> lk(s.m);
    s.cv.wait(lk, [&] { return s.outstanding == 0; });
    s.inFlight = false;
  }
}

}  // namespace

std::unique_ptr<Device> createMetalDevice(const DeviceOptions& opt, std::string& err) {
  auto d = std::make_unique<MetalDev>();
  if (!d->init(opt, err)) return nullptr;
  return d;
}
std::vector<std::string> shaderNames() {
  std::vector<std::string> v;
  for (std::size_t i = 0; i < kShaderCount; ++i) v.push_back(kShaders[i].name);
  return v;
}
void* nativeMetalDevice(Device& d) { return dynamic_cast<MetalDev*>(&d) ? static_cast<MetalDev&>(d).nativeDevice() : nullptr; }

}  // namespace dapple::hal::metal

// hal.cpp declares the Vulkan factory; on Apple there is none (Api::Metal is a first-class backend: hal::createDevice(Api::Metal),
// selectDevice() tries it first, gpu_context.cpp lists it as a candidate).
namespace dapple::hal::vk {
std::unique_ptr<Device> createVulkanDevice(const DeviceOptions&, std::string& err) {
  err = "Vulkan is not part of the Apple build (Metal is used)";
  return nullptr;
}
}  // namespace dapple::hal::vk
