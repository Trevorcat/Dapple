#pragma once
// GLES 3.1 backend entry points (docs/engine/pipeline-architecture.md 6.3, 6.6). Callers normally go through
// hal::createDevice(Api::Gles31, ...) (hal.cpp dispatches to createGlesDevice) and hal::selectDevice (select.h).
#include <string>
#include <vector>

#include "engine/hal/hal.h"

namespace dapple::hal::gles {

std::unique_ptr<Device> createGlesDevice(const DeviceOptions&, std::string& err);
// names of the shaders embedded for this backend (generated table)
std::vector<std::string> shaderNames();
// hash of the shader set (feeds the self-test / program-cache keys)
const char* shaderSetHash();

struct SelfTestReport {
  bool ok = false;                       // device usable: the numeric case passed and the core programs linked
  std::vector<std::string> failed;       // programs that failed to compile/link on this driver (isolated: node becomes Unsupported)
  std::string text;                      // human readable
  double ms = 0;
};
// §6.6: pre-checks are done by createGlesDevice; this runs the numeric kernel case (buffer -> image -> readback) plus one
// default-variant build per embedded program, each isolated.
SelfTestReport selfTest(Device& dev);

}  // namespace dapple::hal::gles
