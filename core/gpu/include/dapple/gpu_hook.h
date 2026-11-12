#pragma once
// The seam between the C API (core/src/c_api.cpp) and the GPU backend, kept out of c_api.cpp so that file only needs a
// few lines. Every function returns "not handled" (null / false) when the backend is off, unavailable, or cannot render
// this particular request; the caller then renders on the CPU exactly as before. Nothing here throws.
#include <cstdint>
#include <memory>
#include <string>

#include "dapple/gpu.h"

namespace dapple::gpu {

// Owned by the caller's per-photo object; created lazily.
using SessionSlot = std::unique_ptr<Session>;

// Same contract as Renderer::renderBakedRgba, but from the full-resolution source: `srcLong` is the long edge the CPU
// path would have scaled the source to (0 = natural). Returns a malloc()ed RGBA8 buffer or null.
std::uint8_t* hookRenderFrame(SessionSlot& slot, const SourceImage& full, const Renderer& r, const BakedLut& lut, const FastOptions& fo,
                              int srcLong, int& w, int& h);

// Full-picture export (tiled) into an 8-bit RGB image, optionally downscaled to `outLongEdge` (Lanczos + a little output
// sharpening). False = not handled.
bool hookExport(SessionSlot& slot, const SourceImage& full, const Renderer& r, int outLongEdge, Image8& out);

// {"enabled":..,"available":..,"device":..,"selfTest":..,"reason":..} (for diagnostics / the settings screen)
std::string statusJson();

}  // namespace dapple::gpu
