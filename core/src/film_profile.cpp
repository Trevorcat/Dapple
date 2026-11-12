// Film profile loading (JSON and the compact binary film pack) and the FilmLibrary container.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "dapple/film.h"
#include "film_internal.h"
#include "film_json.h"

namespace dapple::film {

using detail::JValue;

namespace {

bool readFile(const std::filesystem::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::string strOf(const JValue* v) { return v && v->type == JValue::Str ? v->str : std::string(); }

bool needArray(const JValue* data, const char* key, std::size_t count, std::vector<float>& dst, std::string* err,
               bool optional = false) {
  const JValue* v = data->get(key);
  if (!v) {
    if (optional) return true;
    if (err) *err = std::string("profile: missing '") + key + "'";
    return false;
  }
  dst.clear();
  detail::flattenNumbers(*v, dst);
  if (count && !dst.empty() && dst.size() != count) {
    if (err) *err = std::string("profile: '") + key + "' has wrong size " + std::to_string(dst.size());
    return false;
  }
  if (!optional && dst.empty()) {
    if (err) *err = std::string("profile: empty '") + key + "'";
    return false;
  }
  return true;
}

// ---- pack format DFP2 -----------------------------------------------------------------------------------
// All integers and floats are little-endian regardless of the host (explicit byte encoding, no memcpy of host
// representation).
//   header  : "DFP2" | u32 fileSize | u32 crc32(bytes [16, fileSize)) | u32 sectionCount
//   table   : sectionCount x { u32 tag, u32 offset, u32 length }   (offsets from file start, inside the file)
//   sections: ATTR (string), UPSM (spectral upsampling), CMAX (gamut table), STCK (one per stock, sorted by id).
// A reader skips sections with an unknown tag (room for new data); a known section must be consumed exactly.
// The magic carries the format version: there is no separate version field.
constexpr char kMagic[4] = {'D', 'F', 'P', '2'};
constexpr std::uint32_t tag4(const char (&s)[5]) {
  return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 | std::uint32_t(std::uint8_t(s[2])) << 16 |
         std::uint32_t(std::uint8_t(s[3])) << 24;
}
constexpr std::uint32_t kTagAttr = tag4("ATTR"), kTagUpsm = tag4("UPSM"), kTagCmax = tag4("CMAX"), kTagStck = tag4("STCK");
constexpr std::uint32_t kMaxStocks = 4096, kMaxFloats = 1u << 24, kMaxString = 1u << 20;

std::uint32_t crc32(const unsigned char* p, std::size_t n) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

struct Writer {
  std::string buf;
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(char((v >> (8 * i)) & 0xFF));
  }
  void f32(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    u32(v);
  }
  void str(const std::string& s) {
    u32(std::uint32_t(s.size()));
    buf.append(s);
  }
  void floats(const std::vector<float>& v) {
    u32(std::uint32_t(v.size()));
    for (float f : v) f32(f);
  }
};

struct Reader {
  const unsigned char* p;
  const unsigned char* end;
  bool ok = true;
  std::uint32_t u32() {
    if (std::size_t(end - p) < 4) {
      ok = false;
      return 0;
    }
    const std::uint32_t v = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    p += 4;
    return v;
  }
  std::string str() {
    const std::uint32_t n = u32();
    if (!ok || n > kMaxString || std::size_t(end - p) < n) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    return s;
  }
  std::vector<float> floats() {
    const std::uint32_t n = u32();
    std::vector<float> v;
    if (!ok || n > kMaxFloats || (std::size_t(end - p) / 4) < n) {
      ok = false;
      return v;
    }
    v.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t b = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
      std::memcpy(&v[i], &b, 4);
      p += 4;
    }
    return v;
  }
  bool done() const { return ok && p == end; }
};

bool allFinite(const std::vector<float>& v) {
  for (float f : v)
    if (!std::isfinite(f)) return false;
  return true;
}

}  // namespace

