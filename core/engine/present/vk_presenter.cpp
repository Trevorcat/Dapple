// Vulkan swapchain presenter for an ANativeWindow (Android). No link-time Vulkan dependency: libvulkan.so is dlopen'ed and
// the entry points live in a private table (so this file can coexist with core/gpu's volk in one library).
#if defined(__ANDROID__)
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "presenter.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "dapple_present", __VA_ARGS__)

namespace dapple::present {
namespace {

struct Fn {
  PFN_vkGetInstanceProcAddr gipa = nullptr;
#define F(n) PFN_##n n = nullptr;
  F(vkCreateInstance) F(vkDestroyInstance) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties)
  F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceFormatProperties)
  F(vkEnumerateDeviceExtensionProperties) F(vkCreateDevice) F(vkGetDeviceProcAddr)
  F(vkCreateAndroidSurfaceKHR) F(vkDestroySurfaceKHR) F(vkGetPhysicalDeviceSurfaceSupportKHR)
  F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(vkGetPhysicalDeviceSurfaceFormatsKHR) F(vkGetPhysicalDeviceSurfacePresentModesKHR)
  // device level
  F(vkDestroyDevice) F(vkGetDeviceQueue) F(vkCreateSwapchainKHR) F(vkDestroySwapchainKHR) F(vkGetSwapchainImagesKHR)
  F(vkAcquireNextImageKHR) F(vkQueuePresentKHR) F(vkCreateCommandPool) F(vkDestroyCommandPool) F(vkAllocateCommandBuffers)
  F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkResetCommandBuffer) F(vkCmdPipelineBarrier) F(vkCmdCopyBufferToImage)
  F(vkCmdBlitImage) F(vkCreateFence) F(vkDestroyFence) F(vkWaitForFences) F(vkResetFences) F(vkCreateSemaphore)
  F(vkDestroySemaphore) F(vkQueueSubmit) F(vkDeviceWaitIdle) F(vkCreateBuffer) F(vkDestroyBuffer)
  F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) F(vkBindBufferMemory) F(vkMapMemory) F(vkUnmapMemory)
  F(vkFlushMappedMemoryRanges) F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory)
#undef F
};

double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

constexpr int kRing = 2;

class VkPresenter final : public Presenter {
 public:
  ~VkPresenter() override {
    std::lock_guard<std::mutex> l(m_);
    destroyAll();
    if (lib_) dlclose(lib_);
  }

