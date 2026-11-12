// Test shim: fills the Vulkan factory slot of hal.cpp with the GLES 3.1 device, so the UNMODIFIED GPU executor
// (exec/gpu_*.cpp, nodes/*/gpu.cpp, the scheduler) and its tests run on GLES (ANGLE on the desktop, the emulator's GLES on Android).
// Linked only into dapple_gpu_gles (gles_gpu.cmake); the production selection path is hal/select.cpp.
#include "engine/hal/gles/gles_device.h"
#include "engine/hal/hal.h"

namespace dapple::hal::vk {
std::unique_ptr<Device> createVulkanDevice(const DeviceOptions& o, std::string& err) { return gles::createGlesDevice(o, err); }
}  // namespace dapple::hal::vk
