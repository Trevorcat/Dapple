#pragma once
// The GPU runtime switch and status, the part of the GPU executor the C API needs (no HAL / scheduler headers).
#include <string>

namespace dapple::engine::gpu {

// Enabled when (in this order) setRuntimeEnabled() was called, env DAPPLE_GPU=1, or on Android the system property
// debug.dapple.gpu=1. Default: off.
bool runtimeEnabled();
void setRuntimeEnabled(int enabled);  // 0 off, 1 on, -1 back to environment / property
// Installs the provider into EngineSession (idempotent). Call before the first frame that may use the GPU.
void installProvider();
// Presenter seam: enable VK_KHR_surface / VK_KHR_android_surface / VK_KHR_swapchain on the device the provider creates (must be
// called before the first GPU frame; -1 = environment DAPPLE_GPU_PRESENT). See docs/engine/present.md.
void setPresentExtensions(int on);
// {"enabled":..,"available":..,"api":"vulkan|gles31","device":..,"selfTest":..,"selection":{..},"fallbacks":..,...}
std::string statusJson();

}  // namespace dapple::engine::gpu