  bool load(std::string& err) {
    lib_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) return err = "libvulkan.so not found", false;
    f_.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
    if (!f_.gipa) return err = "vkGetInstanceProcAddr missing", false;
    f_.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(f_.gipa(nullptr, "vkCreateInstance"));
    if (!f_.vkCreateInstance) return err = "vkCreateInstance missing", false;
    return true;
  }

  bool setWindow(void* window, int w, int h, std::string& err) override {
    std::lock_guard<std::mutex> l(m_);
    releaseWindowLocked();
    if (!window) return err = "null window", false;
    win_ = static_cast<ANativeWindow*>(window);
    ANativeWindow_acquire(win_);
    reqW_ = std::max(1, w);
    reqH_ = std::max(1, h);
    // build eagerly so a broken device is reported at attach time (the view then falls back to readback)
    if (!ensureDevice(err) || !ensureSwapchain(err)) {
      releaseWindowLocked();
      return false;
    }
    return true;
  }

  void clearWindow() override {
    std::lock_guard<std::mutex> l(m_);
    releaseWindowLocked();
  }

  bool hasWindow() const override {
    std::lock_guard<std::mutex> l(m_);
    return win_ != nullptr;
  }

  bool present(const std::uint8_t* rgba, int w, int h, PresentTiming* tm, std::string& err) override {
    std::lock_guard<std::mutex> l(m_);
    if (!win_) return err = "no window", false;
    if (w <= 0 || h <= 0) return err = "empty frame", false;
    const auto t0 = std::chrono::steady_clock::now();
    if (!ensureDevice(err) || !ensureSwapchain(err)) return false;
    Ring& r = ring_[cur_];
    f_.vkWaitForFences(dev_, 1, &r.fence, VK_TRUE, UINT64_C(2000000000));
    if (!ensureStaging(r, VkDeviceSize(w) * VkDeviceSize(h) * 4, err)) return false;
    if (!ensureFrameImage(w, h, err)) return false;
    std::memcpy(r.map, rgba, std::size_t(w) * std::size_t(h) * 4);
    if (!r.coherent) {
      VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      mr.memory = r.stageMem;
      mr.size = VK_WHOLE_SIZE;
      f_.vkFlushMappedMemoryRanges(dev_, 1, &mr);
    }
    const double upMs = msSince(t0);
    const auto t1 = std::chrono::steady_clock::now();
    std::uint32_t idx = 0;
    VkResult ar = f_.vkAcquireNextImageKHR(dev_, sc_, UINT64_C(100000000), r.acquire, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
      destroySwapchain();
      if (!ensureSwapchain(err)) return false;
      ar = f_.vkAcquireNextImageKHR(dev_, sc_, UINT64_C(100000000), r.acquire, VK_NULL_HANDLE, &idx);
    }
    if (ar == VK_ERROR_SURFACE_LOST_KHR) {
      destroySwapchain();
      destroySurface();
      return err = "surface lost", false;
    }
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY) return err = "acquire timeout (consumer stalled)", false;
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return fail(err, "vkAcquireNextImageKHR", ar);
    const double acqMs = msSince(t1);
    const auto t2 = std::chrono::steady_clock::now();

    f_.vkResetFences(dev_, 1, &r.fence);
    f_.vkResetCommandBuffer(r.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f_.vkBeginCommandBuffer(r.cb, &bi);
    barrier(r.cb, fimg_, fimgLayout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {std::uint32_t(w), std::uint32_t(h), 1};
    f_.vkCmdCopyBufferToImage(r.cb, r.stage, fimg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    barrier(r.cb, fimg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    fimgLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier(r.cb, imgs_[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {w, h, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {std::int32_t(ext_.width), std::int32_t(ext_.height), 1};
    const bool same = std::uint32_t(w) == ext_.width && std::uint32_t(h) == ext_.height;
    f_.vkCmdBlitImage(r.cb, fimg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                      same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier(r.cb, imgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    f_.vkEndCommandBuffer(r.cb);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &r.acquire;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &r.cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done_[idx];
    VkResult sr = f_.vkQueueSubmit(q_, 1, &si, r.fence);
    if (sr == VK_ERROR_DEVICE_LOST) {
      loseDevice();
      return err = "device lost", false;
    }
    if (sr != VK_SUCCESS) return fail(err, "vkQueueSubmit", sr);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done_[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc_;
    pi.pImageIndices = &idx;
    VkResult pr = f_.vkQueuePresentKHR(q_, &pi);
    cur_ = (cur_ + 1) % kRing;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
      stale_ = true;  // recreate on the next frame (this one was queued)
    } else if (pr == VK_ERROR_SURFACE_LOST_KHR) {
      destroySwapchain();
      destroySurface();
      return err = "surface lost", false;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
      loseDevice();
      return err = "device lost", false;
    } else if (pr != VK_SUCCESS) {
      return fail(err, "vkQueuePresentKHR", pr);
    }
    ++n_;
    if (tm) {
      tm->uploadMs = upMs;
      tm->acquireMs = acqMs;
      tm->submitMs = msSince(t2);
    }
    return true;
  }

  void trim(int level) override {
    std::lock_guard<std::mutex> l(m_);
    if (!dev_) return;
    f_.vkDeviceWaitIdle(dev_);
    destroyStagingAndFrame();
    if (level >= 1) destroySwapchain();
    if (level >= 2) {
      destroySurface();
      destroyDevice();
    }
  }

  std::string info() const override {
    std::lock_guard<std::mutex> l(m_);
    return "{\"variant\":\"vk-swapchain\",\"device\":\"" + devName_ + "\",\"deviceType\":" + std::to_string(devType_) +
           ",\"api\":" + std::to_string(api_) + ",\"format\":" + std::to_string(int(fmt_)) + ",\"extent\":[" +
           std::to_string(ext_.width) + "," + std::to_string(ext_.height) + "],\"images\":" + std::to_string(imgs_.size()) +
           ",\"presentMode\":" + std::to_string(int(mode_)) + ",\"presented\":" + std::to_string(n_.load()) +
           ",\"preTransform\":" + std::to_string(preTransform_) + "}";
  }
  std::uint64_t presented() const override { return n_.load(); }

 private:
  struct Ring {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    VkDeviceSize stageSize = 0;
    void* map = nullptr;
    bool coherent = true;
  };

  bool fail(std::string& err, const char* what, VkResult r) {
    err = std::string(what) + " failed: " + std::to_string(int(r));
    return false;
  }

  void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags s0,
               VkPipelineStageFlags s1, VkAccessFlags a0, VkAccessFlags a1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = a0;
    b.dstAccessMask = a1;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f_.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  int findMem(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) {
    for (std::uint32_t i = 0; i < mem_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & want) == want && !(mem_.memoryTypes[i].propertyFlags & avoid))
        return int(i);
    return -1;
  }

  // ---- instance / surface / device ---------------------------------------------------------------------------
  bool ensureInstance(std::string& err) {
    if (inst_) return true;
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "dapple";
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VkResult r = f_.vkCreateInstance(&ci, nullptr, &inst_);
    if (r != VK_SUCCESS) {
      inst_ = VK_NULL_HANDLE;
      return fail(err, "vkCreateInstance(surface exts)", r);
    }
#define L(n) f_.n = reinterpret_cast<PFN_##n>(f_.gipa(inst_, #n))
    L(vkDestroyInstance); L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties); L(vkGetPhysicalDeviceFormatProperties);
    L(vkEnumerateDeviceExtensionProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkCreateAndroidSurfaceKHR);
    L(vkDestroySurfaceKHR); L(vkGetPhysicalDeviceSurfaceSupportKHR); L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    L(vkGetPhysicalDeviceSurfaceFormatsKHR); L(vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef L
    return true;
  }

  bool ensureSurface(std::string& err) {
    if (surf_) return true;
    if (!win_) return err = "no window", false;
    if (!ensureInstance(err)) return false;
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = win_;
    VkResult r = f_.vkCreateAndroidSurfaceKHR(inst_, &ci, nullptr, &surf_);
    if (r != VK_SUCCESS) {
      surf_ = VK_NULL_HANDLE;
      return fail(err, "vkCreateAndroidSurfaceKHR", r);
    }
    return true;
  }

  bool ensureDevice(std::string& err) {
    if (!ensureSurface(err)) return false;
    if (dev_) return true;
    std::uint32_t n = 0;
    f_.vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    f_.vkEnumeratePhysicalDevices(inst_, &n, pds.data());
    int bestScore = -1;
    for (VkPhysicalDevice pd : pds) {
      std::uint32_t en = 0;
      f_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
      std::vector<VkExtensionProperties> ex(en);
      f_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ex.data());
      bool sw = false;
      for (auto& e : ex) sw = sw || std::strcmp(e.extensionName, "VK_KHR_swapchain") == 0;
      if (!sw) continue;
      std::uint32_t qn = 0;
      f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qf(qn);
      f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
      int fam = -1;
      for (std::uint32_t i = 0; i < qn && fam < 0; ++i) {
        VkBool32 sup = VK_FALSE;
        f_.vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf_, &sup);
        if (sup && (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) fam = int(i);
      }
      if (fam < 0) continue;
      VkPhysicalDeviceProperties pp;
      f_.vkGetPhysicalDeviceProperties(pd, &pp);
      const int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                        : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                        : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                        : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                   : 1;
      if (score > bestScore) {
        bestScore = score;
        phys_ = pd;
        family_ = std::uint32_t(fam);
        devName_ = pp.deviceName;
        devType_ = int(pp.deviceType);
        api_ = pp.apiVersion;
      }
    }
    if (bestScore < 0) return err = "no Vulkan device can present to this window", false;
    f_.vkGetPhysicalDeviceMemoryProperties(phys_, &mem_);
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dexts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dexts;
    VkResult r = f_.vkCreateDevice(phys_, &di, nullptr, &dev_);
    if (r != VK_SUCCESS) {
      dev_ = VK_NULL_HANDLE;
      return fail(err, "vkCreateDevice", r);
    }
#define D(n) f_.n = reinterpret_cast<PFN_##n>(f_.vkGetDeviceProcAddr(dev_, #n))
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateSwapchainKHR); D(vkDestroySwapchainKHR); D(vkGetSwapchainImagesKHR);
    D(vkAcquireNextImageKHR); D(vkQueuePresentKHR); D(vkCreateCommandPool); D(vkDestroyCommandPool); D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkResetCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdCopyBufferToImage);
    D(vkCmdBlitImage); D(vkCreateFence); D(vkDestroyFence); D(vkWaitForFences); D(vkResetFences); D(vkCreateSemaphore);
    D(vkDestroySemaphore); D(vkQueueSubmit); D(vkDeviceWaitIdle); D(vkCreateBuffer); D(vkDestroyBuffer);
    D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkFreeMemory); D(vkBindBufferMemory); D(vkMapMemory);
    D(vkUnmapMemory); D(vkFlushMappedMemoryRanges); D(vkCreateImage); D(vkDestroyImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory);
#undef D
    f_.vkGetDeviceQueue(dev_, family_, 0, &q_);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = family_;
    f_.vkCreateCommandPool(dev_, &pi, nullptr, &pool_);
    for (Ring& rg : ring_) {
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ai.commandPool = pool_;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      f_.vkAllocateCommandBuffers(dev_, &ai, &rg.cb);
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      f_.vkCreateFence(dev_, &fi, nullptr, &rg.fence);
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      f_.vkCreateSemaphore(dev_, &sci, nullptr, &rg.acquire);
    }
    LOGI("device %s type %d api %u family %u", devName_.c_str(), devType_, api_, family_);
    return true;
  }

  // ---- swapchain -----------------------------------------------------------------------------------------------
  bool ensureSwapchain(std::string& err) {
    if (sc_ && !stale_) return true;
    if (!ensureSurface(err)) return false;
    if (sc_) destroySwapchain();
    stale_ = false;
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = f_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surf_, &caps);
    if (r != VK_SUCCESS) return fail(err, "surface capabilities", r);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return err = "surface lacks TRANSFER_DST usage", false;
    std::uint32_t fn = 0;
    f_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surf_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fm(fn);
    f_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surf_, &fn, fm.data());
    if (fm.empty()) return err = "no surface formats", false;
    VkSurfaceFormatKHR pick = fm[0];
    bool found = false;
    for (auto& f : fm)
      if (!found && f.format == VK_FORMAT_R8G8B8A8_UNORM) { pick = f; found = true; }
    for (auto& f : fm)
      if (!found && f.format == VK_FORMAT_B8G8R8A8_UNORM) { pick = f; found = true; }
    VkFormatProperties fp;
    f_.vkGetPhysicalDeviceFormatProperties(phys_, pick.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return err = "swapchain format cannot be a blit target", false;
    fmt_ = pick.format;
    VkExtent2D ex = caps.currentExtent;
    if (ex.width == 0xFFFFFFFFu) {
      ex.width = std::clamp<std::uint32_t>(std::uint32_t(reqW_), caps.minImageExtent.width, caps.maxImageExtent.width);
      ex.height = std::clamp<std::uint32_t>(std::uint32_t(reqH_), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ex.width == 0 || ex.height == 0) return err = "zero sized surface", false;
    mode_ = VK_PRESENT_MODE_FIFO_KHR;  // guaranteed; on Android the BufferQueue behind it paces to the consumer
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surf_;
    std::uint32_t want = std::max<std::uint32_t>(3, caps.minImageCount);
    if (caps.maxImageCount) want = std::min(want, caps.maxImageCount);
    ci.minImageCount = want;
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = ex;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    preTransform_ = int(caps.currentTransform);
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode_;
    ci.clipped = VK_TRUE;
    r = f_.vkCreateSwapchainKHR(dev_, &ci, nullptr, &sc_);
    if (r != VK_SUCCESS) {
      sc_ = VK_NULL_HANDLE;
      return fail(err, "vkCreateSwapchainKHR", r);
    }
    ext_ = ex;
    std::uint32_t in = 0;
    f_.vkGetSwapchainImagesKHR(dev_, sc_, &in, nullptr);
    imgs_.resize(in);
    f_.vkGetSwapchainImagesKHR(dev_, sc_, &in, imgs_.data());
    done_.resize(in);
    for (auto& s : done_) {
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      f_.vkCreateSemaphore(dev_, &sci, nullptr, &s);
    }
    LOGI("swapchain %ux%u fmt %d images %u transform %d", ex.width, ex.height, int(pick.format), in, preTransform_);
    return true;
  }

  void destroySwapchain() {
    if (!dev_) return;
    f_.vkDeviceWaitIdle(dev_);
    for (auto s : done_) f_.vkDestroySemaphore(dev_, s, nullptr);
    done_.clear();
    imgs_.clear();
    if (sc_) f_.vkDestroySwapchainKHR(dev_, sc_, nullptr);
    sc_ = VK_NULL_HANDLE;
    // an interrupted frame may have left an acquire semaphore signalled: recreate them
    for (Ring& rg : ring_) {
      if (rg.acquire) f_.vkDestroySemaphore(dev_, rg.acquire, nullptr);
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      f_.vkCreateSemaphore(dev_, &sci, nullptr, &rg.acquire);
    }
  }

  void destroySurface() {
    if (surf_ && inst_) f_.vkDestroySurfaceKHR(inst_, surf_, nullptr);
    surf_ = VK_NULL_HANDLE;
  }

  // ---- staging / frame image ----------------------------------------------------------------------------------
  bool ensureStaging(Ring& rg, VkDeviceSize bytes, std::string& err) {
    if (rg.stageSize >= bytes) return true;
    f_.vkDeviceWaitIdle(dev_);
    freeStaging(rg);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes + bytes / 4;  // headroom: frame sizes vary with the quality rung
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult res = f_.vkCreateBuffer(dev_, &bi, nullptr, &rg.stage);
    if (res != VK_SUCCESS) return fail(err, "staging buffer", res);
    VkMemoryRequirements mr;
    f_.vkGetBufferMemoryRequirements(dev_, rg.stage, &mr);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, co = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int t = findMem(mr.memoryTypeBits, hv | co, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);  // write-combined: fastest for a one-way copy
    if (t < 0) t = findMem(mr.memoryTypeBits, hv | co);
    if (t < 0) t = findMem(mr.memoryTypeBits, hv);
    if (t < 0) return err = "no host visible memory", false;
    rg.coherent = (mem_.memoryTypes[t].propertyFlags & co) != 0;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = std::uint32_t(t);
    res = f_.vkAllocateMemory(dev_, &ai, nullptr, &rg.stageMem);
    if (res != VK_SUCCESS) return fail(err, "staging memory", res);
    f_.vkBindBufferMemory(dev_, rg.stage, rg.stageMem, 0);
    res = f_.vkMapMemory(dev_, rg.stageMem, 0, VK_WHOLE_SIZE, 0, &rg.map);
    if (res != VK_SUCCESS) return fail(err, "map staging", res);
    rg.stageSize = bi.size;
    return true;
  }

  void freeStaging(Ring& rg) {
    if (rg.map) f_.vkUnmapMemory(dev_, rg.stageMem);
    if (rg.stage) f_.vkDestroyBuffer(dev_, rg.stage, nullptr);
    if (rg.stageMem) f_.vkFreeMemory(dev_, rg.stageMem, nullptr);
    rg.stage = VK_NULL_HANDLE;
    rg.stageMem = VK_NULL_HANDLE;
    rg.map = nullptr;
    rg.stageSize = 0;
  }

  bool ensureFrameImage(int w, int h, std::string& err) {
    if (fimg_ && fw_ == w && fh_ == h) return true;
    f_.vkDeviceWaitIdle(dev_);
    freeFrameImage();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {std::uint32_t(w), std::uint32_t(h), 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = f_.vkCreateImage(dev_, &ii, nullptr, &fimg_);
    if (r != VK_SUCCESS) {
      fimg_ = VK_NULL_HANDLE;
      return fail(err, "frame image", r);
    }
    VkMemoryRequirements mr;
    f_.vkGetImageMemoryRequirements(dev_, fimg_, &mr);
    int t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = findMem(mr.memoryTypeBits, 0);
    if (t < 0) return err = "no memory for the frame image", false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = std::uint32_t(t);
    r = f_.vkAllocateMemory(dev_, &ai, nullptr, &fmem_);
    if (r != VK_SUCCESS) {
      fmem_ = VK_NULL_HANDLE;
      return fail(err, "frame image memory", r);
    }
    f_.vkBindImageMemory(dev_, fimg_, fmem_, 0);
    fw_ = w;
    fh_ = h;
    fimgLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
  }

  void freeFrameImage() {
    if (fimg_) f_.vkDestroyImage(dev_, fimg_, nullptr);
    if (fmem_) f_.vkFreeMemory(dev_, fmem_, nullptr);
    fimg_ = VK_NULL_HANDLE;
    fmem_ = VK_NULL_HANDLE;
    fw_ = fh_ = 0;
  }

  void destroyStagingAndFrame() {
    for (Ring& rg : ring_) freeStaging(rg);
    freeFrameImage();
  }

  // ---- teardown ------------------------------------------------------------------------------------------------
  void destroyDevice() {
    if (!dev_) return;
    f_.vkDeviceWaitIdle(dev_);
    destroyStagingAndFrame();
    destroySwapchain();
    for (Ring& rg : ring_) {
      if (rg.fence) f_.vkDestroyFence(dev_, rg.fence, nullptr);
      if (rg.acquire) f_.vkDestroySemaphore(dev_, rg.acquire, nullptr);
      rg = Ring{};
    }
    if (pool_) f_.vkDestroyCommandPool(dev_, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    f_.vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
    q_ = VK_NULL_HANDLE;
  }

  // The device is unusable (lost): drop everything that belongs to it, keep the window; the next frame rebuilds.
  void loseDevice() {
    for (Ring& rg : ring_) {
      rg.map = nullptr;  // memory died with the device
    }
    // best effort teardown; calls on a lost device return errors but must not crash
    destroyDevice();
    destroySurface();
  }

  void releaseWindowLocked() {
    if (dev_) destroySwapchain();
    destroySurface();
    if (win_) ANativeWindow_release(win_);
    win_ = nullptr;
  }

  void destroyAll() {
    destroyDevice();
    destroySurface();
    if (inst_ && f_.vkDestroyInstance) f_.vkDestroyInstance(inst_, nullptr);
    inst_ = VK_NULL_HANDLE;
    if (win_) ANativeWindow_release(win_);
    win_ = nullptr;
  }

  mutable std::mutex m_;
  void* lib_ = nullptr;
  Fn f_;
  VkInstance inst_ = VK_NULL_HANDLE;
  VkSurfaceKHR surf_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue q_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_{};
  std::uint32_t family_ = 0;
  std::string devName_;
  int devType_ = 0;
  std::uint32_t api_ = 0;
  ANativeWindow* win_ = nullptr;
  int reqW_ = 1, reqH_ = 1;
  VkSwapchainKHR sc_ = VK_NULL_HANDLE;
  std::vector<VkImage> imgs_;
  std::vector<VkSemaphore> done_;
  VkExtent2D ext_{};
  VkFormat fmt_ = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
  int preTransform_ = 0;
  bool stale_ = false;
  Ring ring_[kRing];
  int cur_ = 0;
  VkImage fimg_ = VK_NULL_HANDLE;
  VkDeviceMemory fmem_ = VK_NULL_HANDLE;
  int fw_ = 0, fh_ = 0;
  VkImageLayout fimgLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  std::atomic<std::uint64_t> n_{0};
};

}  // namespace

std::unique_ptr<Presenter> makeVulkanPresenter(std::string& err) {
  auto p = std::make_unique<VkPresenter>();
  if (!p->load(err)) return nullptr;
  return p;
}

}  // namespace dapple::present
#endif
