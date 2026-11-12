// Vulkan 1.0 backend of the compute HAL. Extracted from core/gpu/src/vk_core.cpp (phase 1); behaviour kept, including the
// Pixel 5 findings (docs/engine/pipeline-architecture.md 16): submissions are cut by the scheduler through
// CommandList::flushPart(), readback buffers prefer HOST_CACHED non-coherent memory (invalidated in wait()), memory types
// are chosen per buffer from memoryTypeBits, descriptor sets are cached so a steady-state frame allocates nothing.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "engine/hal/hal.h"
#include "volk.h"

namespace dapple::hal::vk {

struct ShaderBin {
  const char* name;
  const unsigned char* data;
  std::size_t size;
  const char* bindings;
};
#include "shader_table.inc"

namespace {

std::string vkErr(const char* what, VkResult r) {
  char b[160];
  std::snprintf(b, sizeof b, "%s failed (VkResult %d)", what, int(r));
  return b;
}

VkFormat toVk(Format f) {
  switch (f) {
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::R16F: return VK_FORMAT_R16_SFLOAT;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32UI: return VK_FORMAT_R32_UINT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

struct VBuffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
  bool coherent = true;
  std::uint64_t id = 0;
};
struct VImage {
  VkImage img = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  std::uint64_t id = 0;
  bool inited = false;  // moved out of UNDEFINED into GENERAL (lazily, in the first command buffer that uses it)
};
struct VPipeline {
  VkPipeline pipe = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  std::string bindings;
  std::string name;
};

constexpr std::size_t kParamRing = 512 * 1024;
constexpr std::size_t kUboRange = 256;
constexpr int kMaxBind = 8;

struct Slot {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> extraCbs;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool qp = VK_NULL_HANDLE;
  bool inFlight = false;
  std::uint64_t gen = 0;
  VBuffer params;
  std::vector<std::string> passNames;
  std::vector<PassTime> times;
  SubmitStats st;
  double gpuMs = 0;
};

class VkDev;

class VkCmd final : public CommandList {
 public:
  VkCmd(VkDev& d, int slot);
  int slot() const override { return slot_; }
  void bindPipeline(PipelineH p) override { pl_ = static_cast<VPipeline*>(p.impl); }
  void bind(std::uint32_t s, BufferH b) override {
    if (s < kMaxBind) bound_[s] = {nullptr, static_cast<VBuffer*>(b.impl)};
  }
  void bind(std::uint32_t s, ImageH i) override {
    if (s < kMaxBind) {
      VImage* im = static_cast<VImage*>(i.impl);
      init(im);
      bound_[s] = {im, nullptr};
    }
  }
  Status setParams(const void* p, std::size_t n) override;
  Status dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) override;
  void barrier(std::uint32_t mask) override;
  void copyBufferToImage(BufferH, ImageH, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t off) override;
  void copyBufferToBuffer(BufferH, BufferH, std::uint64_t bytes, std::uint64_t off) override;
  void copyImageToBuffer(ImageH, BufferH, const Rect2&, std::uint64_t off) override;
  void copyImage(ImageH, ImageH, std::uint32_t w, std::uint32_t h) override;
  void timestamp(const char* label) override;
  Status flushPart() override;
  Status status() const override { return err_; }

  void finish();  // end recording (host barrier)
  void fail(Status s) {
    if (err_ == Status::Ok) err_ = s;
  }

 private:
  friend class VkDev;
  void init(VImage* im);
  void memBarrier();

  VkDev& d_;
  int slot_;
  Slot& s_;
  VkCommandBuffer cb_;
  VPipeline* pl_ = nullptr;
  struct B {
    VImage* img = nullptr;
    VBuffer* buf = nullptr;
  } bound_[kMaxBind];
  std::size_t ring_ = 0, paramOff_ = 0;
  bool haveParams_ = false;
  std::size_t extraUsed_ = 0;
  int tsIdx_ = 0;
  Status err_ = Status::Ok;
};

class VkDev final : public Device {
 public:
  ~VkDev() override;
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
  void flush(BufferH b, std::uint64_t off, std::uint64_t n) override;
  void invalidate(BufferH b, std::uint64_t off, std::uint64_t n) override;
  std::unique_ptr<CommandList> begin() override;
  void pinSlot(int s) override { pinned_ = s; }
  Status submit(CommandList&, FenceH*) override;
  Status wait(FenceH, std::uint64_t timeoutNs) override;
  bool poll(FenceH) override;
  Status waitAll() override;
  void waitIdle() override;
  const std::vector<PassTime>& passTimes(FenceH f) const override { return slots_[f.slot < kSlots ? f.slot : 0].times; }
  SubmitStats stats(FenceH f) const override { return slots_[f.slot < kSlots ? f.slot : 0].st; }
  double gpuAccumMs() const override { return gpuAccum_; }
  std::vector<std::uint8_t> pipelineCacheBlob() const override;
  void trim() override;
  double splitGroups() const override { return splitGroups_; }
  std::size_t poolBudget() const override { return budget_; }

