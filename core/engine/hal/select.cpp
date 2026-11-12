#include "engine/hal/select.h"

#include <chrono>
#include <cstdlib>
#include <sstream>

#include "engine/hal/hal.h"
#if defined(DAPPLE_HAL_GLES)
#include "engine/hal/gles/gles_device.h"
#endif

namespace dapple::hal {

const char* apiName(Api a) {
  switch (a) {
    case Api::Vulkan: return "vulkan";
    case Api::Gles31: return "gles31";
    case Api::Metal: return "metal";
    default: return "cpu";
  }
}

static std::string jsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    if (c == '\r') continue;
    o += c;
  }
  return o;
}

std::string Selection::statusJson() const {
  std::ostringstream o;
  o << "{\"api\":\"" << apiName(api) << "\",\"forced\":\"" << jsonEsc(forced) << "\"";
  if (device) o << ",\"device\":\"" << jsonEsc(device->caps().name) << "\",\"key\":\"" << jsonEsc(device->caps().key()) << "\",\"software\":" << (device->caps().software ? "true" : "false");
  o << ",\"fallbacks\":[";
  bool first = true;
  for (const Attempt& a : attempts) {
    if (a.ok) continue;
    o << (first ? "" : ",") << "{\"api\":\"" << apiName(a.api) << "\",\"reason\":\"" << jsonEsc(a.detail) << "\"}";
    first = false;
  }
  o << "],\"attempts\":[";
  for (std::size_t i = 0; i < attempts.size(); i++)
    o << (i ? "," : "") << "{\"api\":\"" << apiName(attempts[i].api) << "\",\"ok\":" << (attempts[i].ok ? "true" : "false") << ",\"ms\":" << attempts[i].ms
      << ",\"detail\":\"" << jsonEsc(attempts[i].detail) << "\"}";
  o << "]}";
  return o.str();
}

// Gles31 is created directly (hal.cpp's createDevice switch belongs to the Vulkan side and has no Gles31 case yet); everything else
// goes through hal::createDevice.
static std::unique_ptr<Device> makeDevice(Api api, const DeviceOptions& opt, std::string& err) {
#if defined(DAPPLE_HAL_GLES)
  if (api == Api::Gles31) return gles::createGlesDevice(opt, err);
#endif
  return createDevice(api, opt, err);
}

static bool tryApi(Api api, const DeviceOptions& opt, const SelfTestFn& test, Selection& s) {
  Attempt a;
  a.api = api;
  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  std::unique_ptr<Device> dev = makeDevice(api, opt, err);
  if (!dev) {
    a.detail = err.empty() ? "no device" : err;
  } else {
    std::string why;
    bool ok = true;
#if defined(DAPPLE_HAL_GLES)
    if (api == Api::Gles31 && !test) {
      const gles::SelfTestReport r = gles::selfTest(*dev);
      ok = r.ok;
      why = r.text;
    } else
#endif
        if (test) {
      ok = test(*dev, why);
    }
    if (!ok) {
      a.detail = "self-test failed: " + why;
      dev.reset();
    } else {
      a.ok = true;
      a.detail = dev->caps().text();
    }
  }
  a.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  s.attempts.push_back(a);
  if (a.ok) {
    s.api = api;
    s.device = std::move(dev);
  }
  return a.ok;
}

Selection selectDevice(const DeviceOptions& opt, const std::string& forcedIn, SelfTestFn vkTest, SelfTestFn glesTest) {
  Selection s;
  std::string f = forcedIn;
  if (f.empty()) {
    const char* e = std::getenv("DAPPLE_GPU_API");
    if (e) f = e;
  }
  for (auto& c : f) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (f == "vk") f = "vulkan";
  s.forced = f;
  if (f == "cpu") return s;
  if (f == "vulkan") {
    tryApi(Api::Vulkan, opt, vkTest, s);
    return s;
  }
  if (f == "metal") {
    tryApi(Api::Metal, opt, vkTest, s);
    return s;
  }
  if (f == "gles") {
    tryApi(Api::Gles31, opt, glesTest, s);
    return s;
  }
#if defined(DAPPLE_HAL_METAL)
  if (tryApi(Api::Metal, opt, vkTest, s)) return s;  // Apple: Metal is the primary API (there is no Vulkan)
#else
  if (tryApi(Api::Vulkan, opt, vkTest, s)) return s;
#endif
  if (tryApi(Api::Gles31, opt, glesTest, s)) return s;
  return s;  // CPU
}

}  // namespace dapple::hal
