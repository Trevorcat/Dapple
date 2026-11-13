#pragma once
// The GPU runtime switch and status, the part of the GPU executor the C API needs (no HAL / scheduler headers).
#include <string>

namespace dapple::engine::gpu {

// Modes (in this order of precedence): setRuntimeEnabled(0|1); env DAPPLE_GPU=0|1|auto; on Android the property debug.dapple.gpu = 0 (off, developer kill
// switch) | 1 (on, device created and self-tested on the first GPU frame) | anything else or unset = auto. Default: auto on Android, off elsewhere.
// Auto: the GPU is used once a background probe (startWarmup) created a device and it passed the self-test (Vulkan, else GLES 3.1); until then, and
// when nothing passed, frames render on the CPU. debug.dapple.gpu_api / DAPPLE_GPU_API force one API.
bool runtimeEnabled();                // mode != off (the GPU may be used; says nothing about the probe)
void setRuntimeEnabled(int enabled);  // 0 off, 1 on, -1 back to environment / property
// Where the self-test verdicts and the in-progress marker live (an app-private directory), the app version and Build.FINGERPRINT they are keyed by
// (empty fingerprint = the ro.build.fingerprint property). Call before startWarmup(); without a directory nothing is persisted.
void configure(const std::string& cacheDir, const std::string& appVersion, const std::string& fingerprint);
// The cache directory, app version and fingerprint passed to configure() (empty when never configured): per-device verdicts of other nodes (demosaic) are cached next to the probe verdicts.
void cacheConfig(std::string* dir, std::string* appVersion, std::string* fingerprint);
// Auto mode: starts the device creation + self-test on a detached low-priority thread (idempotent, returns at once).
void startWarmup();
// Installs the provider into EngineSession (idempotent). Call before the first frame that may use the GPU.
void installProvider();
// Presenter seam: enable VK_KHR_surface / VK_KHR_android_surface / VK_KHR_swapchain on the device the provider creates (must be
// called before the first GPU frame; -1 = environment DAPPLE_GPU_PRESENT). See docs/engine/present.md.
void setPresentExtensions(int on);
// {"mode":"off|on|auto","state":"idle|probing|ready|failed","enabled":..,"available":..,"api":"vulkan|gles31","device":..,"selfTest":..,"selection":{..},
//  "reason":..,"fallbacks":..,"executors":{engine_session.h executorStatsJson},...}
std::string statusJson();

}  // namespace dapple::engine::gpu
