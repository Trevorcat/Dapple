// Backend independent parts of the HAL: caps text / key, blocklist, device factory.
#include "engine/hal/hal.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#if defined(DAPPLE_HAL_METAL)
#include "engine/hal/metal/metal_device.h"
#endif

namespace dapple::hal {

namespace vk {
std::unique_ptr<Device> createVulkanDevice(const DeviceOptions&, std::string&);
}

std::uint32_t bytesPerTexel(Format f) {
  switch (f) {
    case Format::R8: return 1;
    case Format::R16F: return 2;
    case Format::RGBA16F:
    case Format::RG32F: return 8;
    case Format::RGBA32F: return 16;
    default: return 4;
  }
}

std::string DeviceCaps::key() const {
  char b[96];
  std::snprintf(b, sizeof b, "%04x:%04x:%08x", vendorId, deviceId, driverVersion);
  return b;
}

std::string DeviceCaps::text() const {
  char b[1024];
  std::snprintf(b, sizeof b,
                "%s (vendor 0x%04x, type %u, driver %s)\n  Vulkan %u.%u.%u  maxImage2D %u  maxImage3D %u  shared %u KB  "
                "maxInvocations %u\n  subgroup size %u ops 0x%x  fp16 arith %d  storage16 %d  linear-filter RGBA32F %d  "
                "timestamps %d  device-local heap %.0f MB  unified memory %d",
                name.c_str(), vendorId, deviceType, driver.c_str(), apiVersion >> 22, (apiVersion >> 12) & 0x3ff, apiVersion & 0xfff, maxImage2D,
                maxImage3D, maxSharedBytes / 1024, maxWorkgroupInvocations, subgroupSize, subgroupOps, int(fp16Arithmetic), int(storage16),
                int(linearFilterRgba32f), int(timestamps), double(deviceLocalBytes) / 1048576.0, int(unifiedMemory));
  return b;
}

std::vector<BlockRule> parseBlocklist(const std::string& text) {
  std::vector<BlockRule> rules;
  std::string line;
  auto flush = [&] {
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      line.clear();
      return;
    }
    BlockRule r;
    std::stringstream ss(line);
    std::string kv;
    while (std::getline(ss, kv, ';')) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      if (k == "vendor") r.vendor = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "name") r.name = v;
      else if (k == "driver") r.driver = v;
      else if (k == "drvmin") r.driverMin = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "drvmax") r.driverMax = std::uint32_t(std::strtoul(v.c_str(), nullptr, 0));
      else if (k == "reason") r.reason = v;
    }
    rules.push_back(std::move(r));
    line.clear();
  };
  for (char c : text) {
    if (c == '\n' || c == '|') flush();
    else line.push_back(c);
  }
  flush();
  return rules;
}

std::string blockReason(const DeviceCaps& di, const std::vector<BlockRule>& rules, bool allowSoftware) {
  if (di.software && !allowSoftware) return "software Vulkan implementation (" + di.name + ")";
  for (const BlockRule& r : rules) {
    if (r.vendor && r.vendor != di.vendorId) continue;
    if (!r.name.empty() && di.name.find(r.name) == std::string::npos) continue;
    if (!r.driver.empty() && di.driver.find(r.driver) == std::string::npos) continue;
    if (di.driverVersion < r.driverMin || di.driverVersion > r.driverMax) continue;
    return "blocklisted: " + (r.reason.empty() ? std::string("no reason given") : r.reason);
  }
  return {};
}

std::unique_ptr<Device> createDevice(Api api, const DeviceOptions& opt, std::string& err) {
  switch (api) {
    case Api::Vulkan: return vk::createVulkanDevice(opt, err);
#if defined(DAPPLE_HAL_METAL)
    case Api::Metal: return metal::createMetalDevice(opt, err);
#endif
    default: err = "backend not built"; return nullptr;
  }
}

}  // namespace dapple::hal
