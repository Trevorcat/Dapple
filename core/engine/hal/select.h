#pragma once
// Backend selection (docs/engine/pipeline-architecture.md 6.6 "Selection order", 5.9): Vulkan -> GLES 3.1 -> CPU.
// Override: env DAPPLE_GPU_API=vk|vulkan|gles|cpu (Android: also system property debug.dapple.gpu_api, read by the
// caller and passed as `forced`). Every backend is tried at most once; a failure is recorded, not fatal.
#include <functional>
#include <string>
#include <vector>

#include "engine/hal/hal.h"

namespace dapple::hal {

// Runs the backend's self-test on a freshly created device. Return false + reason to reject the backend. The Vulkan one is
// the whole-graph comparison against the CPU path (owned by the executor); the GLES default is gles::selfTest.
using SelfTestFn = std::function<bool(Device&, std::string& why)>;

struct Attempt {
  Api api = Api::Null;
  bool ok = false;
  std::string detail;  // reject reason or device text
  double ms = 0;
};
struct Selection {
  Api api = Api::Null;                 // Api::Null = CPU
  std::unique_ptr<Device> device;      // null for CPU
  std::vector<Attempt> attempts;       // in order tried
  std::string forced;                  // "" or the override that was applied
  // {"api":"gles31","device":"...","key":"...","forced":"","attempts":[{"api":"vulkan","ok":false,"detail":"...","ms":12.3},...]}
  std::string statusJson() const;
};

// forced: "", "vk", "vulkan", "gles", "cpu" (empty = read DAPPLE_GPU_API). A forced backend is the only one tried (no
// silent fallback, so tests can tell what ran); CPU is always the last resort when nothing is forced.
Selection selectDevice(const DeviceOptions& opt, const std::string& forced = {}, SelfTestFn vkTest = {}, SelfTestFn glesTest = {});
const char* apiName(Api);

}  // namespace dapple::hal