 private:
  friend class VkCmd;
  std::uint32_t findMem(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;
  bool slotInit(Slot& s, std::string& err);
  void slotDestroy(Slot& s);
  VkDescriptorSet descriptorFor(VPipeline* pl, const std::vector<std::uint64_t>& ids, VBuffer* ubo, const VkCmd::B* bound, int nb);
  Status waitSlot(Slot& s, std::uint64_t ns);
  void fail(const std::string& e, Status s) {
    lastErr_ = e;
    if (s == Status::DeviceLost) lost_ = true;
  }
  void destroyBuf(VBuffer& b);
  void destroyImg(VImage& im);
  Status makeBuffer(VBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind);

  DeviceOptions opt_;
  DeviceCaps caps_;
  VkInstance inst_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t family_ = 0;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_{};
  VkPhysicalDeviceLimits limits_{};
  float tsPeriod_ = 1.f;
  std::uint32_t tsBits_ = 0;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;
  std::unordered_map<std::string, VkDescriptorSet> sets_;
  std::unordered_map<std::string, VkShaderModule> modules_;
  std::unordered_map<std::string, std::unique_ptr<VPipeline>> pipelines_;
  std::unordered_map<std::string, VkDescriptorSetLayout> dsls_;
  std::unordered_map<std::string, VkPipelineLayout> layouts_;
  std::unordered_map<int, VImage*> dummies_;
  std::vector<VBuffer*> nonCoherentReadbacks_;
  std::mutex pipeMtx_;
  Slot slots_[kSlots];
  int nextSlot_ = 0, pinned_ = -1;
  std::uint64_t nextId_ = 1;
  bool lost_ = false;
  double gpuAccum_ = 0;
  double splitGroups_ = 250000;
  std::size_t budget_ = 0;
  std::string lastErr_;
};

// ---------------------------------------------------------------------------------------------------- device ---
bool VkDev::init(const DeviceOptions& opt, std::string& err) {
  opt_ = opt;
  if (volkInitialize() != VK_SUCCESS) return err = "no Vulkan loader (volkInitialize failed)", false;
  std::uint32_t instVer = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&instVer);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "dapple";
  app.apiVersion = instVer >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkResult r = vkCreateInstance(&ici, nullptr, &inst_);
  if (r != VK_SUCCESS) return err = vkErr("vkCreateInstance", r), false;
  volkLoadInstance(inst_);

  std::uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst_, &n, nullptr);
  if (!n) return err = "no Vulkan physical device", false;
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst_, &n, devs.data());

  std::vector<BlockRule> rules;  // built-in rules: none yet, the self-test is the primary gate
  for (const std::string& extra : {opt.blocklist, std::string(std::getenv("DAPPLE_GPU_BLOCKLIST") ? std::getenv("DAPPLE_GPU_BLOCKLIST") : "")})
    for (BlockRule& b : parseBlocklist(extra)) rules.push_back(std::move(b));
  int forced = opt.deviceIndex;
  if (const char* env = std::getenv("DAPPLE_GPU_DEVICE")) forced = std::atoi(env);