// Structural and numeric validation shared by the JSON and the pack loaders: exact sizes of every array, finite
// values (spectral / density data may hold NaN for "not measured" samples, never infinities).
bool validateStock(const FilmStock& s, std::string* err) {
  auto bad = [&](const char* m) {
    if (err) *err = "stock '" + s.stock + "': " + m;
    return false;
  };
  const std::size_t nc = s.logExposure.size();
  if (s.stock.empty()) return bad("empty id");
  if (s.type != "negative" && s.type != "positive") return bad("bad type");
  if (s.support != "film" && s.support != "paper") return bad("bad support");
  if (s.stage != "filming" && s.stage != "printing") return bad("bad stage");
  if (s.logSensitivity.size() != detail::kN * 3 || s.channelDensity.size() != detail::kN * 3 ||
      s.baseDensity.size() != detail::kN)
    return bad("spectral arrays have the wrong size");
  if (nc < 2 || nc > 4096) return bad("log_exposure size out of range");
  if (s.densityCurves.size() != nc * 3) return bad("density_curves size");
  if (!s.windowParams.empty() && s.windowParams.size() != 4) return bad("window params size");
  if (!s.surfaceParams.empty() && s.surfaceParams.size() != 45) return bad("surface params size");
  if (!s.densityCurvesLayers.empty() && s.densityCurvesLayers.size() != nc * 9) return bad("density_curves_layers size");
  const bool anyModel = !s.modelCenters.empty() || !s.modelAmplitudes.empty() || !s.modelSigmas.empty();
  if (anyModel && (s.modelCenters.size() != 9 || s.modelAmplitudes.size() != 9 || s.modelSigmas.size() != 9))
    return bad("density_curves_model needs 9 centers, amplitudes and sigmas");
  if (s.isPrintMedium() && !anyModel) return bad("print profile without density_curves_model");
  for (const auto* v : {&s.logExposure, &s.windowParams, &s.surfaceParams, &s.modelCenters, &s.modelAmplitudes, &s.modelSigmas})
    if (!allFinite(*v)) return bad("non-finite value");
  // NaN marks "not measured" in the spectral and density data (see FilmStock); infinities are never valid
  for (const auto* v : {&s.logSensitivity, &s.channelDensity, &s.baseDensity, &s.densityCurves})
    for (float f : *v)
      if (std::isinf(f)) return bad("infinite value");
  for (float f : s.densityCurvesLayers)
    if (std::isinf(f)) return bad("infinite density layer");
  for (float f : s.modelSigmas)
    if (!(f > 0)) return bad("non-positive model sigma");
  return true;
}

namespace {

}  // namespace

bool FilmLibrary::loadProfileJson(const std::filesystem::path& file, FilmStock& s, std::string* err) {
  std::string text;
  if (!readFile(file, text)) {
    if (err) *err = "cannot read " + file.string();
    return false;
  }
  JValue root;
  std::string perr;
  if (!detail::parseJson(text, root, &perr)) {
    if (err) *err = file.string() + ": " + perr;
    return false;
  }
  const JValue* info = root.get("info");
  const JValue* data = root.get("data");
  const JValue* meta = root.get("metadata");
  if (!info || !data) {
    if (err) *err = file.string() + ": missing info/data";
    return false;
  }
  s = FilmStock{};
  s.stock = strOf(info->get("stock"));
  s.name = strOf(info->get("name"));
  s.type = strOf(info->get("type"));
  s.support = strOf(info->get("support"));
  s.stage = strOf(info->get("stage"));
  s.use = strOf(info->get("use"));
  s.antihalation = strOf(info->get("antihalation"));
  s.referenceIlluminant = strOf(info->get("reference_illuminant"));
  s.viewingIlluminant = strOf(info->get("viewing_illuminant"));
  if (meta) {
    s.copyright = strOf(meta->get("copyright"));
    s.license = strOf(meta->get("license"));
  }
  if (s.stock.empty() || (s.type != "negative" && s.type != "positive") ||
      (s.support != "film" && s.support != "paper") || (s.stage != "filming" && s.stage != "printing")) {
    if (err) *err = file.string() + ": invalid info fields";
    return false;
  }
  std::vector<float> wl;
  std::string e;
  if (!needArray(data, "wavelengths", detail::kN, wl, &e) || wl[0] != 380.f || wl[detail::kN - 1] != 780.f) {
    if (err) *err = file.string() + ": " + (e.empty() ? "wavelength grid is not 380:780:5" : e);
    return false;
  }
  bool ok = needArray(data, "log_sensitivity", detail::kN * 3, s.logSensitivity, &e) &&
            needArray(data, "channel_density", detail::kN * 3, s.channelDensity, &e) &&
            needArray(data, "base_density", detail::kN, s.baseDensity, &e) &&
            needArray(data, "log_exposure", 0, s.logExposure, &e) &&
            needArray(data, "hanatos2025_adaptation_window_params", 4, s.windowParams, &e, true) &&
            needArray(data, "hanatos2025_adaptation_surface_params", 45, s.surfaceParams, &e, true) &&
            needArray(data, "density_curves_layers", 0, s.densityCurvesLayers, &e, true);
  if (ok) ok = needArray(data, "density_curves", s.logExposure.size() * 3, s.densityCurves, &e);
  if (ok) {
    if (const JValue* m = data->get("density_curves_model")) {
      ok = needArray(m, "centers", 9, s.modelCenters, &e) && needArray(m, "amplitudes", 9, s.modelAmplitudes, &e) &&
           needArray(m, "sigmas", 9, s.modelSigmas, &e);
    } else if (s.isPrintMedium()) {
      e = "print profile without density_curves_model";
      ok = false;
    }
  }
  if (ok && s.logExposure.size() < 2) {
    e = "log_exposure too short";
    ok = false;
  }
  if (ok) ok = validateStock(s, &e);
  if (!ok) {
    if (err) *err = file.string() + ": " + e;
    return false;
  }
  return true;
}

