// dapple_gles_check: GLES backend smoke / self-test tool (ctest `gles_selftest_*`, emulator, devices).
//   dapple_gles_check [--software] [--skip] [--auto]       exit 0 = selected GLES and self-test ok
// env: DAPPLE_ANGLE_DIR / DAPPLE_ANGLE_BACKEND (desktop), DAPPLE_GPU_API (forced selection)
#include <cstdio>
#include <cstring>

#include "engine/hal/gles/gles_device.h"
#include "engine/hal/select.h"

#ifdef DAPPLE_GLES_CHECK_STUB_VK
namespace dapple::hal::vk {
std::unique_ptr<Device> createVulkanDevice(const DeviceOptions&, std::string& err) {
  err = "Vulkan not linked into this tool";
  return nullptr;
}
}  // namespace dapple::hal::vk
#endif

int main(int argc, char** argv) {
  using namespace dapple::hal;
  DeviceOptions opt;
  std::string forced = "gles";
  bool skip = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--software")) opt.allowSoftware = true;
    else if (!std::strcmp(argv[i], "--auto")) forced = "";
    else if (!std::strcmp(argv[i], "--skip")) skip = true;  // ctest: no GLES device / ANGLE = skipped (exit 77), not failed
  }
  Selection s = selectDevice(opt, forced);
  std::printf("%s\n", s.statusJson().c_str());
  if (!s.device) return skip ? 77 : 2;
  std::printf("%s\n", s.device->caps().text().c_str());
  const gles::SelfTestReport r = gles::selfTest(*s.device);
  std::printf("self-test %s in %.1f ms (shader set %s)\n%s\n", r.ok ? "OK" : "FAILED", r.ms, gles::shaderSetHash(), r.text.c_str());
  for (const auto& f : r.failed) std::printf("  FAILED program: %s\n", f.c_str());
  return r.ok && r.failed.empty() ? 0 : 1;
}
