#pragma once
// Internal: the Vulkan layer of the GPU backend. Device selection + capability probe + blocklist, memory, a pool of
// working images (no per-frame allocation), pipelines (SPIR-V embedded at build time, specialised by constants),
// cached descriptor sets, and the pass graph that records one command buffer per frame.
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dapple/gpu.h"
#include "volk.h"

namespace dapple::gpu::vk {

enum class Mem { DeviceLocal, Upload /* host visible, write combined is fine */, Readback /* host cached */ };

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
  bool coherent = true;
  std::uint64_t id = 0;
  explicit operator bool() const { return buf != VK_NULL_HANDLE; }
};

struct Image {
  VkImage img = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  std::uint32_t w = 0, h = 0, d = 1;
  std::uint64_t id = 0, bytes = 0, lastUse = 0;
  bool inited = false;  // moved out of UNDEFINED into GENERAL (done lazily in the first command buffer that uses it)
  explicit operator bool() const { return img != VK_NULL_HANDLE; }
};

struct Pipeline {
  VkPipeline pipe = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  std::string bindings;  // one letter per binding from 1: I storage image, S sampled image + sampler, B storage buffer
  std::string name;
};

// Resource handle inside a graph.
using H = int;

class Engine;

class Graph {
 public:
  explicit Graph(Engine& e) : e_(e) {}
  ~Graph();
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  H external(Image* im);
  H externalBuf(Buffer* b);
  // Pooled working image of (at least) w x h; the allocation is rounded up (or `aw` x `ah` when given) so frames of
  // slightly different size share pool entries. Kernels take the real size from their parameters.
  H transient(VkFormat fmt, std::uint32_t w, std::uint32_t h, std::uint32_t aw = 0, std::uint32_t ah = 0);
  Image* image(H h) const { return res_[h].img; }

  struct Use {
    H res;
    bool write = false;  // images / buffers bound read-write count as both
    bool read = true;
  };
  static Use R(H h) { return {h, false, true}; }
  static Use W(H h) { return {h, true, false}; }
  static Use RW(H h) { return {h, true, true}; }

  // A compute pass: `uses` in binding order (binding 1...), params as a uniform block (binding 0).
  void dispatch(const char* name, Pipeline* pl, std::initializer_list<Use> uses, std::uint32_t gx, std::uint32_t gy,
                const void* params, std::size_t paramBytes, std::uint32_t gz = 1);
  // staging -> image copy (whole buffer rows packed tightly) / image region -> buffer
  void copyBufferToImage(Buffer* src, H dst, std::uint32_t w, std::uint32_t h, std::uint32_t d = 1, VkDeviceSize srcOffset = 0);
  void copyBufferToBuffer(Buffer* src, H dst, VkDeviceSize bytes, VkDeviceSize srcOffset = 0);
  void copyImageToBuffer(H src, Buffer* dst, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
  void copyImage(H src, H dst, std::uint32_t w, std::uint32_t h);

 private:
  friend class Engine;
  struct Res {
    Image* img = nullptr;
    Buffer* buf = nullptr;
    bool pooled = false;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    std::uint32_t w = 0, h = 0;
    int last = -1;
  };
  struct Node {
    enum Kind { Dispatch, BufToImg, BufToBuf, ImgToBuf, ImgToImg } kind = Dispatch;
    std::string name;
    Pipeline* pl = nullptr;
    std::vector<Use> uses;
    std::uint32_t gx = 0, gy = 0, gz = 1;
    std::vector<std::uint8_t> params;
    // copies
    H a = -1, b = -1;
    Buffer* buf = nullptr;
    std::uint32_t x = 0, y = 0, w = 0, h = 0, d = 1;
    VkDeviceSize bytes = 0, off = 0;
  };
  Engine& e_;
  std::vector<Res> res_;
  std::vector<Node> nodes_;
};

struct FrameSlot {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> extraCbs;  // continuation buffers of a frame that is submitted in pieces
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool qp = VK_NULL_HANDLE;
  bool inFlight = false;
  Buffer params;    // uniform block ring
  Buffer weights;   // filter weights (storage)
  Buffer staging;   // host -> device uploads (grown on demand)
  Buffer readback;  // device -> host (grown on demand)
  std::vector<std::string> passNames;
  int passes = 0, barriers = 0;
  double gpuMs = 0;
  std::vector<PassTime> times;
};

struct ShaderBin;

class Engine {
 public:
  static std::unique_ptr<Engine> create(const BackendOptions& opt, std::string& err);
  ~Engine();

  const DeviceInfo& info() const { return info_; }
  const VkPhysicalDeviceLimits& limits() const { return limits_; }
  bool profile() const { return opt_.profile; }
  const BackendOptions& options() const { return opt_; }

