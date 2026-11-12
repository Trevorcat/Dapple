#include "dapple/gpu_hook.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace dapple::gpu {

namespace {

std::atomic<bool> g_broken{false};  // device lost / repeated failures: stay on the CPU for the rest of the process

void logLine(const char* what, const std::string& why) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_WARN, "dapple-gpu", "%s: %s", what, why.c_str());
#else
  std::fprintf(stderr, "dapple-gpu: %s: %s\n", what, why.c_str());
#endif
}

// Backend + the caller's session for this photo; null when the GPU should not be used.
Session* acquire(SessionSlot& slot, const SourceImage& full) {
  if (g_broken.load()) return nullptr;
  std::string why;
  std::shared_ptr<Backend> be = sharedBackend(&why);
  if (!be) {
    static std::atomic<bool> told{false};
    if (runtimeEnabled() && !told.exchange(true)) logLine("backend unavailable, using the CPU", why);
    return nullptr;
  }
  if (!slot) slot = be->createSession();
  std::string err;
  if (!slot->setSource(full, 0, err)) {
    logLine("setSource", err);
    return nullptr;
  }
  return slot.get();
}

void failed(const std::string& err) {
  logLine("frame failed, CPU fallback", err);
  if (err.find("device lost") != std::string::npos || err.find("VkResult -4") != std::string::npos) g_broken.store(true);
}

}  // namespace

std::uint8_t* hookRenderFrame(SessionSlot& slot, const SourceImage& full, const Renderer& r, const BakedLut& lut, const FastOptions& fo,
                              int srcLong, int& w, int& h) {
  try {
    Session* s = acquire(slot, full);
    if (!s) return nullptr;
    if (!s->canRenderResident(full.linear.width, full.linear.height)) return nullptr;
    FrameOptions frame;
    frame.srcLongEdge = srcLong;
    std::string err;
    std::uint8_t* out = s->render(r, lut, fo, frame, w, h, nullptr, err);
    if (!out && err.find("unsupported on the GPU") == std::string::npos) failed(err);
    return out;
  } catch (...) {
    return nullptr;
  }
}

bool hookExport(SessionSlot& slot, const SourceImage& full, const Renderer& r, int outLongEdge, Image8& out) {
  try {
    Session* s = acquire(slot, full);
    if (!s) return false;
    FastOptions fo;  // everything on, region none
    fo.spatial = SpatialQuality::Full;
    FrameOptions frame;
    frame.srcLongEdge = outLongEdge;
    frame.resizeSharpen = 0.2f;  // gentle output sharpening after a Lanczos downscale
    int w = 0, h = 0;
    if (!s->frameSize(r, fo, frame, w, h)) return false;
    const auto lut = r.bake(full, 65);
    if (!lut) return false;
    out.width = w;
    out.height = h;
    out.data.assign(std::size_t(w) * h * 3, 0);
    ExportOptions eo;
    eo.frame = frame;
    eo.sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
      for (int j = 0; j < th; ++j) {
        const std::uint8_t* src = rgba + std::size_t(j) * stride;
        std::uint8_t* dst = out.data.data() + (std::size_t(y + j) * w + x) * 3;
        for (int i = 0; i < tw; ++i) {
          dst[3 * i] = src[4 * i];
          dst[3 * i + 1] = src[4 * i + 1];
          dst[3 * i + 2] = src[4 * i + 2];
        }
      }
      return true;
    };
    std::string err;
    if (!s->exportTiled(r, full, *lut, fo, eo, nullptr, err)) {
      failed(err);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::string statusJson() {
  std::string why;
  const bool enabled = runtimeEnabled();
  std::shared_ptr<Backend> be = enabled && !g_broken.load() ? sharedBackend(&why) : nullptr;
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o.push_back('\\');
      if (c == '\n') o += "\\n";
      else o.push_back(c);
    }
    return o;
  };
  std::string j = std::string("{\"enabled\":") + (enabled ? "true" : "false") + ",\"available\":" + (be ? "true" : "false");
  if (be) j += ",\"device\":\"" + esc(be->info().name) + "\",\"deviceKey\":\"" + be->info().key() + "\",\"selfTest\":\"" + esc(be->selfTestReport()) + "\"";
  else if (enabled) j += ",\"reason\":\"" + esc(g_broken.load() ? std::string("device lost") : why) + "\"";
  return j + "}";
}

}  // namespace dapple::gpu
