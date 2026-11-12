// Metal presenter (Apple): shows frames in a texture the app owns, without the pixels visiting Dart.
//
// The "window" of attach_window is a DappleFrameSink (below): the Swift side (DappleViewPlugin) owns a ring of IOSurface backed BGRA CVPixelBuffers
// that a FlutterTexture hands to Flutter. Per frame:
//   beginExternal  -> the presenter's own frame image (RGBA8 MTLTexture on the ENGINE's device, taken from the device lease); the GPU executor renders
//                     the final pass into it (hal copyImageToExternal) with no readback
//   presentExternal-> a compute pass scales the frame region into the sink's next surface (bilinear; the surface follows the Flutter texture size),
//                     the completion handler tells the sink the surface is ready (FlutterTextureRegistry.textureFrameAvailable)
//   present(rgba)  -> host frames (CPU executor / fallback): uploaded into the same frame image, then the same scale pass
// Why a Flutter Texture and not a CAMetalLayer / platform view: the editor draws Flutter UI over the picture (pads, strips, histogram, overlays) and
// needs the picture to take part in Flutter's compositing and z-order; a platform view would be composited above or below the whole Flutter layer
// (hybrid composition, no per-widget layering) and costs a thread merge; a texture is the same model as the Android SurfaceProducer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "engine/exec/gpu_iface.h"
#include "presenter.h"

extern "C" {
// Implemented by the app (Swift). acquire returns an unretained id<MTLTexture> (BGRA8, shader write) valid until the matching ready(); null = none free.
typedef struct DappleFrameSink {
  void* user;
  void* (*acquire)(void* user, void* mtl_device);
  void (*ready)(void* user);
} DappleFrameSink;
}

namespace dapple::present {
using engine::GpuProvider;
using engine::gpuProvider;
namespace {

const char* kScaleSrc =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct P { uint2 src; uint2 dst; };\n"
    "kernel void k_scale(texture2d<float, access::sample> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],\n"
    "                    constant P& p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= p.dst.x || g.y >= p.dst.y) return;\n"
    "  constexpr sampler sm(filter::linear, address::clamp_to_edge, coord::pixel);\n"
    "  float2 uv = (float2(g) + 0.5) * float2(p.src) / float2(p.dst);\n"
    "  d.write(s.sample(sm, uv), g);\n}\n";

class MetalPresenter final : public Presenter {
 public:
  bool setWindow(void* window, int w, int h, std::string& err) override {
    (void)w;
    (void)h;
    if (!window) return err = "no frame sink", false;
    std::lock_guard<std::mutex> l(mu_);
    sink_ = static_cast<DappleFrameSink*>(window);
    return true;
  }
  void clearWindow() override {
    std::lock_guard<std::mutex> l(mu_);
    sink_ = nullptr;
  }
  bool hasWindow() const override { return sink_ != nullptr; }

  bool present(const std::uint8_t* rgba, int w, int h, PresentTiming* t, std::string& err) override {
    @autoreleasepool {
      std::lock_guard<std::mutex> l(mu_);
      if (!sink_) return err = "no window", false;
      if (!ensureDevice(err)) return false;
      if (!ensureFrame(w, h, err)) return false;
      [frame_ replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:NSUInteger(w) * 4];
      if (t) t->uploadMs = 0;
      return showLocked(w, h, err);
    }
  }

  bool beginExternal(int w, int h, ExternalTarget* out, std::string& err) override {
    @autoreleasepool {
      std::lock_guard<std::mutex> l(mu_);
      if (!sink_) return err = "no window", false;
      syncLease();
      if (!lease_.keepAlive) return err = "engine device not available for sharing", false;
      if (!ensureDevice(err)) return false;
      if (!ensureFrame(w, h, err)) return false;
      out->image = (__bridge void*)frame_;
      out->w = std::uint32_t(frame_.width);
      out->h = std::uint32_t(frame_.height);
      out->layoutIn = out->layoutOut = 0;
      return true;
    }
  }

  bool presentExternal(int w, int h, PresentTiming* t, std::string& err) override {
    @autoreleasepool {
      std::lock_guard<std::mutex> l(mu_);
      if (!sink_ || !frame_) return err = "no frame", false;
      if (t) t->fenceMs = 0;
      return showLocked(w, h, err);
    }
  }

