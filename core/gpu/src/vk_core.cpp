// Vulkan layer of the GPU backend: see vk_core.h. Raw Vulkan 1.0 + volk. Memory is one allocation per resource but
// every working image comes from a pool keyed by (format, size), so a steady-state frame allocates nothing.
#include "vk_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>



namespace dapple::gpu {
namespace vk {

#include "shader_table.inc"

namespace {
std::string vkErr(const char* what, VkResult r) {
  char b[96];
  std::snprintf(b, sizeof b, "%s failed (VkResult %d)", what, int(r));
  return b;
}
}  // namespace

// ---------------------------------------------------------------------------------------------- blocklist ---
std::vector<BlockRule> parseBlocklist(const std::string& text) {
  std::vector<BlockRule> rules;
  std::string line;
  auto flush = [&] {
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      line.clear();
      return;
    }
    BlockRule r;
    std::stringstream ss(line);
    std::string kv;
    while (std::getline(ss, kv, ';')) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      if (k == "vendor") r.vendor = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "name") r.name = v;
      else if (k == "driver") r.driver = v;
      else if (k == "drvmin") r.driverMin = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "drvmax") r.driverMax = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "reason") r.reason = v;
    }
    rules.push_back(std::move(r));
    line.clear();
  };
  for (char c : text) {
    if (c == '\n' || c == '|') flush();
    else line.push_back(c);
  }
  flush();
  return rules;
}

std::string blockReason(const DeviceInfo& di, const std::vector<BlockRule>& rules, bool allowSoftware) {
  if (di.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !allowSoftware)
    return "software Vulkan implementation (" + di.name + ")";
  for (const BlockRule& r : rules) {
    if (r.vendor && r.vendor != di.vendorId) continue;
    if (!r.name.empty() && di.name.find(r.name) == std::string::npos) continue;
    if (!r.driver.empty() && di.driver.find(r.driver) == std::string::npos) continue;
    if (di.driverVersion < r.driverMin || di.driverVersion > r.driverMax) continue;
    return "blocklisted: " + (r.reason.empty() ? std::string("no reason given") : r.reason);
  }
  return {};
}

// Built-in rules (name, driver and version ranges of devices with known bad compute drivers). Kept as text so it is the
// same format as the user supplied list. Empty for now: no device has failed our self-test yet; the self-test is the
// primary gate and extra rules can be shipped with a data update (BackendOptions::blocklist).
static const char* kBuiltinBlocklist = "";

// ---------------------------------------------------------------------------------------------- engine ---
std::uint32_t Engine::bytesPerTexel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: return 4;
    default: return 4;
  }
}

std::uint32_t Engine::findMem(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
  for (std::uint32_t i = 0; i < mem_.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & want) == want &&
        !(mem_.memoryTypes[i].propertyFlags & avoid))
      return i;
  return ~0u;
}

std::unique_ptr<Engine> Engine::create(const BackendOptions& opt, std::string& err) {
  std::unique_ptr<Engine> e(new Engine);
  if (!e->init(opt, err)) return nullptr;
  return e;
}

