#pragma once
// Stage (d): where a finished frame goes. A Presenter owns the connection to a window (the Surface of a Flutter
// SurfaceProducer / TextureRegistry entry) and puts frames on it without the pixels ever visiting Dart.
//
// Variants (decision, measured on the emulator in docs/engine/present.md):
//  * VkSwapchain (chosen): ANativeWindow -> VK_KHR_android_surface -> swapchain. The window is the producer end of the
//    ImageReader that backs the Flutter texture; the swapchain images ARE the texture's buffers (no copy by Flutter).
//  * AHB ring: AHardwareBuffer images imported in Vulkan, handed to the consumer through ASurfaceTransaction-like
//    paths; not needed because the swapchain is the cheaper way to the same buffers. Kept as the documented fallback.
//  * Readback + decodeImageFromPixels (Dart): permanent fallback, unchanged, selected when create/attach fails.
//
// Frame input today is host RGBA8 (the existing renderer's output; one memcpy to a staging buffer, GPU copy+blit to the
// swapchain image). The zero-readback input (a VkImage rendered by the GPU executor on the same device) is the
// `adoptDevice` seam in vk_presenter.h: the HAL hands over its VkDevice/queue and calls presentImage().
#include <cstdint>
#include <memory>
#include <string>

namespace dapple::present {

struct PresentTiming {
  double uploadMs = 0;   // memcpy to staging
  double acquireMs = 0;  // waiting for a free swapchain image (consumer back-pressure)
  double submitMs = 0;   // record + submit + queue present
};

class Presenter {
 public:
  virtual ~Presenter() = default;
  // `window` is an ANativeWindow* (the presenter takes its own reference). w/h: the surface size in pixels.
  virtual bool setWindow(void* window, int w, int h, std::string& err) = 0;
  virtual void clearWindow() = 0;  // releases the swapchain and the window reference; instance/device stay
  virtual bool hasWindow() const = 0;
  // Draw an RGBA8 frame (tightly packed rows) scaled to the surface. False = this frame was not shown (error set).
  virtual bool present(const std::uint8_t* rgba, int w, int h, PresentTiming* t, std::string& err) = 0;
  // 0 = drop transient memory (staging, frame image); 1 = also the swapchain; 2 = everything including the device.
  virtual void trim(int level) = 0;
  virtual std::string info() const = 0;  // JSON object fragment (no braces needed by callers: full object)
  virtual std::uint64_t presented() const = 0;
};

// Vulkan swapchain presenter (Android). Null where Vulkan / Android are not available; `err` says why.
std::unique_ptr<Presenter> makeVulkanPresenter(std::string& err);
// Counts frames, remembers the last one (tests, desktop, and "no GPU": the view then reports the fallback).
std::unique_ptr<Presenter> makeNullPresenter();

}  // namespace dapple::present