void FilmLibrary::add(FilmStock s) {
  const std::string key = s.stock;
  if (!stocks_.count(key)) order_.push_back(key);
  stocks_[key] = std::move(s);
}
const FilmStock* FilmLibrary::find(const std::string& name) const {
  auto it = stocks_.find(name);
  return it == stocks_.end() ? nullptr : &it->second;
}
std::vector<std::string> FilmLibrary::names() const {
  std::vector<std::string> n = order_;
  std::sort(n.begin(), n.end());
  return n;
}

std::shared_ptr<FilmLibrary> FilmLibrary::loadJsonDir(const std::filesystem::path& dir, std::string* err) {
  auto lib = std::make_shared<FilmLibrary>();
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().extension() == ".json") files.push_back(e.path());
  if (ec || files.empty()) {
    if (err) *err = "no profile JSON files in " + dir.string();
    return nullptr;
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    FilmStock s;
    if (!loadProfileJson(f, s, err)) return nullptr;
    lib->add(std::move(s));
  }
  lib->setTables(detail::generateUpsampleTable(192), detail::buildCmaxTable());
  lib->setAttribution(
      "Film and paper profiles: spektrafilm by Andrea Volpato, https://github.com/andreavolpato/spektrafilm, "
      "licensed CC BY-SA 4.0 (resampled / re-encoded into a binary film pack; no other modification).");
  return lib;
}

bool FilmLibrary::writePack(const std::filesystem::path& path, std::string* err) const {
  struct Sec {
    std::uint32_t tag;
    std::string bytes;
  };
  std::vector<Sec> secs;
  {
    Writer w;
    w.str(attribution_);
    secs.push_back({kTagAttr, std::move(w.buf)});
  }
  {
    Writer w;
    w.u32(std::uint32_t(upsample_.n));
    w.floats(upsample_.coef);
    secs.push_back({kTagUpsm, std::move(w.buf)});
  }
  {
    Writer w;
    w.u32(std::uint32_t(cmax_.nJ));
    w.u32(std::uint32_t(cmax_.nH));
    w.floats(cmax_.data);
    secs.push_back({kTagCmax, std::move(w.buf)});
  }
  for (const std::string& k : names()) {
    const FilmStock& s = stocks_.at(k);
    std::string e;
    if (!validateStock(s, &e)) {
      if (err) *err = e;
      return false;
    }
    Writer w;
    for (const std::string* t : {&s.stock, &s.name, &s.type, &s.support, &s.stage, &s.use, &s.antihalation,
                                 &s.referenceIlluminant, &s.viewingIlluminant, &s.copyright, &s.license})
      w.str(*t);
    for (const auto* v : {&s.logSensitivity, &s.windowParams, &s.surfaceParams, &s.channelDensity, &s.baseDensity,
                          &s.logExposure, &s.densityCurves, &s.densityCurvesLayers, &s.modelCenters, &s.modelAmplitudes,
                          &s.modelSigmas})
      w.floats(*v);
    secs.push_back({kTagStck, std::move(w.buf)});
  }
  Writer h;
  h.buf.append(kMagic, 4);
  h.u32(0);  // file size, patched below
  h.u32(0);  // crc, patched below
  h.u32(std::uint32_t(secs.size()));
  std::size_t off = 16 + secs.size() * 12;
  for (const Sec& sc : secs) {
    h.u32(sc.tag);
    h.u32(std::uint32_t(off));
    h.u32(std::uint32_t(sc.bytes.size()));
    off += sc.bytes.size();
  }
  std::string file = std::move(h.buf);
  for (const Sec& sc : secs) file += sc.bytes;
  auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) file[at + i] = char((v >> (8 * i)) & 0xFF);
  };
  put32(4, std::uint32_t(file.size()));
  put32(8, crc32(reinterpret_cast<const unsigned char*>(file.data()) + 16, file.size() - 16));
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot write " + path.string();
    return false;
  }
  f.write(file.data(), std::streamsize(file.size()));
  return bool(f);
}

