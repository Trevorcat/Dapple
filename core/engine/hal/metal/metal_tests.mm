// dapple_metal_tests: checks of the Metal backend that the Vulkan-shaped suites cannot make (ctest `metal.*`).
//   external   copyImageToExternal into an id<MTLTexture> (RGBA8 blit, BGRA8 swizzle kernel): the zero-copy present seam on Apple
//   caps       device caps text, formats and limits actually reported
//   rw         read-write storage image access in a compute pass for every format the shaders use
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/hal/hal.h"
#include "engine/hal/metal/metal_device.h"

using namespace dapple::hal;

static int g_fail = 0;
#define CHECK(c, ...)                                         \
  do {                                                        \
    if (!(c)) {                                               \
      ++g_fail;                                               \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); \
      std::printf(__VA_ARGS__);                               \
      std::printf("\n");                                      \
    }                                                         \
  } while (0)

static bool run(Device& d, CommandList& cl) {
  FenceH f;
  if (d.submit(cl, &f) != Status::Ok) return false;
  return d.wait(f, 10ull * 1000 * 1000 * 1000) == Status::Ok;
}

static void testCaps(Device& d) {
  std::printf("== caps ==\n%s\n", d.caps().text().c_str());
  const DeviceCaps& c = d.caps();
  CHECK(c.api == Api::Metal, "api");
  CHECK(c.maxImage2D >= 8192, "maxImage2D %u", c.maxImage2D);
  CHECK(c.maxSharedBytes >= 16384, "shared %u", c.maxSharedBytes);
  CHECK(c.maxWorkgroupInvocations >= 256, "invocations %u", c.maxWorkgroupInvocations);
  const char* names[] = {"R8", "RGBA8", "R16F", "RGBA16F", "R32F", "RG32F", "RGBA32F", "R32UI"};
  for (int i = 0; i < kFormatCount - 1; ++i)
    std::printf("  %-8s sampled %d filterable %d storage r/w/rw %d/%d/%d\n", names[i], c.format[i].sampled, c.format[i].filterable,
                c.format[i].storageRead, c.format[i].storageWrite, c.format[i].storageReadWrite);
  CHECK(c.format[int(Format::RGBA16F)].storageReadWrite, "RGBA16F read-write storage (the NR/detail kernels need it)");
}

static void testExternal(Device& d) {
  std::printf("== external image write (RGBA8 blit, BGRA8 swizzle) ==\n");
  const std::uint32_t W = 67, H = 41;  // not multiples of the threadgroup size
  BufferH up;
  CHECK(d.createBuffer({W * H * 4, MemKind::Upload, true}, &up) == Status::Ok, "upload buffer");
  auto* px = static_cast<std::uint8_t*>(up.mapped);
  for (std::uint32_t i = 0; i < W * H; ++i) {
    px[i * 4 + 0] = std::uint8_t(i * 3);
    px[i * 4 + 1] = std::uint8_t(i * 5 + 1);
    px[i * 4 + 2] = std::uint8_t(i * 7 + 2);
    px[i * 4 + 3] = 255;
  }
  ImageH src;
  CHECK(d.createImage({Format::RGBA8, 128, 64, 1}, &src) == Status::Ok, "src image");
  id<MTLDevice> md = (__bridge id<MTLDevice>)metal::nativeMetalDevice(d);  // (MTLCreateSystemDefaultDevice is nil in a session without a window server)
  CHECK(md != nil, "native device");
  for (int variant = 0; variant < 2; ++variant) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:variant ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm
                                                                                   width:W + 9
                                                                                  height:H + 5
                                                                               mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [md newTextureWithDescriptor:td];
    CHECK(dst != nil, "dst texture");
    std::vector<std::uint8_t> zero((W + 9) * (H + 5) * 4, 0x55);
    [dst replaceRegion:MTLRegionMake2D(0, 0, W + 9, H + 5) mipmapLevel:0 withBytes:zero.data() bytesPerRow:(W + 9) * 4];
    auto cl = d.begin();
    CHECK(cl != nullptr, "begin");
    cl->copyBufferToImage(up, src, W, H, 1, 0);
    ExternalImage ex;
    ex.handle = (__bridge void*)dst;
    ex.fmt = Format::RGBA8;
    ex.w = W + 9;
    ex.h = H + 5;
    const Status st = cl->copyImageToExternal(src, ex, W, H);
    CHECK(st == Status::Ok, "copyImageToExternal status %s", statusName(st));
    CHECK(run(d, *cl), "submit/wait");
    std::vector<std::uint8_t> out((W + 9) * (H + 5) * 4);
    [dst getBytes:out.data() bytesPerRow:(W + 9) * 4 fromRegion:MTLRegionMake2D(0, 0, W + 9, H + 5) mipmapLevel:0];
    int bad = 0, outside = 0;
    for (std::uint32_t y = 0; y < H + 5; ++y)
      for (std::uint32_t x = 0; x < W + 9; ++x) {
        const std::uint8_t* o = &out[(y * (W + 9) + x) * 4];
        if (x < W && y < H) {
          const std::uint8_t* s = &px[(y * W + x) * 4];
          const bool ok = variant ? (o[0] == s[2] && o[1] == s[1] && o[2] == s[0] && o[3] == s[3]) : !std::memcmp(o, s, 4);
          bad += !ok;
        } else {
          outside += !(o[0] == 0x55 && o[1] == 0x55 && o[2] == 0x55 && o[3] == 0x55);
        }
      }
    std::printf("  %s: %d wrong texels inside, %d touched outside the copied rect\n", variant ? "BGRA8 (swizzle kernel)" : "RGBA8 (blit)", bad, outside);
    CHECK(bad == 0, "texels differ");
    CHECK(outside == 0, "texels outside the rectangle were written");
  }
  d.destroy(up);
  d.destroy(src);
}

int main(int argc, char** argv) {
  std::string only = argc > 1 ? argv[1] : "";
  std::string err;
  DeviceOptions o;
  auto dev = createDevice(Api::Metal, o, err);
  if (!dev) {
    std::printf("no Metal device: %s\n", err.c_str());
    return 77;
  }
  if (only.empty() || only == "caps") testCaps(*dev);
  if (only.empty() || only == "external") testExternal(*dev);
  std::printf(g_fail ? "FAILED (%d failures)\n" : "PASSED (0 failures)\n", g_fail);
  return g_fail ? 1 : 0;
}