  void trim(int level) override {
    std::lock_guard<std::mutex> l(mu_);
    if (level >= 0) frame_ = nil;
    if (level >= 2) {
      scale_ = nil;
      queue_ = nil;
      dev_ = nil;
      lease_ = {};
    }
  }
  std::string info() const override {
    char b[256];
    std::snprintf(b, sizeof b, "{\"backend\":\"metal\",\"adopted\":%s,\"presented\":%llu,\"frame\":\"%dx%d\"}", lease_.keepAlive ? "true" : "false",
                  (unsigned long long)n_.load(), fw_, fh_);
    return b;
  }
  std::uint64_t presented() const override { return n_.load(); }

 private:
  void syncLease() {
    GpuProvider* pv = gpuProvider();
    GpuProvider::DeviceLease l;
    if (pv && pv->lease(&l)) {
      if (!lease_.keepAlive || l.generation != lease_.generation || l.handles.device != lease_.handles.device) {
        lease_ = std::move(l);
        frame_ = nil;
        scale_ = nil;
        queue_ = nil;
        dev_ = nil;  // rebuilt on the new device
      }
    } else if (lease_.keepAlive) {
      lease_ = {};
      frame_ = nil;
      scale_ = nil;
      queue_ = nil;
      dev_ = nil;
    }
  }

  bool ensureDevice(std::string& err) {
    if (dev_) return true;
    if (lease_.keepAlive) {
      dev_ = (__bridge id<MTLDevice>)lease_.handles.device;
      queue_ = (__bridge id<MTLCommandQueue>)lease_.handles.queue;
    } else {
      dev_ = MTLCreateSystemDefaultDevice();
      queue_ = [dev_ newCommandQueue];
    }
    if (!dev_ || !queue_) return err = "no Metal device", false;
    NSError* e = nil;
    MTLCompileOptions* co = [MTLCompileOptions new];
    co.fastMathEnabled = NO;
    id<MTLLibrary> lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:kScaleSrc] options:co error:&e];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"k_scale"] : nil;
    scale_ = fn ? [dev_ newComputePipelineStateWithFunction:fn error:&e] : nil;
    if (!scale_) return err = std::string("scale kernel: ") + (e ? e.localizedDescription.UTF8String : "?"), false;
    return true;
  }

  bool ensureFrame(int w, int h, std::string& err) {
    if (frame_ && int(frame_.width) >= w && int(frame_.height) >= h) return true;
    const int cw = std::max(w, int(frame_ ? frame_.width : 0)), ch = std::max(h, int(frame_ ? frame_.height : 0));
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:NSUInteger(cw) height:NSUInteger(ch) mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModeShared;  // unified memory: the host path writes it directly, the GPU path through the executor's blit
    frame_ = [dev_ newTextureWithDescriptor:td];
    if (!frame_) return err = "frame image allocation failed", false;
    fw_ = cw;
    fh_ = ch;
    return true;
  }

  // frame_ (top-left w x h) -> the sink's next surface, scaled, then ready() when the GPU is done
  bool showLocked(int w, int h, std::string& err) {
    id<MTLTexture> dst = (__bridge id<MTLTexture>)sink_->acquire(sink_->user, (__bridge void*)dev_);
    if (!dst) return err = "no free surface", false;
    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    const std::uint32_t p[4] = {std::uint32_t(w), std::uint32_t(h), std::uint32_t(dst.width), std::uint32_t(dst.height)};
    [e setComputePipelineState:scale_];
    [e setTexture:frame_ atIndex:0];
    [e setTexture:dst atIndex:1];
    [e setBytes:p length:sizeof p atIndex:0];
    [e dispatchThreads:MTLSizeMake(dst.width, dst.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    DappleFrameSink* sink = sink_;
    id<MTLTexture> keepFrame = frame_;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      (void)keepFrame;
      sink->ready(sink->user);
    }];
    [cb commit];
    ++n_;
    return true;
  }

  std::mutex mu_;
  DappleFrameSink* sink_ = nullptr;
  GpuProvider::DeviceLease lease_;
  id<MTLDevice> dev_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLComputePipelineState> scale_ = nil;
  id<MTLTexture> frame_ = nil;
  int fw_ = 0, fh_ = 0;
  std::atomic<std::uint64_t> n_{0};
};

}  // namespace

// Apple: the display presenter is the Metal one (view.cpp asks for "the" platform presenter by this name).
std::unique_ptr<Presenter> makeVulkanPresenter(std::string& err) {
  (void)err;
  return std::make_unique<MetalPresenter>();
}

}  // namespace dapple::present