std::shared_ptr<FilmLibrary> FilmLibrary::loadPackMemory(const void* data, std::size_t size, std::string* err) {
  auto fail = [&](const std::string& m) -> std::shared_ptr<FilmLibrary> {
    if (err) *err = "film pack: " + m;
    return nullptr;
  };
  try {
    const auto* base = static_cast<const unsigned char*>(data);
    if (!data || size < 16 || std::memcmp(base, kMagic, 4) != 0) return fail("bad magic");
    Reader hr{base + 4, base + 16};
    const std::uint32_t fileSize = hr.u32(), crc = hr.u32(), nSec = hr.u32();
    if (fileSize != size) return fail("size mismatch (truncated or trailing bytes)");
    if (crc32(base + 16, size - 16) != crc) return fail("checksum mismatch");
    if (nSec > kMaxStocks + 8 || size < 16 + std::size_t(nSec) * 12) return fail("bad section table");
    auto lib = std::make_shared<FilmLibrary>();
    bool haveAttr = false, haveUp = false, haveCm = false;
    UpsampleTable up;
    CmaxTable cm;
    std::uint32_t nStocks = 0;
    Reader tr{base + 16, base + 16 + std::size_t(nSec) * 12};
    for (std::uint32_t i = 0; i < nSec; ++i) {
      const std::uint32_t tag = tr.u32(), off = tr.u32(), len = tr.u32();
      if (off < 16 + std::size_t(nSec) * 12 || std::size_t(off) + len > size) return fail("section out of range");
      Reader r{base + off, base + off + len};
      if (tag == kTagAttr) {
        lib->attribution_ = r.str();
        haveAttr = r.done();
        if (!haveAttr) return fail("corrupt attribution section");
      } else if (tag == kTagUpsm) {
        const std::uint32_t n = r.u32();
        up.coef = r.floats();
        if (!r.done() || n < 2 || n > 1024 || up.coef.size() != std::size_t(n) * n * 4 || !allFinite(up.coef))
          return fail("corrupt upsampling table");
        up.n = int(n);
        haveUp = true;
      } else if (tag == kTagCmax) {
        const std::uint32_t nj = r.u32(), nh = r.u32();
        cm.data = r.floats();
        if (!r.done() || nj < 1 || nh < 1 || nj > 4096 || nh > 4096 || cm.data.size() != std::size_t(nj) * nh ||
            !allFinite(cm.data))
          return fail("corrupt gamut table");
        cm.nJ = int(nj);
        cm.nH = int(nh);
        haveCm = true;
      } else if (tag == kTagStck) {
        if (++nStocks > kMaxStocks) return fail("too many stocks");
        FilmStock s;
        for (std::string* t : {&s.stock, &s.name, &s.type, &s.support, &s.stage, &s.use, &s.antihalation,
                               &s.referenceIlluminant, &s.viewingIlluminant, &s.copyright, &s.license})
          *t = r.str();
        for (auto* v : {&s.logSensitivity, &s.windowParams, &s.surfaceParams, &s.channelDensity, &s.baseDensity,
                        &s.logExposure, &s.densityCurves, &s.densityCurvesLayers, &s.modelCenters, &s.modelAmplitudes,
                        &s.modelSigmas})
          *v = r.floats();
        if (!r.done()) return fail("corrupt stock record");
        std::string e;
        if (!validateStock(s, &e)) return fail(e);
        lib->add(std::move(s));
      }  // unknown tags are skipped
    }
    if (!haveAttr || !haveUp || !haveCm || nStocks == 0) return fail("missing sections");
    lib->setTables(std::move(up), std::move(cm));
    return lib;
  } catch (const std::exception& ex) {
    return fail(ex.what());
  }
}

std::shared_ptr<FilmLibrary> FilmLibrary::loadPack(const std::filesystem::path& path, std::string* err) {
  std::string buf;
  if (!readFile(path, buf)) {
    if (err) *err = "cannot read " + path.string();
    return nullptr;
  }
  return loadPackMemory(buf.data(), buf.size(), err);
}

}  // namespace dapple::film
