#include "session.h"

#include <cstdarg>
#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif

namespace capi {

std::mutex& sessionRegistryMutex() {
  static std::mutex m;
  return m;
}
std::vector<DappleSession*>& sessionRegistry() {
  static std::vector<DappleSession*> v;
  return v;
}

thread_local bool tlExportCall = false;

namespace {
thread_local std::string g_lastError;
}

const std::string& lastError() { return g_lastError; }

void setError(std::string message) { g_lastError = std::move(message); }

// UTF-8 C string -> path (std::filesystem handles non-ASCII on Windows).
fs::path toPath(const char* utf8) {
  const std::string s = utf8 ? utf8 : "";
  return fs::path(std::u8string(s.begin(), s.end()));
}

const CameraRegistry& cameras() {
  static const CameraRegistry registry = CameraRegistry::withBuiltins();
  return registry;
}
const DecoderRegistry& decoders() {
  static const DecoderRegistry registry = DecoderRegistry::withBuiltins();
  return registry;
}

char* dupString(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

// ---- tiny JSON writing helpers ----
std::string esc(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
std::string n(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return buf;
}

namespace {
// ---- LUT cache (by path; reloaded when the file's mtime changes) ----
struct CachedLut {
  std::shared_ptr<const Lut3D> lut;
  fs::file_time_type mtime{};
};
std::mutex g_lutMutex;
std::map<std::string, CachedLut> g_luts;
}  // namespace

std::shared_ptr<const Lut3D> loadLut(const std::string& pathUtf8) {
  const fs::path path = toPath(pathUtf8.c_str());
  std::error_code ec;
  const auto mtime = fs::last_write_time(path, ec);
  std::lock_guard<std::mutex> lock(g_lutMutex);
  auto it = g_luts.find(pathUtf8);
  if (it != g_luts.end() && (ec || it->second.mtime == mtime)) return it->second.lut;
  auto lut = Lut3D::loadCube(path);
  if (!lut) {
    setError(lut.error().message);
    return nullptr;
  }
  CachedLut entry{std::make_shared<const Lut3D>(std::move(*lut)), ec ? fs::file_time_type{} : mtime};
  if (g_luts.size() > 16) g_luts.clear();
  return (g_luts[pathUtf8] = std::move(entry)).lut;
}

bool parseParams(const char* paramsJson, EditParams& params) {
  params = EditParams{};
  if (paramsJson && paramsJson[0]) {
    auto parsed = fromJson(paramsJson);
    if (!parsed) {
      setError("params: " + parsed.error().message);
      return false;
    }
    params = std::move(*parsed);
  }
  return true;
}

// In place: 8-bit sRGB -> 8-bit Display P3 (colorimetric, D65, same transfer curve).
void srgb8ToDisplayP3(Image8& img) {
  static const std::array<float, 256> dec = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) t[std::size_t(i)] = dapple::color::srgbDecode(float(i) / 255.f);
    return t;
  }();
  const dapple::color::Mat3& m = dapple::color::srgbToP3();
  const std::size_t n = std::size_t(img.width) * img.height;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint8_t* px = img.data.data() + 3 * i;
    const float in[3] = {dec[px[0]], dec[px[1]], dec[px[2]]};
    float o[3];
    m.apply(in, o);
    for (int c = 0; c < 3; ++c) px[c] = std::uint8_t(dapple::color::srgbEncode(o[c]) * 255.f + 0.5f);
  }
}

bool fillImage(const Image8& img, DappleImage* out) {
  const std::size_t pixels = std::size_t(img.width) * img.height;
  auto* rgba = static_cast<uint8_t*>(std::malloc(pixels * 4));
  if (!rgba) {
    setError("out of memory");
    return false;
  }
  for (std::size_t i = 0; i < pixels; ++i) {
    rgba[4 * i] = img.data[3 * i];
    rgba[4 * i + 1] = img.data[3 * i + 1];
    rgba[4 * i + 2] = img.data[3 * i + 2];
    rgba[4 * i + 3] = 255;
  }
  out->width = img.width;
  out->height = img.height;
  out->rgba = rgba;
  return true;
}

// One-line stage log (field diagnostics): tag dapple_engine on Android, os_log on Apple, stderr elsewhere.
void stageLog(const char* fmt, ...) {
  char buf[1536];  // the export and trim lines carry the memory figures at the end: keep them whole
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
#if defined(__ANDROID__)
  __android_log_write(ANDROID_LOG_INFO, "dapple_engine", buf);
#elif defined(__APPLE__)
  os_log(OS_LOG_DEFAULT, "dapple_engine: %{public}s", buf);
#else
  std::fprintf(stderr, "dapple_engine: %s\n", buf);
#endif
}
double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace capi

DappleSession::DappleSession() {
  std::lock_guard<std::mutex> lk(sessionRegistryMutex());
  sessionRegistry().push_back(this);
}

DappleSession::~DappleSession() {
  {
    std::lock_guard<std::mutex> lk(sessionRegistryMutex());
    auto& v = sessionRegistry();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
  }
  dapple::aimask::cancelOwner(this);  // AI mask jobs hold a pointer to this session
  dapple::aimask::waitOwner(this);
  // The film-match and touch-hint workers hold a raw pointer to this session: ask them to stop (no queued match is started, the hint burst ends at its
  // next frame) and wait until they are gone. Every loop is finite, so the wait is unbounded on purpose: a timeout that fell through would free the
  // session under a running worker.
  {
    std::lock_guard<std::mutex> lk(asyncMtx);
    asyncHave = false;
  }
  frameSeq.fetch_add(1);
  while (matchBusy.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

const char* dapple_version(void) { return DAPPLE_VERSION_STRING; }

const char* dapple_last_error(void) { return lastError().c_str(); }

void dapple_free_string(char* s) { std::free(s); }

void dapple_free_image(DappleImage* image) {
  if (!image) return;
  std::free(image->rgba);
  image->rgba = nullptr;
  image->width = 0;
  image->height = 0;
}
