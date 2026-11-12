// Engine selection between the GPU executor and the CPU (docs/engine/pipeline-architecture.md 5.9, step b3). Every function
// returns "not handled" (null / false) when the GPU is off, unavailable or cannot render this request; the caller then
// renders on the CPU exactly as before. Nothing here throws.
//
//   unsupported plan      -> this frame on the CPU, the GPU stays in use                       (counted: fallbacks)
//   transient error       -> this frame on the CPU, the session stays on the GPU              (counted: transients)
//   device lost           -> rebuild the device once and continue on the GPU; a second loss
//                            (or a failed rebuild / self-test) -> CPU for the rest of the process
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

std::atomic<bool> g_broken{false};  // second device loss / failed rebuild: stay on the CPU for the rest of the process
std::atomic<int> g_fallbacks{0}, g_transients{0}, g_lost{0};

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

// Classifies the failure of the last call on `s` and applies the policy above.
void failed(Session& s, const std::string& err) {
  switch (s.lastFailure()) {
    case Session::Failure::None:
    case Session::Failure::Unsupported:
      ++g_fallbacks;
      return;
    case Session::Failure::Transient:
      ++g_transients;
      logLine("frame failed, CPU for this frame", err);
      return;
    case Session::Failure::DeviceLost: {
      ++g_lost;
      logLine("device lost", err);
      std::string why;
      std::shared_ptr<Backend> be = sharedBackend(nullptr);
      if (!be || !be->rebuild(why)) {
        logLine("device rebuild failed, CPU for the rest of the session", why);
        g_broken.store(true);
      } else {
        logLine("device rebuilt", be->selfTestReport());
      }
      return;
    }
  }
}

}  // namespace

std::uint8_t* hookRenderFrame(SessionSlot& slot, const SourceImage& full, const Renderer& r, const BakedLut& lut, const FastOptions& fo, int srcLong, int& w,
                              int& h) {
  try {
    Session* s = acquire(slot, full);
    if (!s) return nullptr;
    if (!s->canRenderResident(full.linear.width, full.linear.height)) {
      ++g_fallbacks;
      return nullptr;
    }
    FrameOptions frame;
    frame.srcLongEdge = srcLong;
    std::string err;
    std::uint8_t* out = s->render(r, lut, fo, frame, w, h, nullptr, err);
    if (!out) failed(*s, err);
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
    // mobile / unified memory: tile 1024 keeps the export pool near 320 MB (pixel-architecture.md 16 rule 8)
    if (std::shared_ptr<Backend> be = sharedBackend(nullptr)) eo.tile = be->info().unifiedMemory ? 1024 : 2048;
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
      failed(*s, err);
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
  if (be) j += ",\"api\":\"vulkan\",\"device\":\"" + esc(be->info().name) + "\",\"deviceKey\":\"" + be->info().key() + "\",\"selfTest\":\"" + esc(be->selfTestReport()) + "\"";
  else if (enabled) j += ",\"reason\":\"" + esc(g_broken.load() ? std::string("device lost") : why) + "\"";
  j += ",\"fallbacks\":" + std::to_string(g_fallbacks.load()) + ",\"transients\":" + std::to_string(g_transients.load()) +
       ",\"deviceLost\":" + std::to_string(g_lost.load()) + ",\"rebuilds\":" + std::to_string(be ? be->rebuilds() : 0);
  return j + "}";
}

}  // namespace dapple::gpu