  struct Cand {
    int idx, score;
    std::uint32_t fam;
    DeviceCaps di;
  };
  std::vector<Cand> cands;
  std::string firstReject;
  for (std::uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
    int fam = -1;
    for (std::uint32_t q = 0; q < qn; ++q)
      if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        if (fam < 0 || (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qf[q].timestampValidBits > 0)) fam = int(q);
      }
    if (fam < 0) continue;
    DeviceCaps di;
    di.api = Api::Vulkan;
    di.name = p.deviceName;
    di.apiVersion = p.apiVersion;
    di.driverVersion = p.driverVersion;
    di.vendorId = p.vendorID;
    di.deviceId = p.deviceID;
    di.deviceType = p.deviceType;
    di.software = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (p.apiVersion >= VK_API_VERSION_1_2 && vkGetPhysicalDeviceProperties2) {
      VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p2.pNext = &drv;
      vkGetPhysicalDeviceProperties2(devs[i], &p2);
      di.driver = std::string(drv.driverName) + " " + drv.driverInfo;
    }
    const std::string why = blockReason(di, rules, opt.allowSoftware);
    if (!why.empty()) {
      if (firstReject.empty()) firstReject = why;
      continue;
    }
    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
    if (forced == int(i)) score = 100;
    cands.push_back({int(i), score, std::uint32_t(fam), di});
  }
  if (cands.empty())
    return err = firstReject.empty() ? "no Vulkan device with a compute queue" : "no usable Vulkan device: " + firstReject, false;
  std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
  const Cand& pick = cands.front();
  phys_ = devs[pick.idx];
  family_ = pick.fam;
  caps_ = pick.di;

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys_, &props);
  limits_ = props.limits;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_);
  caps_.maxImage2D = limits_.maxImageDimension2D;
  caps_.maxImage3D = limits_.maxImageDimension3D;
  caps_.maxSharedBytes = limits_.maxComputeSharedMemorySize;
  caps_.maxWorkgroupInvocations = limits_.maxComputeWorkGroupInvocations;
  caps_.uniformOffsetAlign = std::uint32_t(std::max<VkDeviceSize>(256, limits_.minUniformBufferOffsetAlignment));
  caps_.maxBufferBytes = limits_.maxStorageBufferRange;
  for (std::uint32_t i = 0; i < mem_.memoryHeapCount; ++i)
    if (mem_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      caps_.deviceLocalBytes = std::max<std::uint64_t>(caps_.deviceLocalBytes, mem_.memoryHeaps[i].size);
  for (std::uint32_t i = 0; i < mem_.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem_.memoryTypes[i].propertyFlags;
    char b[160];
    std::snprintf(b, sizeof b, "  memtype %u heap %u (%.0f MB%s): %s%s%s%s%s\n", i, mem_.memoryTypes[i].heapIndex,
                  double(mem_.memoryHeaps[mem_.memoryTypes[i].heapIndex].size) / 1048576.0,
                  (mem_.memoryHeaps[mem_.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", device-local heap" : "",
                  (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
                  (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "", (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
                  (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZY " : "");
    caps_.memoryTypes += b;
  }
  caps_.unifiedMemory = caps_.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || caps_.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
  caps_.linearFilterRgba32f = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  for (int f = 0; f < kFormatCount; ++f) {
    const VkFormat vf = toVk(Format(f));
    vkGetPhysicalDeviceFormatProperties(phys_, vf, &fp);
    FormatCaps& c = caps_.format[f];
    c.sampled = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    c.filterable = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    c.storageRead = c.storageWrite = c.storageReadWrite = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
  }
  for (Format f : {Format::RGBA16F, Format::RGBA32F, Format::R32F, Format::RGBA8})
    if (!caps_.format[int(f)].sampled || !caps_.format[int(f)].storageWrite) return err = "device lacks storage image support for a working format", false;
  if (limits_.maxPerStageDescriptorStorageImages < 4 || limits_.maxComputeSharedMemorySize < 16384)
    return err = "device limits below what the kernels need (storage images per stage " + std::to_string(limits_.maxPerStageDescriptorStorageImages) +
                 ", shared memory " + std::to_string(limits_.maxComputeSharedMemorySize) + ")",
           false;

  if (props.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    caps_.subgroupSize = sg.subgroupSize;
    caps_.subgroupOps = sg.supportedOperations;
    caps_.subgroups = sg.supportedOperations != 0;
    VkPhysicalDevice16BitStorageFeatures s16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &s16;
    vkGetPhysicalDeviceFeatures2(phys_, &f2);
    caps_.storage16 = s16.storageBuffer16BitAccess != 0;
  }
  {
    std::uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ex(en);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, ex.data());
    for (auto& x : ex)
      if (!std::strcmp(x.extensionName, "VK_KHR_shader_float16_int8")) caps_.fp16Arithmetic = true;
  }
  std::uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
  tsBits_ = qf[family_].timestampValidBits;
  caps_.timestamps = tsBits_ > 0 && limits_.timestampComputeAndGraphics;
  tsPeriod_ = limits_.timestampPeriod;

  const float prio = 1.f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  r = vkCreateDevice(phys_, &dci, nullptr, &dev_);
  if (r != VK_SUCCESS) return err = vkErr("vkCreateDevice", r), false;
  volkLoadDevice(dev_);
  vkGetDeviceQueue(dev_, family_, 0, &queue_);

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = family_;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if ((r = vkCreateCommandPool(dev_, &cpi, nullptr, &cmdPool_)) != VK_SUCCESS) return err = vkErr("vkCreateCommandPool", r), false;

  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  std::vector<char> cacheBlob;
  if (!opt.pipelineCachePath.empty()) {
    std::ifstream f(opt.pipelineCachePath, std::ios::binary);
    if (f) cacheBlob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    pci.initialDataSize = cacheBlob.size();
    pci.pInitialData = cacheBlob.data();
  }
  if (vkCreatePipelineCache(dev_, &pci, nullptr, &cache_) != VK_SUCCESS && !cacheBlob.empty()) {
    pci.initialDataSize = 0;
    pci.pInitialData = nullptr;
    vkCreatePipelineCache(dev_, &pci, nullptr, &cache_);
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(dev_, &sci, nullptr, &sampler_);

  VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048},
                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048 * 6},
                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048 * 2},
                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048 * 2}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 2048;
  dpi.poolSizeCount = 4;
  dpi.pPoolSizes = ps;
  if ((r = vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_)) != VK_SUCCESS) return err = vkErr("vkCreateDescriptorPool", r), false;

  budget_ = opt.poolBudgetBytes ? opt.poolBudgetBytes
                                : std::size_t(std::min<std::uint64_t>(std::max<std::uint64_t>(caps_.deviceLocalBytes / 4, 256u << 20), std::uint64_t(caps_.unifiedMemory ? 512u : 1024u) << 20));
  if (opt.splitGroups > 0) splitGroups_ = opt.splitGroups;
  if (const char* sg = std::getenv("DAPPLE_GPU_SPLIT_GROUPS")) splitGroups_ = std::atof(sg) > 0 ? std::atof(sg) : 1e30;
  for (Slot& s : slots_)
    if (!slotInit(s, err)) return false;
  return true;
}

bool VkDev::slotInit(Slot& s, std::string& err) {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = cmdPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkResult r = vkAllocateCommandBuffers(dev_, &ai, &s.cb);
  if (r != VK_SUCCESS) return err = vkErr("vkAllocateCommandBuffers", r), false;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if ((r = vkCreateFence(dev_, &fi, nullptr, &s.fence)) != VK_SUCCESS) return err = vkErr("vkCreateFence", r), false;
  if (opt_.profile && caps_.timestamps) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 512;
    vkCreateQueryPool(dev_, &qi, nullptr, &s.qp);
  }
  const Status st = makeBuffer(s.params, kParamRing + kUboRange, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemKind::Upload);
  if (st != Status::Ok) return err = lastErr_, false;
  return true;
}

void VkDev::slotDestroy(Slot& s) {
  destroyBuf(s.params);
  if (s.qp) vkDestroyQueryPool(dev_, s.qp, nullptr);
  if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
  if (s.cb) vkFreeCommandBuffers(dev_, cmdPool_, 1, &s.cb);
  if (!s.extraCbs.empty()) vkFreeCommandBuffers(dev_, cmdPool_, std::uint32_t(s.extraCbs.size()), s.extraCbs.data());
  s = Slot{};
}

VkDev::~VkDev() {
  if (dev_) {
    vkDeviceWaitIdle(dev_);
    if (!opt_.pipelineCachePath.empty() && cache_) {
      const auto blob = pipelineCacheBlob();
      if (!blob.empty()) {
        std::ofstream f(opt_.pipelineCachePath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
      }
    }
    for (Slot& s : slots_) slotDestroy(s);
    for (auto& d : dummies_) {
      destroyImg(*d.second);
      delete d.second;
    }
    for (auto& p : pipelines_) vkDestroyPipeline(dev_, p.second->pipe, nullptr);
    for (auto& m : modules_) vkDestroyShaderModule(dev_, m.second, nullptr);
    for (auto& l : layouts_) vkDestroyPipelineLayout(dev_, l.second, nullptr);
    for (auto& l : dsls_) vkDestroyDescriptorSetLayout(dev_, l.second, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    if (cache_) vkDestroyPipelineCache(dev_, cache_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(dev_, cmdPool_, nullptr);
    vkDestroyDevice(dev_, nullptr);
  }
  if (inst_) vkDestroyInstance(inst_, nullptr);
}

std::vector<std::uint8_t> VkDev::pipelineCacheBlob() const {
  std::size_t n = 0;
  std::vector<std::uint8_t> v;
  if (!cache_ || vkGetPipelineCacheData(dev_, cache_, &n, nullptr) != VK_SUCCESS || !n) return v;
  v.resize(n);
  if (vkGetPipelineCacheData(dev_, cache_, &n, v.data()) != VK_SUCCESS) v.clear();
  return v;
}

std::uint32_t VkDev::findMem(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
  for (std::uint32_t i = 0; i < mem_.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & want) == want && !(mem_.memoryTypes[i].propertyFlags & avoid)) return i;
  return ~0u;
}

// ---- memory ----
Status VkDev::makeBuffer(VBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkResult r = vkCreateBuffer(dev_, &bi, nullptr, &b.buf);
  if (r != VK_SUCCESS) return fail(vkErr("vkCreateBuffer", r), Status::OutOfMemory), Status::OutOfMemory;
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev_, b.buf, &mr);
  std::uint32_t t = ~0u;
  bool coherent = true;
  if (kind == MemKind::DeviceLocal) {
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT);
  } else if (kind == MemKind::Readback) {
    // CPU reads: HOST_CACHED without HOST_COHERENT first (a normal write-back mapping, invalidated in wait()), then
    // cached+coherent. Adreno 620 only allows the cached+coherent type for buffers; reading it still runs at ~1 GB/s
    // (Pixel 5: 6.8 MB in 6-13 ms), see docs/engine/device-results/pixel5.md.
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    coherent = false;
    if (t == ~0u) {
      t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      coherent = true;
    }
  } else {
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  if (t == ~0u && kind != MemKind::DeviceLocal) {
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    coherent = true;
  }
  if (t == ~0u) return destroyBuf(b), fail("no suitable memory type for a buffer", Status::OutOfMemory), Status::OutOfMemory;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = t;
  if ((r = vkAllocateMemory(dev_, &ai, nullptr, &b.mem)) != VK_SUCCESS)
    return destroyBuf(b), fail(vkErr("vkAllocateMemory (buffer)", r), Status::OutOfMemory), Status::OutOfMemory;
  vkBindBufferMemory(dev_, b.buf, b.mem, 0);
  if (kind != MemKind::DeviceLocal) {
    if ((r = vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped)) != VK_SUCCESS)
      return destroyBuf(b), fail(vkErr("vkMapMemory", r), Status::OutOfMemory), Status::OutOfMemory;
  }
  b.size = size;
  b.coherent = coherent;
  b.id = nextId_++;
  return Status::Ok;
}

void VkDev::destroyBuf(VBuffer& b) {
  if (b.mapped) vkUnmapMemory(dev_, b.mem);
  if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
  if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
  b = {};
}

Status VkDev::createBuffer(const BufferDesc& d, BufferH* out) {
  auto* b = new VBuffer;
  const VkBufferUsageFlags usage = d.storage ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0;
  const Status s = makeBuffer(*b, d.bytes, usage, d.kind);
  if (s != Status::Ok) {
    delete b;
    return s;
  }
  if (d.kind == MemKind::Readback && !b->coherent) nonCoherentReadbacks_.push_back(b);
  out->id = b->id;
  out->impl = b;
  out->bytes = d.bytes;
  out->mapped = b->mapped;
  return Status::Ok;
}

void VkDev::destroy(BufferH& h) {
  if (!h.impl) return;
  auto* b = static_cast<VBuffer*>(h.impl);
  nonCoherentReadbacks_.erase(std::remove(nonCoherentReadbacks_.begin(), nonCoherentReadbacks_.end(), b), nonCoherentReadbacks_.end());
  destroyBuf(*b);
  delete b;
  h = {};
}

Status VkDev::createImage(const ImageDesc& d, ImageH* out) {
  if (d.w == 0 || d.h == 0 || d.w > limits_.maxImageDimension2D || d.h > limits_.maxImageDimension2D)
    return fail("image size outside the device limits", Status::InvalidArgument), Status::InvalidArgument;
  auto* im = new VImage;
  const VkFormat fmt = toVk(d.fmt);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = d.d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {d.w, d.h, d.d};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  auto bad = [&](const std::string& e, Status s) {
    destroyImg(*im);
    delete im;
    fail(e, s);
    return s;
  };
  VkResult r = vkCreateImage(dev_, &ci, nullptr, &im->img);
  if (r != VK_SUCCESS) return bad(vkErr("vkCreateImage", r), Status::OutOfMemory);
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev_, im->img, &mr);
  const std::uint32_t t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (t == ~0u) return bad("no device-local memory type", Status::OutOfMemory);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = t;
  if ((r = vkAllocateMemory(dev_, &ai, nullptr, &im->mem)) != VK_SUCCESS) return bad(vkErr("vkAllocateMemory (image)", r), Status::OutOfMemory);
  vkBindImageMemory(dev_, im->img, im->mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im->img;
  vi.viewType = d.d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if ((r = vkCreateImageView(dev_, &vi, nullptr, &im->view)) != VK_SUCCESS) return bad(vkErr("vkCreateImageView", r), Status::Internal);
  im->fmt = fmt;
  im->id = nextId_++;
  out->id = im->id;
  out->impl = im;
  out->fmt = d.fmt;
  out->w = d.w;
  out->h = d.h;
  out->d = d.d;
  out->bytes = mr.size;
  return Status::Ok;
}

void VkDev::destroyImg(VImage& im) {
  if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
  if (im.img) vkDestroyImage(dev_, im.img, nullptr);
  if (im.mem) vkFreeMemory(dev_, im.mem, nullptr);
  im = {};
}
void VkDev::destroy(ImageH& h) {
  if (!h.impl) return;
  auto* im = static_cast<VImage*>(h.impl);
  destroyImg(*im);
  delete im;
  h = {};
}

ImageH VkDev::dummyImage(Format f) {
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
  dummies_[int(f)] = static_cast<VImage*>(h.impl);
  return h;
}

void VkDev::flush(BufferH b, std::uint64_t off, std::uint64_t n) {
  auto* v = static_cast<VBuffer*>(b.impl);
  if (!v || v->coherent) return;
  VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  mr.memory = v->mem;
  mr.offset = off;
  mr.size = n ? n : VK_WHOLE_SIZE;
  vkFlushMappedMemoryRanges(dev_, 1, &mr);
}
void VkDev::invalidate(BufferH b, std::uint64_t, std::uint64_t) {
  auto* v = static_cast<VBuffer*>(b.impl);
  if (!v || v->coherent) return;
  VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  mr.memory = v->mem;
  mr.size = VK_WHOLE_SIZE;
  vkInvalidateMappedMemoryRanges(dev_, 1, &mr);
}

// ---- pipelines ----
Status VkDev::createPipeline(const PipelineDesc& pd, PipelineH* out) {
  std::lock_guard<std::mutex> lk(pipeMtx_);
  std::string key = pd.shader;
  for (auto& s : pd.spec) key += "|" + std::to_string(s.id) + "=" + std::to_string(s.value);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    out->impl = it->second.get();
    out->bindings = it->second->bindings.c_str();
    return Status::Ok;
  }
  const ShaderBin* bin = nullptr;
  for (std::size_t i = 0; i < kShaderCount; ++i)
    if (pd.shader == kShaders[i].name) bin = &kShaders[i];
  if (!bin) return fail("unknown shader " + pd.shader, Status::Unsupported), Status::Unsupported;
  if (!pd.bindings.empty() && pd.bindings != bin->bindings)
    return fail("binding signature of " + pd.shader + " differs from the shader table", Status::InvalidArgument), Status::InvalidArgument;

  VkShaderModule mod = VK_NULL_HANDLE;
  auto mi = modules_.find(pd.shader);
  if (mi != modules_.end()) {
    mod = mi->second;
  } else {
    std::vector<std::uint32_t> words((bin->size + 3) / 4);
    std::memcpy(words.data(), bin->data, bin->size);
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bin->size;
    si.pCode = words.data();
    VkResult r = vkCreateShaderModule(dev_, &si, nullptr, &mod);
    if (r != VK_SUCCESS) return fail(vkErr("vkCreateShaderModule", r), Status::Internal), Status::Internal;
    modules_[pd.shader] = mod;
  }
  const std::string bind = bin->bindings;
  VkDescriptorSetLayout dsl;
  auto di = dsls_.find(bind);
  if (di != dsls_.end()) {
    dsl = di->second;
  } else {
    std::vector<VkDescriptorSetLayoutBinding> b;
    b.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (std::size_t i = 0; i < bind.size(); ++i) {
      const VkDescriptorType t = bind[i] == 'I' ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                 : bind[i] == 'S' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                  : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      b.push_back({std::uint32_t(i + 1), t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = std::uint32_t(b.size());
    li.pBindings = b.data();
    VkResult r = vkCreateDescriptorSetLayout(dev_, &li, nullptr, &dsl);
    if (r != VK_SUCCESS) return fail(vkErr("vkCreateDescriptorSetLayout", r), Status::Internal), Status::Internal;
    dsls_[bind] = dsl;
  }
  VkPipelineLayout pl;
  auto li = layouts_.find(bind);
  if (li != layouts_.end()) {
    pl = li->second;
  } else {
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &dsl;
    VkResult r = vkCreatePipelineLayout(dev_, &pi, nullptr, &pl);
    if (r != VK_SUCCESS) return fail(vkErr("vkCreatePipelineLayout", r), Status::Internal), Status::Internal;
    layouts_[bind] = pl;
  }
  std::vector<VkSpecializationMapEntry> me;
  std::vector<std::int32_t> vals;
  for (auto& s : pd.spec) {
    me.push_back({s.id, std::uint32_t(vals.size() * 4), 4});
    vals.push_back(s.value);
  }
  VkSpecializationInfo sp{};
  sp.mapEntryCount = std::uint32_t(me.size());
  sp.pMapEntries = me.data();
  sp.dataSize = vals.size() * 4;
  sp.pData = vals.data();
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", me.empty() ? nullptr : &sp};
  ci.layout = pl;
  VkPipeline pipe;
  VkResult r = vkCreateComputePipelines(dev_, cache_, 1, &ci, nullptr, &pipe);
  if (r != VK_SUCCESS) return fail(vkErr(("vkCreateComputePipelines " + key).c_str(), r), Status::Internal), Status::Internal;
  auto p = std::make_unique<VPipeline>();
  p->pipe = pipe;
  p->layout = pl;
  p->dsl = dsl;
  p->bindings = bind;
  p->name = key;
  out->impl = p.get();
  out->bindings = p->bindings.c_str();
  pipelines_[key] = std::move(p);
  return Status::Ok;
}

// ---- command lists ----
VkCmd::VkCmd(VkDev& d, int slot) : d_(d), slot_(slot), s_(d.slots_[slot]), cb_(s_.cb) {
  vkResetCommandBuffer(cb_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb_, &bi);
  if (s_.qp) {
    vkCmdResetQueryPool(cb_, s_.qp, 0, 512);
    vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_.qp, std::uint32_t(tsIdx_++));
  }
  s_.passNames.clear();
  s_.st = {};
  memBarrier();  // everything from earlier submissions (and the host writes into staging / uniform memory)
}

void VkCmd::memBarrier() {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
  b.dstAccessMask = b.srcAccessMask;
  vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  ++s_.st.barriers;
}

void VkCmd::barrier(std::uint32_t) {
  // Images stay in GENERAL; one global memory barrier covers every class (what phase 1 did). Masks matter for GLES.
  memBarrier();
}

void VkCmd::init(VImage* im) {
  if (im->inited) return;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im->img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  im->inited = true;
}

Status VkCmd::setParams(const void* p, std::size_t n) {
  if (n > kUboRange) return fail(Status::InvalidArgument), d_.fail("parameter block too large", Status::InvalidArgument), Status::InvalidArgument;
  const std::size_t align = d_.caps_.uniformOffsetAlign;
  ring_ = (ring_ + align - 1) / align * align;
  if (ring_ + kUboRange > s_.params.size) return fail(Status::OutOfMemory), d_.fail("parameter ring exhausted", Status::OutOfMemory), Status::OutOfMemory;
  std::memcpy(static_cast<std::uint8_t*>(s_.params.mapped) + ring_, p, n);
  paramOff_ = ring_;
  ring_ += kUboRange;
  haveParams_ = true;
  return Status::Ok;
}

Status VkCmd::dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) {
  if (err_ != Status::Ok) return err_;
  if (!pl_ || !haveParams_) return fail(Status::InvalidArgument), Status::InvalidArgument;
  const int nb = int(pl_->bindings.size());
  std::vector<std::uint64_t> ids;
  ids.reserve(std::size_t(nb) + 2);
  ids.push_back(reinterpret_cast<std::uint64_t>(pl_));
  ids.push_back(s_.params.id);
  for (int i = 0; i < nb; ++i) {
    const auto& b = bound_[i];
    if (!b.img && !b.buf) return fail(Status::InvalidArgument), d_.fail("unbound slot in dispatch of " + pl_->name, Status::InvalidArgument), Status::InvalidArgument;
    ids.push_back(b.img ? b.img->id : b.buf->id);
  }
  VkDescriptorSet set = d_.descriptorFor(pl_, ids, &s_.params, bound_, nb);
  if (!set) return fail(Status::OutOfMemory), Status::OutOfMemory;
  const std::uint32_t dyn = std::uint32_t(paramOff_);
  vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, pl_->pipe);
  vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, pl_->layout, 0, 1, &set, 1, &dyn);
  vkCmdDispatch(cb_, gx, gy, gz);
  ++s_.st.passes;
  haveParams_ = false;
  return Status::Ok;
}