bool Engine::init(const BackendOptions& opt, std::string& err) {
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

  std::vector<BlockRule> rules = parseBlocklist(kBuiltinBlocklist);
  for (const std::string& extra : {opt.blocklist, std::string(std::getenv("DAPPLE_GPU_BLOCKLIST") ? std::getenv("DAPPLE_GPU_BLOCKLIST") : "")})
    for (BlockRule& b : parseBlocklist(extra)) rules.push_back(std::move(b));
  int forced = opt.deviceIndex;
  if (const char* env = std::getenv("DAPPLE_GPU_DEVICE")) forced = std::atoi(env);

  struct Cand {
    int idx, score;
    std::uint32_t fam;
    DeviceInfo di;
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
    DeviceInfo di;
    di.name = p.deviceName;
    di.apiVersion = p.apiVersion;
    di.driverVersion = p.driverVersion;
    di.vendorId = p.vendorID;
    di.deviceId = p.deviceID;
    di.deviceType = p.deviceType;
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
  info_ = pick.di;

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys_, &props);
  limits_ = props.limits;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_);
  info_.maxImageDim2D = limits_.maxImageDimension2D;
  info_.maxImageDim3D = limits_.maxImageDimension3D;
  info_.maxSharedMem = limits_.maxComputeSharedMemorySize;
  info_.maxInvocations = limits_.maxComputeWorkGroupInvocations;
  for (std::uint32_t i = 0; i < mem_.memoryHeapCount; ++i)
    if (mem_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      info_.deviceLocalBytes = std::max<std::uint64_t>(info_.deviceLocalBytes, mem_.memoryHeaps[i].size);
  for (std::uint32_t i = 0; i < mem_.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem_.memoryTypes[i].propertyFlags;
    char b[160];
    std::snprintf(b, sizeof b, "  memtype %u heap %u (%.0f MB%s): %s%s%s%s%s\n", i, mem_.memoryTypes[i].heapIndex,
                  double(mem_.memoryHeaps[mem_.memoryTypes[i].heapIndex].size) / 1048576.0,
                  (mem_.memoryHeaps[mem_.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", device-local heap" : "",
                  (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
                  (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "", (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
                  (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZY " : "");
    info_.memoryTypes += b;
  }
  info_.unifiedMemory = info_.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || info_.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
  info_.linearFilterRgba32f = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  // the working formats must support storage + sampled + transfer use
  for (VkFormat f : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM}) {
    vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // transfer feature bits only exist from 1.1 (and are implied before), so only require storage + sampled there
    const VkFormatFeatureFlags must = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    (void)need;
    if ((fp.optimalTilingFeatures & must) != must) return err = "device lacks storage image support for a working format", false;
  }
  if (limits_.maxPerStageDescriptorStorageImages < 4 || limits_.maxComputeSharedMemorySize < 16384)
    return err = "device limits below what the kernels need (storage images per stage " + std::to_string(limits_.maxPerStageDescriptorStorageImages) + ", shared memory " + std::to_string(limits_.maxComputeSharedMemorySize) + ")", false;

  if (props.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    info_.subgroupSize = sg.subgroupSize;
    info_.subgroupOps = sg.supportedOperations;
    VkPhysicalDevice16BitStorageFeatures s16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &s16;
    vkGetPhysicalDeviceFeatures2(phys_, &f2);
    info_.storage16 = s16.storageBuffer16BitAccess != 0;
  }
  {
    std::uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ex(en);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, ex.data());
    for (auto& x : ex)
      if (!std::strcmp(x.extensionName, "VK_KHR_shader_float16_int8")) info_.fp16Arith = true;
  }
  std::uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
  tsBits_ = qf[family_].timestampValidBits;
  info_.timestamps = tsBits_ > 0 && limits_.timestampComputeAndGraphics;
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
                                : std::size_t(std::min<std::uint64_t>(std::max<std::uint64_t>(info_.deviceLocalBytes / 4, 256u << 20), 1024u << 20));
  if (const char* sg = std::getenv("DAPPLE_GPU_SPLIT_GROUPS")) splitGroups_ = std::atof(sg) > 0 ? std::atof(sg) : 1e30;
  for (FrameSlot& s : slots_)
    if (!slotInit(s, err)) return false;
  return true;
}

bool Engine::slotInit(FrameSlot& s, std::string& err) {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = cmdPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkResult r = vkAllocateCommandBuffers(dev_, &ai, &s.cb);
  if (r != VK_SUCCESS) return err = vkErr("vkAllocateCommandBuffers", r), false;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if ((r = vkCreateFence(dev_, &fi, nullptr, &s.fence)) != VK_SUCCESS) return err = vkErr("vkCreateFence", r), false;
  if (opt_.profile && info_.timestamps) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 512;
    vkCreateQueryPool(dev_, &qi, nullptr, &s.qp);
  }
  if (!createBuffer(s.params, kParamRing + kUboRange, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Mem::Upload, err)) return false;
  if (!createBuffer(s.weights, 16384, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Mem::Upload, err)) return false;
  return createBuffer(s.staging, 8u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Mem::Upload, err);
}

void Engine::slotDestroy(FrameSlot& s) {
  destroy(s.params);
  destroy(s.weights);
  destroy(s.staging);
  destroy(s.readback);
  if (s.qp) vkDestroyQueryPool(dev_, s.qp, nullptr);
  if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
  if (s.cb) vkFreeCommandBuffers(dev_, cmdPool_, 1, &s.cb);
  if (!s.extraCbs.empty()) vkFreeCommandBuffers(dev_, cmdPool_, std::uint32_t(s.extraCbs.size()), s.extraCbs.data());
  s = FrameSlot{};
}

Engine::~Engine() {
  if (dev_) {
    vkDeviceWaitIdle(dev_);
    if (!opt_.pipelineCachePath.empty() && cache_) {
      const auto blob = pipelineCacheData();
      if (!blob.empty()) {
        std::ofstream f(opt_.pipelineCachePath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
      }
    }
    for (FrameSlot& s : slots_) slotDestroy(s);
    for (auto& i : images_) destroy(*i);
    for (auto& d : dummies_) destroy(*d.second);
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

std::vector<std::uint8_t> Engine::pipelineCacheData() const {
  std::size_t n = 0;
  std::vector<std::uint8_t> v;
  if (!cache_ || vkGetPipelineCacheData(dev_, cache_, &n, nullptr) != VK_SUCCESS || !n) return v;
  v.resize(n);
  if (vkGetPipelineCacheData(dev_, cache_, &n, v.data()) != VK_SUCCESS) v.clear();
  return v;
}

// ---- memory ----
bool Engine::createBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, Mem kind, std::string& err) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkResult r = vkCreateBuffer(dev_, &bi, nullptr, &b.buf);
  if (r != VK_SUCCESS) return err = vkErr("vkCreateBuffer", r), false;
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev_, b.buf, &mr);
  std::uint32_t t = ~0u;
  bool coherent = true;
  if (kind == Mem::DeviceLocal) {
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT);
  } else if (kind == Mem::Readback) {
    // CPU reads: HOST_CACHED without HOST_COHERENT first (a normal write-back mapping, invalidated in wait()), then
    // cached+coherent. Adreno 620 only allows the cached+coherent type (memtype 2) for buffers; reading it still runs at
    // ~1 GB/s (Pixel 5: 6.8 MB in 6-13 ms vs 1 ms for an ordinary heap copy), see docs/engine/device-results/pixel5.md.
    {
      t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      coherent = false;
      if (t == ~0u) {
        t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        coherent = true;
      }
    }
  } else {
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  if (t == ~0u && kind != Mem::DeviceLocal)
    t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (t == ~0u) return err = "no suitable memory type for a buffer", destroy(b), false;
  if (std::getenv("DAPPLE_GPU_DEBUG_MEM")) std::fprintf(stderr, "buffer kind %d size %llu bits 0x%x -> type %u\n", int(kind), (unsigned long long)mr.size, mr.memoryTypeBits, t);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = t;
  if ((r = vkAllocateMemory(dev_, &ai, nullptr, &b.mem)) != VK_SUCCESS) return err = vkErr("vkAllocateMemory (buffer)", r), destroy(b), false;
  vkBindBufferMemory(dev_, b.buf, b.mem, 0);
  if (kind != Mem::DeviceLocal) {
    if ((r = vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped)) != VK_SUCCESS) return err = vkErr("vkMapMemory", r), destroy(b), false;
  }
  b.size = size;
  b.coherent = coherent;
  b.id = nextId_++;
  return true;
}

void Engine::destroy(Buffer& b) {
  if (b.mapped) vkUnmapMemory(dev_, b.mem);
  if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
  if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
  b = {};
}

bool Engine::createImage(Image& im, VkFormat fmt, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::string& err) {
  if (w == 0 || h == 0 || w > limits_.maxImageDimension2D || h > limits_.maxImageDimension2D)
    return err = "image size outside the device limits", false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {w, h, d};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult r = vkCreateImage(dev_, &ci, nullptr, &im.img);
  if (r != VK_SUCCESS) return err = vkErr("vkCreateImage", r), false;
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev_, im.img, &mr);
  const std::uint32_t t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (t == ~0u) return err = "no device-local memory type", destroy(im), false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = t;
  if ((r = vkAllocateMemory(dev_, &ai, nullptr, &im.mem)) != VK_SUCCESS) return err = vkErr("vkAllocateMemory (image)", r), destroy(im), false;
  vkBindImageMemory(dev_, im.img, im.mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im.img;
  vi.viewType = d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if ((r = vkCreateImageView(dev_, &vi, nullptr, &im.view)) != VK_SUCCESS) return err = vkErr("vkCreateImageView", r), destroy(im), false;
  im.fmt = fmt;
  im.w = w;
  im.h = h;
  im.d = d;
  im.bytes = mr.size;
  im.id = nextId_++;
  im.inited = false;
  return true;
}

void Engine::destroy(Image& im) {
  if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
  if (im.img) vkDestroyImage(dev_, im.img, nullptr);
  if (im.mem) vkFreeMemory(dev_, im.mem, nullptr);
  im = {};
}

Image* Engine::acquire(VkFormat fmt, std::uint32_t w, std::uint32_t h, std::string& err) {
  for (std::size_t i = 0; i < free_.size(); ++i) {
    Image* im = free_[i];
    if (im->fmt == fmt && im->w == w && im->h == h) {
      free_.erase(free_.begin() + std::ptrdiff_t(i));
      return im;
    }
  }
  auto im = std::make_unique<Image>();
  if (!createImage(*im, fmt, w, h, 1, err)) return nullptr;
  pooledBytes_ += im->bytes;
  images_.push_back(std::move(im));
  return images_.back().get();
}

void Engine::release(Image* im) {
  im->lastUse = ++tick_;
  free_.push_back(im);
}

std::size_t Engine::freeBytes() const {
  std::size_t n = 0;
  for (const Image* i : free_) n += i->bytes;
  return n;
}

void Engine::trimPool(std::size_t keepFree) {
  if (freeBytes() <= keepFree) return;
  // freed images may still be referenced by the last submitted frame
  waitIdle();
  std::sort(free_.begin(), free_.end(), [](const Image* a, const Image* b) { return a->lastUse > b->lastUse; });  // newest first
  std::size_t kept = 0;
  std::vector<Image*> keep, drop;
  for (Image* i : free_) {
    if (kept + i->bytes <= keepFree) {
      kept += i->bytes;
      keep.push_back(i);
    } else {
      drop.push_back(i);
    }
  }
  free_ = keep;
  for (Image* d : drop) {
    pooledBytes_ -= d->bytes;
    destroy(*d);
    images_.erase(std::remove_if(images_.begin(), images_.end(), [&](const std::unique_ptr<Image>& u) { return u.get() == d; }), images_.end());
  }
  // descriptor sets that named the dropped views are dead weight now
  if (!drop.empty()) {
    vkResetDescriptorPool(dev_, dpool_, 0);
    sets_.clear();
    setCount_ = 0;
  }
}

bool Engine::waitAll(std::string& err) {
  for (FrameSlot& s : slots_)
    if (s.inFlight && !wait(s, err)) return false;
  return true;
}

std::uint32_t Engine::bucket(std::uint32_t v) const {
  const std::uint32_t q = v >= 1024 ? 128 : 32;
  return std::min((v + q - 1) / q * q, limits_.maxImageDimension2D);
}

Image* Engine::dummyImage(VkFormat fmt) {
  auto it = dummies_.find(std::uint32_t(fmt));
  if (it != dummies_.end()) return it->second.get();
  auto im = std::make_unique<Image>();
  std::string err;
  if (!createImage(*im, fmt, 1, 1, 1, err)) return nullptr;
  Image* p = im.get();
  dummies_[std::uint32_t(fmt)] = std::move(im);
  return p;
}

void Engine::waitIdle() {
  if (dev_) vkDeviceWaitIdle(dev_);
  for (FrameSlot& s : slots_) s.inFlight = false;
}

// ---- pipelines ----
Pipeline* Engine::pipeline(const char* shader, std::initializer_list<std::pair<std::uint32_t, std::int32_t>> spec, std::string& err) {
  std::string key = shader;
  for (auto& s : spec) key += "|" + std::to_string(s.first) + "=" + std::to_string(s.second);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second.get();
  const ShaderBin* bin = nullptr;
  for (std::size_t i = 0; i < kShaderCount; ++i)
    if (!std::strcmp(kShaders[i].name, shader)) bin = &kShaders[i];
  if (!bin) return err = std::string("unknown shader ") + shader, nullptr;

  VkShaderModule mod = VK_NULL_HANDLE;
  auto mi = modules_.find(shader);
  if (mi != modules_.end()) {
    mod = mi->second;
  } else {
    std::vector<std::uint32_t> words((bin->size + 3) / 4);
    std::memcpy(words.data(), bin->data, bin->size);
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bin->size;
    si.pCode = words.data();
    VkResult r = vkCreateShaderModule(dev_, &si, nullptr, &mod);
    if (r != VK_SUCCESS) return err = vkErr("vkCreateShaderModule", r), nullptr;
    modules_[shader] = mod;
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
    if (r != VK_SUCCESS) return err = vkErr("vkCreateDescriptorSetLayout", r), nullptr;
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
    if (r != VK_SUCCESS) return err = vkErr("vkCreatePipelineLayout", r), nullptr;
    layouts_[bind] = pl;
  }
  std::vector<VkSpecializationMapEntry> me;
  std::vector<std::int32_t> vals;
  for (auto& s : spec) {
    me.push_back({s.first, std::uint32_t(vals.size() * 4), 4});
    vals.push_back(s.second);
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
  if (r != VK_SUCCESS) return err = vkErr(("vkCreateComputePipelines " + key).c_str(), r), nullptr;
  auto p = std::make_unique<Pipeline>();
  p->pipe = pipe;
  p->layout = pl;
  p->dsl = dsl;
  p->bindings = bind;
  p->name = key;
  Pipeline* out = p.get();
  pipelines_[key] = std::move(p);
  return out;
}

// ---- graph ----
Graph::~Graph() = default;

H Graph::external(Image* im) {
  Res r;
  r.img = im;
  res_.push_back(r);
  return H(res_.size() - 1);
}
H Graph::externalBuf(Buffer* b) {
  Res r;
  r.buf = b;
  res_.push_back(r);
  return H(res_.size() - 1);
}
H Graph::transient(VkFormat fmt, std::uint32_t w, std::uint32_t h, std::uint32_t aw, std::uint32_t ah) {
  Res r;
  r.pooled = true;
  r.fmt = fmt;
  r.w = aw >= w ? aw : e_.bucket(w);
  r.h = ah >= h ? ah : e_.bucket(h);
  res_.push_back(r);
  return H(res_.size() - 1);
}
void Graph::dispatch(const char* name, Pipeline* pl, std::initializer_list<Use> uses, std::uint32_t gx, std::uint32_t gy,
                     const void* params, std::size_t paramBytes, std::uint32_t gz) {
  Node n;
  n.kind = Node::Dispatch;
  n.name = name;
  n.pl = pl;
  n.uses.assign(uses.begin(), uses.end());
  n.gx = gx;
  n.gy = gy;
  n.gz = gz;
  n.params.assign(static_cast<const std::uint8_t*>(params), static_cast<const std::uint8_t*>(params) + paramBytes);
  nodes_.push_back(std::move(n));
}
void Graph::copyBufferToImage(Buffer* src, H dst, std::uint32_t w, std::uint32_t h, std::uint32_t d, VkDeviceSize srcOffset) {
  Node n;
  n.kind = Node::BufToImg;
  n.name = "upload";
  n.buf = src;
  n.b = dst;
  n.w = w;
  n.h = h;
  n.d = d;
  n.off = srcOffset;
  nodes_.push_back(std::move(n));
}
void Graph::copyBufferToBuffer(Buffer* src, H dst, VkDeviceSize bytes, VkDeviceSize srcOffset) {
  Node n;
  n.kind = Node::BufToBuf;
  n.name = "upload";
  n.buf = src;
  n.b = dst;
  n.bytes = bytes;
  n.off = srcOffset;
  nodes_.push_back(std::move(n));
}
void Graph::copyImageToBuffer(H src, Buffer* dst, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  Node n;
  n.kind = Node::ImgToBuf;
  n.name = "readback";
  n.a = src;
  n.buf = dst;
  n.x = x;
  n.y = y;
  n.w = w;
  n.h = h;
  nodes_.push_back(std::move(n));
}

void Graph::copyImage(H src, H dst, std::uint32_t w, std::uint32_t h) {
  Node n;
  n.kind = Node::ImgToImg;
  n.name = "copy";
  n.a = src;
  n.b = dst;
  n.w = w;
  n.h = h;
  nodes_.push_back(std::move(n));
}

// ---- frames ----
FrameSlot* Engine::acquireSlot(std::string& err) {
  FrameSlot* pick = &slots_[nextSlot_];
  nextSlot_ = (nextSlot_ + 1) % kSlots;
  if (pick == pinned_) {
    pick = &slots_[nextSlot_];
    nextSlot_ = (nextSlot_ + 1) % kSlots;
  }
  FrameSlot& s = *pick;
  if (s.inFlight && !wait(s, err)) return nullptr;
  return &s;
}

bool Engine::ensureStaging(FrameSlot& s, VkDeviceSize bytes, std::string& err) {
  if (s.staging.size >= bytes) return true;
  destroy(s.staging);
  return createBuffer(s.staging, std::max<VkDeviceSize>(bytes, 8u << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Mem::Upload, err);
}
bool Engine::ensureReadback(FrameSlot& s, VkDeviceSize bytes, std::string& err) {
  if (s.readback.size >= bytes) return true;
  destroy(s.readback);
  return createBuffer(s.readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Mem::Readback, err);
}

VkDescriptorSet Engine::descriptorFor(Pipeline* pl, const std::vector<std::uint64_t>& ids, Buffer* ubo,
                                      const std::vector<std::pair<Image*, Buffer*>>& bound, std::string& err) {
  std::string key(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(std::uint64_t));
  auto it = sets_.find(key);
  if (it != sets_.end()) return it->second;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = dpool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &pl->dsl;
  VkDescriptorSet set;
  VkResult r = vkAllocateDescriptorSets(dev_, &ai, &set);
  if (r != VK_SUCCESS) return err = vkErr("vkAllocateDescriptorSets (pool exhausted)", r), VK_NULL_HANDLE;
  std::vector<VkWriteDescriptorSet> w;
  std::vector<VkDescriptorImageInfo> ii(bound.size());
  std::vector<VkDescriptorBufferInfo> bi(bound.size() + 1);
  VkWriteDescriptorSet z{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  bi[0] = {ubo->buf, 0, kUboRange};
  z.dstSet = set;
  z.dstBinding = 0;
  z.descriptorCount = 1;
  z.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  z.pBufferInfo = &bi[0];
  w.push_back(z);
  for (std::size_t i = 0; i < bound.size(); ++i) {
    z = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    z.dstSet = set;
    z.dstBinding = std::uint32_t(i + 1);
    z.descriptorCount = 1;
    const char kind = pl->bindings[i];
    if (kind == 'B') {
      bi[i + 1] = {bound[i].second->buf, 0, VK_WHOLE_SIZE};
      z.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      z.pBufferInfo = &bi[i + 1];
    } else {
      ii[i] = {kind == 'S' ? sampler_ : VK_NULL_HANDLE, bound[i].first->view, VK_IMAGE_LAYOUT_GENERAL};
      z.descriptorType = kind == 'S' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      z.pImageInfo = &ii[i];
    }
    w.push_back(z);
  }
  vkUpdateDescriptorSets(dev_, std::uint32_t(w.size()), w.data(), 0, nullptr);
  sets_[key] = set;
  return set;
}

bool Engine::submit(Graph& g, FrameSlot& s, std::string& err) {
  if (lost_) return err = "device lost", false;
  const auto& nodes = g.nodes_;
  auto& res = g.res_;

  // Schedule: the recorded order is a valid one; passes of independent chains (texture / sharpen / clarity analysis, ...)
  // are regrouped into dependency levels so each level needs one barrier instead of one per pass. Dependencies are
  // read-after-write, write-after-read and write-after-write on the same resource.
  std::vector<int> order(nodes.size());
  {
    auto keyOf = [&](H h) -> std::uint64_t {
      const auto& r = res[h];
      return r.img ? r.img->id : r.buf ? r.buf->id : (1ull << 62) | std::uint64_t(h);
    };
    std::unordered_map<std::uint64_t, int> lastWriter;
    std::unordered_map<std::uint64_t, std::vector<int>> readers;
    std::vector<int> level(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const Graph::Node& n = nodes[i];
      std::vector<std::pair<std::uint64_t, std::pair<bool, bool>>> acc;
      auto add = [&](H h, bool rd, bool wr) { acc.push_back({keyOf(h), {rd, wr}}); };
      if (n.kind == Graph::Node::Dispatch) {
        for (const auto& u : n.uses) add(u.res, u.read, u.write);
      } else if (n.kind == Graph::Node::ImgToBuf) {
        add(n.a, true, false);
      } else if (n.kind == Graph::Node::ImgToImg) {
        add(n.a, true, false);
        add(n.b, false, true);
      } else {
        add(n.b, false, true);
      }
      int lv = 0;
      for (auto& a : acc) {
        auto w = lastWriter.find(a.first);
        if (w != lastWriter.end() && w->second != int(i)) lv = std::max(lv, level[w->second] + 1);
        if (a.second.second)
          for (int r : readers[a.first])
            if (r != int(i)) lv = std::max(lv, level[r] + 1);
      }
      level[i] = lv;
      for (auto& a : acc) {
        if (a.second.second) {
          lastWriter[a.first] = int(i);
          readers[a.first].clear();
        }
        if (a.second.first) readers[a.first].push_back(int(i));
      }
    }
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return level[a] < level[b]; });
  }

  // last use of every resource
  for (auto& r : res) r.last = -1;
  auto touch = [&](H h, int i) {
    if (h >= 0) res[h].last = i;
  };
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Graph::Node& n = nodes[order[i]];
    for (const auto& u : n.uses) touch(u.res, int(i));
    touch(n.a, int(i));
    touch(n.b, int(i));
  }
  if (nodes.size() + 1 > 512) return err = "too many passes in one frame", false;

  // Descriptor sets are cached per (pipeline, resources); when the cache gets large (sizes changed a lot) start over. Only
  // here, before recording: a command buffer being recorded must not lose its sets.
  if (sets_.size() > 1400) {
    vkDeviceWaitIdle(dev_);
    for (FrameSlot& o : slots_) o.inFlight = false;
    vkResetDescriptorPool(dev_, dpool_, 0);
    sets_.clear();
  }
  // The slot's UBO ring is written now: the slot has been waited for by acquireSlot().
  VkCommandBuffer cb = s.cb;
  std::size_t extraUsed = 0;
  double workSince = 0;  // groups dispatched since the last submit (weighted)
  vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  const bool ts = s.qp != VK_NULL_HANDLE;
  if (ts) vkCmdResetQueryPool(cb, s.qp, 0, 512);
  s.passNames.clear();
  s.passes = s.barriers = 0;

  auto memBarrier = [&] {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    b.dstAccessMask = b.srcAccessMask;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  };
  memBarrier();  // everything from earlier submissions (and the host writes into the staging / uniform memory)
  ++s.barriers;

  std::vector<std::uint64_t> dirty, readSince;
  auto has = [](const std::vector<std::uint64_t>& v, std::uint64_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
  std::size_t ring = 0;
  std::string e2;
  int tsIdx = 0;
  if (ts) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s.qp, std::uint32_t(tsIdx++));
  auto resolve = [&](H h) -> bool {
    auto& r = res[h];
    if (r.pooled && !r.img) {
      r.img = acquire(r.fmt, r.w, r.h, e2);
      if (!r.img) return false;
    }
    if (r.img && !r.img->inited) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = r.img->img;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      r.img->inited = true;
    }
    return true;
  };
  auto idOf = [&](H h) -> std::uint64_t { return res[h].img ? res[h].img->id : res[h].buf->id; };

  for (std::size_t ni = 0; ni < nodes.size(); ++ni) {
    const Graph::Node& n = nodes[order[ni]];
    std::vector<std::pair<H, std::pair<bool, bool>>> acc;  // handle, (read, write)
    if (n.kind == Graph::Node::Dispatch) {
      for (const auto& u : n.uses) acc.push_back({u.res, {u.read, u.write}});
    } else if (n.kind == Graph::Node::ImgToBuf) {
      acc.push_back({n.a, {true, false}});
    } else if (n.kind == Graph::Node::ImgToImg) {
      acc.push_back({n.a, {true, false}});
      acc.push_back({n.b, {false, true}});
    } else {
      acc.push_back({n.b, {false, true}});
    }
    for (auto& a : acc)
      if (!resolve(a.first)) return err = e2, vkEndCommandBuffer(cb), false;
    bool need = false;
    for (auto& a : acc) {
      const std::uint64_t id = idOf(a.first);
      if (has(dirty, id) || (a.second.second && has(readSince, id))) need = true;
    }
    if (need) {
      memBarrier();
      dirty.clear();
      readSince.clear();
      ++s.barriers;
    }
    for (auto& a : acc) {
      const std::uint64_t id = idOf(a.first);
      if (a.second.second) dirty.push_back(id);
      if (a.second.first && !has(readSince, id)) readSince.push_back(id);
    }

    switch (n.kind) {
      case Graph::Node::Dispatch: {
        if (n.params.size() > kUboRange) return err = "parameter block too large", vkEndCommandBuffer(cb), false;
        const std::size_t align = std::max<std::size_t>(256, limits_.minUniformBufferOffsetAlignment);
        ring = (ring + align - 1) / align * align;
        if (ring + kUboRange > s.params.size) return err = "parameter ring exhausted", vkEndCommandBuffer(cb), false;
        std::memcpy(static_cast<std::uint8_t*>(s.params.mapped) + ring, n.params.data(), n.params.size());
        std::vector<std::uint64_t> ids;
        ids.push_back(reinterpret_cast<std::uint64_t>(n.pl));
        ids.push_back(s.params.id);
        std::vector<std::pair<Image*, Buffer*>> bound;
        for (const auto& u : n.uses) {
          bound.push_back({res[u.res].img, res[u.res].buf});
          ids.push_back(idOf(u.res));
        }
        VkDescriptorSet set = descriptorFor(n.pl, ids, &s.params, bound, err);
        if (!set) return vkEndCommandBuffer(cb), false;
        const std::uint32_t dyn = std::uint32_t(ring);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, n.pl->pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, n.pl->layout, 0, 1, &set, 1, &dyn);
        vkCmdDispatch(cb, n.gx, n.gy, n.gz);
        ring += kUboRange;
        break;
      }
      case Graph::Node::BufToImg: {
        Image* im = res[n.b].img;
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.bufferOffset = n.off;
        c.imageExtent = {n.w, n.h, n.d};
        vkCmdCopyBufferToImage(cb, n.buf->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        break;
      }
      case Graph::Node::BufToBuf: {
        VkBufferCopy c{n.off, 0, n.bytes};
        vkCmdCopyBuffer(cb, n.buf->buf, res[n.b].buf->buf, 1, &c);
        break;
      }
      case Graph::Node::ImgToImg: {
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {n.w, n.h, 1};
        vkCmdCopyImage(cb, res[n.a].img->img, VK_IMAGE_LAYOUT_GENERAL, res[n.b].img->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        break;
      }
      case Graph::Node::ImgToBuf: {
        Image* im = res[n.a].img;
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {std::int32_t(n.x), std::int32_t(n.y), 0};
        c.imageExtent = {n.w, n.h, 1};
        vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, n.buf->buf, 1, &c);
        break;
      }
    }
    ++s.passes;
    s.passNames.push_back(n.name);
    if (ts) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s.qp, std::uint32_t(tsIdx++));
    if (n.kind == Graph::Node::Dispatch) {
      const bool slow = n.name == "box" || n.name == "gauss" || n.name == "min";
      workSince += double(n.gx) * n.gy * n.gz * (slow ? 2.0 : 1.0);
    }
    // Long frames are cut into several submissions. Android GPU drivers (Adreno kgsl hang detection) treat a single
    // submission that keeps the GPU busy for more than ~2-3 s as a hang, reset the GPU and replay it (24 MP frames took
    // minutes on a Pixel 5). Each piece stays well under a second.
    if (workSince >= splitGroups_ && ni + 1 < nodes.size()) {
      vkEndCommandBuffer(cb);
      VkSubmitInfo sp{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      sp.commandBufferCount = 1;
      sp.pCommandBuffers = &cb;
      const VkResult rr = vkQueueSubmit(queue_, 1, &sp, VK_NULL_HANDLE);
      if (rr != VK_SUCCESS) {
        if (rr == VK_ERROR_DEVICE_LOST) lost_ = true;
        return err = vkErr("vkQueueSubmit (split)", rr), false;
      }
      if (extraUsed == s.extraCbs.size()) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmdPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer nb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev_, &cai, &nb) != VK_SUCCESS) return err = "vkAllocateCommandBuffers (split)", false;
        s.extraCbs.push_back(nb);
      }
      cb = s.extraCbs[extraUsed++];
      vkResetCommandBuffer(cb, 0);
      vkBeginCommandBuffer(cb, &bi);
      memBarrier();
      ++s.barriers;
      dirty.clear();
      readSince.clear();
      workSince = 0;
    }
    for (auto& r : res)
      if (r.pooled && r.img && r.last == int(ni)) {
        release(r.img);
        r.img = nullptr;
        r.last = -2;
      }
  }
  // make the results visible to the host (readback buffers, coherent or not)
  {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  vkEndCommandBuffer(cb);
  vkResetFences(dev_, 1, &s.fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult r = vkQueueSubmit(queue_, 1, &si, s.fence);
  if (r != VK_SUCCESS) {
    if (r == VK_ERROR_DEVICE_LOST) lost_ = true;
    return err = vkErr("vkQueueSubmit", r), false;
  }
  s.inFlight = true;
  return true;
}

bool Engine::wait(FrameSlot& s, std::string& err) {
  if (!s.inFlight) return true;
  const VkResult r = vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, 30ull * 1000 * 1000 * 1000);
  s.inFlight = false;
  if (r != VK_SUCCESS) {
    if (r == VK_ERROR_DEVICE_LOST) lost_ = true;
    return err = vkErr("vkWaitForFences", r), false;
  }
  if (s.readback.mapped && !s.readback.coherent) {
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = s.readback.mem;
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
  return true;
}

bool Engine::uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, Image& dst, int dx, int dy, std::string& err) {
  Pipeline* pl = pipeline("convert", {}, err);
  if (!pl) return false;
  const std::size_t rowBytes = std::size_t(w) * 12;
  const std::size_t chunkBytes = 24u << 20;
  const int rows = int(std::max<std::size_t>(1, std::min<std::size_t>(std::size_t(h), chunkBytes / rowBytes)));
  std::vector<FrameSlot*> used;
  for (int y = 0; y < h; y += rows) {
    const int n = std::min(rows, h - y);
    FrameSlot* s = acquireSlot(err);
    if (s && std::find(used.begin(), used.end(), s) == used.end()) used.push_back(s);
    if (!s || !ensureStaging(*s, rowBytes * std::size_t(n), err)) return false;
    auto* dstp = static_cast<std::uint8_t*>(s->staging.mapped);
    if (pitchFloats == std::size_t(w) * 3) {
      std::memcpy(dstp, rgb + std::size_t(y) * pitchFloats, rowBytes * std::size_t(n));
    } else {
      for (int j = 0; j < n; ++j) std::memcpy(dstp + std::size_t(j) * rowBytes, rgb + std::size_t(y + j) * pitchFloats, rowBytes);
    }
    Graph g(*this);
    const H in = g.externalBuf(&s->staging), out = g.external(&dst);
    const std::int32_t p[4] = {w, n, dx, dy + y};
    g.dispatch("convert", pl, {Graph::R(in), Graph::W(out)}, std::uint32_t((w + 15) / 16), std::uint32_t((n + 15) / 16), p, sizeof p);
    if (!submit(g, *s, err)) return false;
  }
  for (FrameSlot* s : used)
    if (s->inFlight && !wait(*s, err)) return false;
  return true;
}

}  // namespace vk
}  // namespace dapple::gpu
