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

// Keep-warm (touch hint, docs/engine/device-results/redmi-garnet.md round 10): after an idle pause the GPU / memory clocks are low, the first submission
// pays a wake-up, and a drag alone does not lift the clocks. keepWarm(ms), called on touch-down, opens a window of GPU-only work units (a copy over a
// working set larger than the caches, ~1.5 ms each at the low clock; nothing read back, no effect on any picture) plus one busy thread on the big
// cores. At most two units are on the queue while no real frame is pending, none is submitted while one is (it waits for at most the units already on
// the GPU), and while the drag runs units are queued right behind each real frame (keepWarmAfterSubmit). Window: at least `ms`; until the first real
// frame or 250 ms; then 300 ms of the drag; never more than 400 ms after touch-down. Returns at once (one worker thread; a second call extends the
// window). ms < 0: allocate the working set now and run one unit. No-op when no GPU device is active. Env (measurement): DAPPLE_HINT_* in gpu_context.cpp.
void keepWarm(int ms);
// Real-frame bracket for the keep-warm pump: +1 when a foreground frame is requested, -1 when it returned (cheap, any thread).
void keepWarmFrame(int delta);
// GPU executor, right after a foreground frame was submitted (context mutex held): queues keep-warm units behind it while a window is open.
class Context;
void keepWarmAfterSubmit(Context& c);
// Diagnostics of the last keep-warm window: {"units":..,"gpuMs":..,"firstMBps":..,"lastMBps":..,...}
std::string keepWarmJson();

}  // namespace dapple::engine::gpu