void VkCmd::copyBufferToImage(BufferH src, ImageH dst, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t off) {
  auto* im = static_cast<VImage*>(dst.impl);
  init(im);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.bufferOffset = off;
  c.imageExtent = {w, h, d};
  vkCmdCopyBufferToImage(cb_, static_cast<VBuffer*>(src.impl)->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
  ++s_.st.passes;
}
void VkCmd::copyBufferToBuffer(BufferH src, BufferH dst, std::uint64_t bytes, std::uint64_t off) {
  VkBufferCopy c{off, 0, bytes};
  vkCmdCopyBuffer(cb_, static_cast<VBuffer*>(src.impl)->buf, static_cast<VBuffer*>(dst.impl)->buf, 1, &c);
  ++s_.st.passes;
}
void VkCmd::copyImageToBuffer(ImageH src, BufferH dst, const Rect2& r, std::uint64_t off) {
  auto* im = static_cast<VImage*>(src.impl);
  init(im);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.bufferOffset = off;
  c.imageOffset = {std::int32_t(r.x), std::int32_t(r.y), 0};
  c.imageExtent = {r.w, r.h, 1};
  vkCmdCopyImageToBuffer(cb_, im->img, VK_IMAGE_LAYOUT_GENERAL, static_cast<VBuffer*>(dst.impl)->buf, 1, &c);
  ++s_.st.passes;
}
void VkCmd::copyImage(ImageH src, ImageH dst, std::uint32_t w, std::uint32_t h) {
  auto* a = static_cast<VImage*>(src.impl);
  auto* b = static_cast<VImage*>(dst.impl);
  init(a);
  init(b);
  VkImageCopy c{};
  c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.extent = {w, h, 1};
  vkCmdCopyImage(cb_, a->img, VK_IMAGE_LAYOUT_GENERAL, b->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
  ++s_.st.passes;
}

void VkCmd::timestamp(const char* label) {
  if (!s_.qp || tsIdx_ >= 511) return;
  s_.passNames.push_back(label ? label : "");
  vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_.qp, std::uint32_t(tsIdx_++));
}

Status VkCmd::flushPart() {
  if (err_ != Status::Ok) return err_;
  vkEndCommandBuffer(cb_);
  VkSubmitInfo sp{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  sp.commandBufferCount = 1;
  sp.pCommandBuffers = &cb_;
  const VkResult rr = vkQueueSubmit(d_.queue_, 1, &sp, VK_NULL_HANDLE);
  if (rr != VK_SUCCESS) {
    const Status st = rr == VK_ERROR_DEVICE_LOST ? Status::DeviceLost : Status::Internal;
    d_.fail(vkErr("vkQueueSubmit (split)", rr), st);
    return fail(st), st;
  }
  if (extraUsed_ == s_.extraCbs.size()) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d_.cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer nb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d_.dev_, &cai, &nb) != VK_SUCCESS) return d_.fail("vkAllocateCommandBuffers (split)", Status::OutOfMemory), fail(Status::OutOfMemory), Status::OutOfMemory;
    s_.extraCbs.push_back(nb);
  }
  cb_ = s_.extraCbs[extraUsed_++];
  vkResetCommandBuffer(cb_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb_, &bi);
  memBarrier();
  ++s_.st.parts;
  return Status::Ok;
}