  // ---- resources ----
  bool createBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, Mem kind, std::string& err);
  void destroy(Buffer& b);
  bool createImage(Image& im, VkFormat fmt, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::string& err);
  void destroy(Image& im);
  Image* acquire(VkFormat fmt, std::uint32_t w, std::uint32_t h, std::string& err);
  void release(Image* im);
  std::size_t pooledBytes() const { return pooledBytes_; }
  std::size_t freeBytes() const;
  void trimPool(std::size_t keepFreeBytes);
  std::size_t poolBudget() const { return budget_; }
  // allocation size for a requested dimension: a multiple of 32 (128 above 1024), within the device limit
  std::uint32_t bucket(std::uint32_t v) const;
  static std::uint32_t bytesPerTexel(VkFormat f);

  // ---- pipelines ----
  // `spec` = (constant id, value) pairs. Null (err set) when the shader does not exist or fails to build.
  Pipeline* pipeline(const char* shader, std::initializer_list<std::pair<std::uint32_t, std::int32_t>> spec,
                     std::string& err);
  std::vector<std::uint8_t> pipelineCacheData() const;

  // ---- frames ----
  FrameSlot* acquireSlot(std::string& err);  // waits for the slot's previous work
  void pinSlot(FrameSlot* s) { pinned_ = s; }  // a slot whose readback is still being consumed: not handed out
  // Records and submits the graph on `slot`; the work is complete once wait(slot) returned.
  bool submit(Graph& g, FrameSlot& slot, std::string& err);
  bool wait(FrameSlot& slot, std::string& err);
  bool ensureStaging(FrameSlot& slot, VkDeviceSize bytes, std::string& err);
  bool ensureReadback(FrameSlot& slot, VkDeviceSize bytes, std::string& err);
  void waitIdle();
  bool waitAll(std::string& err);  // every submitted frame finished (and its timestamps read)
  double gpuAccum() const { return gpuAccum_; }  // sum of all pass timestamps read so far (profile only)
  bool deviceLost() const { return lost_; }

  // Uploads packed float RGB rows [y0, y0+h) x [x0, x0+w) of `rgb` (row pitch `pitchFloats`) into `dst` at (dx, dy).
  bool uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, Image& dst, int dx, int dy,
                    std::string& err);

  static constexpr int kSlots = 3;
  static constexpr std::size_t kParamRing = 512 * 1024;
  static constexpr std::size_t kUboRange = 256;

  VkDevice device() const { return dev_; }
  Image* dummyImage(VkFormat fmt);  // 1x1 image for bindings a shader variant never touches

 private:
  Engine() = default;
  bool init(const BackendOptions& opt, std::string& err);
  VkDescriptorSet descriptorFor(Pipeline* pl, const std::vector<std::uint64_t>& ids, Buffer* ubo,
                                const std::vector<std::pair<Image*, Buffer*>>& bound, std::string& err);
  std::uint32_t findMem(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;
  bool slotInit(FrameSlot& s, std::string& err);
  void slotDestroy(FrameSlot& s);

  BackendOptions opt_;
  VkInstance inst_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t family_ = 0;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_{};
  VkPhysicalDeviceLimits limits_{};
  DeviceInfo info_;
  float tsPeriod_ = 1.f;
  std::uint32_t tsBits_ = 0;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;
  std::unordered_map<std::string, VkDescriptorSet> sets_;
  std::size_t setCount_ = 0;
  std::unordered_map<std::string, VkShaderModule> modules_;
  std::unordered_map<std::string, std::unique_ptr<Pipeline>> pipelines_;
  std::unordered_map<std::string, VkDescriptorSetLayout> dsls_;
  std::unordered_map<std::string, VkPipelineLayout> layouts_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Image>> dummies_;

  std::vector<std::unique_ptr<Image>> images_;  // all pooled images (stable addresses)
  std::vector<Image*> free_;
  std::size_t pooledBytes_ = 0, budget_ = 0;
  double splitGroups_ = 250000;  // weighted workgroups per submission (env DAPPLE_GPU_SPLIT_GROUPS, 0 = never split)
  std::uint64_t tick_ = 0, nextId_ = 1;

  FrameSlot slots_[kSlots];
  int nextSlot_ = 0;
  FrameSlot* pinned_ = nullptr;
  bool lost_ = false;
  double gpuAccum_ = 0;
  friend class Graph;
};

// the SPIR-V table generated by CMake (shader_table.inc)
struct ShaderBin {
  const char* name;
  const unsigned char* data;
  std::size_t size;
  const char* bindings;
};
extern const ShaderBin kShaders[];
extern const std::size_t kShaderCount;

// ---- blocklist -----------------------------------------------------------------------------------------------
struct BlockRule {
  std::uint32_t vendor = 0;      // 0 = any
  std::string name;              // substring of the device name, empty = any
  std::string driver;            // substring of the driver name / info, empty = any
  std::uint32_t driverMin = 0, driverMax = 0xffffffffu;  // raw VkPhysicalDeviceProperties::driverVersion
  std::string reason;
};
std::vector<BlockRule> parseBlocklist(const std::string& text);
// Empty when the device is acceptable, else the reason.
std::string blockReason(const DeviceInfo& di, const std::vector<BlockRule>& rules, bool allowSoftware);

}  // namespace dapple::gpu::vk
