#pragma once
// Metal backend entry points (docs/ios/metal-backend.md). Callers normally use hal::createDevice(Api::Metal, ...).
#include <memory>
#include <string>
#include <vector>

#include "engine/hal/hal.h"

namespace dapple::hal::metal {

std::unique_ptr<Device> createMetalDevice(const DeviceOptions&, std::string& err);
std::vector<std::string> shaderNames();
// The id<MTLDevice> behind a Metal Device (for a presenter that must share it), or null.
void* nativeMetalDevice(Device&);

}  // namespace dapple::hal::metal