void VkCmd::finish() {
  // make the results visible to the host (readback buffers, coherent or not)
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cb_);
}

// ---- descriptors / submission ----
VkDescriptorSet VkDev::descriptorFor(VPipeline* pl, const std::vector<std::uint64_t>& ids, VBuffer* ubo, const VkCmd::B* bound, int nb) {
  std::string key(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(std::uint64_t));
  auto it = sets_.find(key);
  if (it != sets_.end()) return it->second;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = dpool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &pl->dsl;
  VkDescriptorSet set;
  VkResult r = vkAllocateDescriptorSets(dev_, &ai, &set);
  if (r != VK_SUCCESS) return fail(vkErr("vkAllocateDescriptorSets (pool exhausted)", r), Status::OutOfMemory), VK_NULL_HANDLE;
  std::vector<VkWriteDescriptorSet> w;
  std::vector<VkDescriptorImageInfo> ii(static_cast<std::size_t>(nb) + 0u);
  std::vector<VkDescriptorBufferInfo> bi(static_cast<std::size_t>(nb) + 1u);
  VkWriteDescriptorSet z{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  bi[0] = {ubo->buf, 0, kUboRange};
  z.dstSet = set;
  z.dstBinding = 0;
  z.descriptorCount = 1;
  z.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  z.pBufferInfo = &bi[0];
  w.push_back(z);
  for (int i = 0; i < nb; ++i) {
    z = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    z.dstSet = set;
    z.dstBinding = std::uint32_t(i + 1);
    z.descriptorCount = 1;
    const char kind = pl->bindings[std::size_t(i)];
    if (kind == 'B') {
      bi[std::size_t(i) + 1] = {bound[i].buf->buf, 0, VK_WHOLE_SIZE};
      z.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      z.pBufferInfo = &bi[std::size_t(i) + 1];
    } else {
      ii[std::size_t(i)] = {kind == 'S' ? sampler_ : VK_NULL_HANDLE, bound[i].img->view, VK_IMAGE_LAYOUT_GENERAL};
      z.descriptorType = kind == 'S' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      z.pImageInfo = &ii[std::size_t(i)];
    }
    w.push_back(z);
  }
  vkUpdateDescriptorSets(dev_, std::uint32_t(w.size()), w.data(), 0, nullptr);
  sets_[key] = set;
  return set;
}

std::unique_ptr<CommandList> VkDev::begin() {
  if (lost_) return nullptr;
  // Descriptor sets are cached per (pipeline, resources); when the cache gets large (sizes changed a lot) start over. Only
  // here, between frames: a command buffer being recorded must not lose its sets.
  if (sets_.size() > 1400) trim();
  int idx = nextSlot_;
  nextSlot_ = (nextSlot_ + 1) % kSlots;
  if (idx == pinned_) {
    idx = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
  }
  Slot& s = slots_[idx];
  if (s.inFlight && waitSlot(s, 30ull * 1000 * 1000 * 1000) != Status::Ok) return nullptr;
  return std::make_unique<VkCmd>(*this, idx);
}

Status VkDev::submit(CommandList& cl, FenceH* signal) {
  if (lost_) return Status::DeviceLost;
  auto& c = static_cast<VkCmd&>(cl);
  if (c.err_ != Status::Ok) {
    vkEndCommandBuffer(c.cb_);
    return c.err_;
  }
  Slot& s = slots_[c.slot_];
  c.finish();
  vkResetFences(dev_, 1, &s.fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &c.cb_;
  const VkResult r = vkQueueSubmit(queue_, 1, &si, s.fence);
  if (r != VK_SUCCESS) {
    const Status st = r == VK_ERROR_DEVICE_LOST ? Status::DeviceLost : Status::Internal;
    fail(vkErr("vkQueueSubmit", r), st);
    return st;
  }
  s.inFlight = true;
  ++s.gen;
  if (signal) {
    signal->slot = std::uint32_t(c.slot_);
    signal->gen = s.gen;
  }
  return Status::Ok;
}

Status VkDev::waitSlot(Slot& s, std::uint64_t ns) {
  if (!s.inFlight) return Status::Ok;
  const VkResult r = vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, ns);
  if (r == VK_TIMEOUT) return fail("fence wait timed out", Status::Timeout), Status::Timeout;
  s.inFlight = false;
  if (r != VK_SUCCESS) {
    const Status st = r == VK_ERROR_DEVICE_LOST ? Status::DeviceLost : Status::Internal;
    fail(vkErr("vkWaitForFences", r), st);
    return st;
  }
  for (VBuffer* b : nonCoherentReadbacks_) {
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = b->mem;
    mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev_, 1, &mr);
  }
  s.times.clear();
  s.gpuMs = 0;
  if (s.qp && !s.passNames.empty()) {
    std::vector<std::uint64_t> v(s.passNames.size() + 1);
    if (vkGetQueryPoolResults(dev_, s.qp, 0, std::uint32_t(v.size()), v.size() * 8, v.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
      const std::uint64_t mask = tsBits_ >= 64 ? ~0ull : ((1ull << tsBits_) - 1);
      for (std::size_t i = 0; i < s.passNames.size(); ++i) {
        const double ms = double((v[i + 1] - v[i]) & mask) * tsPeriod_ * 1e-6;
        s.times.push_back({s.passNames[i], ms});
        s.gpuMs += ms;
        gpuAccum_ += ms;
      }
    }
  }
  return Status::Ok;
}

Status VkDev::wait(FenceH f, std::uint64_t ns) {
  if (!f || f.slot >= std::uint32_t(kSlots)) return Status::InvalidArgument;
  Slot& s = slots_[f.slot];
  if (s.gen != f.gen) return Status::Ok;  // the slot was reused: that submission finished long ago
  return waitSlot(s, ns);
}
bool VkDev::poll(FenceH f) {
  if (!f || f.slot >= std::uint32_t(kSlots)) return true;
  Slot& s = slots_[f.slot];
  if (s.gen != f.gen || !s.inFlight) return true;
  return vkGetFenceStatus(dev_, s.fence) == VK_SUCCESS;
}
Status VkDev::waitAll() {
  Status res = Status::Ok;
  for (Slot& s : slots_) {
    const Status st = waitSlot(s, 30ull * 1000 * 1000 * 1000);
    if (st != Status::Ok) res = st;
  }
  return res;
}
void VkDev::waitIdle() {
  if (dev_) vkDeviceWaitIdle(dev_);
  for (Slot& s : slots_) s.inFlight = false;
}
void VkDev::trim() {
  waitIdle();
  vkResetDescriptorPool(dev_, dpool_, 0);
  sets_.clear();
}

}  // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceOptions& opt, std::string& err) {
  auto d = std::make_unique<VkDev>();
  if (!d->init(opt, err)) return nullptr;
  return d;
}

}  // namespace dapple::hal::vk
