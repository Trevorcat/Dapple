// Camera JPEGs (and other 8-bit images). They are display-referred sRGB, so
// they are linearized and moved into the common linear ProPhoto space.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../color.h"
#include "../colour_space.h"
#include "dapple/decoder.h"
#include "file_util.h"
#include "parallel.h"

namespace dapple {

namespace {

struct ExifInfo {
  int orientation = 1;
  std::string make, model;
};

// Minimal EXIF reader: IFD0 Make/Model/Orientation from a TIFF block (the payload of a JPEG APP1 "Exif" segment or a PNG eXIf chunk).
ExifInfo readTiffExif(const std::uint8_t* t, std::size_t tlen) {
  ExifInfo info;
  if (tlen < 8) return info;
  const bool le = t[0] == 'I';
  auto u16 = [&](std::size_t o) -> unsigned {
    if (o + 2 > tlen) return 0;
    return le ? unsigned(t[o] | (t[o + 1] << 8)) : unsigned((t[o] << 8) | t[o + 1]);
  };
  auto u32 = [&](std::size_t o) -> std::size_t {
    if (o + 4 > tlen) return 0;
    return le ? (std::size_t(t[o]) | (std::size_t(t[o + 1]) << 8) | (std::size_t(t[o + 2]) << 16) | (std::size_t(t[o + 3]) << 24))
              : ((std::size_t(t[o]) << 24) | (std::size_t(t[o + 1]) << 16) | (std::size_t(t[o + 2]) << 8) | std::size_t(t[o + 3]));
  };
  const std::size_t ifd = u32(4);
  const unsigned count = u16(ifd);
  for (unsigned i = 0; i < count && ifd + 2 + 12 * (i + 1) <= tlen; ++i) {
    const std::size_t e = ifd + 2 + 12 * i;
    const unsigned tag = u16(e), type = u16(e + 2);
    const std::size_t n = u32(e + 4);
    if (tag == 0x0112 && type == 3) {
      const unsigned o = u16(e + 8);
      if (o >= 1 && o <= 8) info.orientation = int(o);
    } else if ((tag == 0x010F || tag == 0x0110) && type == 2 && n > 0) {
      const std::size_t off = n <= 4 ? e + 8 : u32(e + 8);
      if (off + n <= tlen) {
        std::string str(reinterpret_cast<const char*>(t + off), n);
        while (!str.empty() && (str.back() == '\0' || str.back() == ' ')) str.pop_back();
        (tag == 0x010F ? info.make : info.model) = str;
      }
    }
  }
  return info;
}

std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

// What the container says: EXIF (orientation, camera) and colour tags. PNG: chunks (cICP, iCCP, sRGB, gAMA, cHRM, eXIf);
// JPEG: APP1 Exif and the APP2 ICC_PROFILE segments.
struct Container {
  ExifInfo exif;
  color::ColourTags tags;
  bool png = false;
};

void inflateIcc(const std::uint8_t* d, std::size_t n, color::ColourTags& tags) {
  std::size_t z = 0;
  while (z < n && d[z] != 0) ++z;  // profile name, NUL, compression method
  z += 2;
  if (z >= n) return;
  int outLen = 0;
  char* out = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(d + z), int(n - z), &outLen);
  if (!out) return;
  tags.icc.assign(reinterpret_cast<std::uint8_t*>(out), reinterpret_cast<std::uint8_t*>(out) + outLen);
  stbi_image_free(out);
}

Container readContainer(const std::vector<std::uint8_t>& b) {
  Container c;
  static const std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (b.size() >= 8 && std::memcmp(b.data(), kPng, 8) == 0) {
    c.png = true;
    std::size_t pos = 8;
    while (pos + 12 <= b.size()) {
      const std::size_t len = be32(&b[pos]);
      const char* type = reinterpret_cast<const char*>(&b[pos + 4]);
      if (pos + 12 + len > b.size()) break;
      const std::uint8_t* d = &b[pos + 8];
      if (!std::memcmp(type, "IEND", 4)) break;
      if (!std::memcmp(type, "cICP", 4) && len >= 4) {
        c.tags.hasCicp = true;
        c.tags.cicpPrimaries = d[0], c.tags.cicpTransfer = d[1], c.tags.cicpMatrix = d[2], c.tags.cicpFullRange = d[3];
      } else if (!std::memcmp(type, "iCCP", 4)) {
        inflateIcc(d, len, c.tags);
      } else if (!std::memcmp(type, "sRGB", 4)) {
        c.tags.hasSrgbChunk = true;
      } else if (!std::memcmp(type, "gAMA", 4) && len >= 4) {
        c.tags.gamma = double(be32(d)) / 100000.0;
      } else if (!std::memcmp(type, "cHRM", 4) && len >= 32) {
        c.tags.hasChrm = true;
        for (int i = 0; i < 8; ++i) c.tags.chrm[i] = double(be32(d + 4 * i)) / 100000.0;
      } else if (!std::memcmp(type, "eXIf", 4)) {
        c.exif = readTiffExif(d, len);
      }
      pos += 12 + len;
    }
    return c;
  }
  // JPEG
  std::size_t pos = 2;
  std::vector<std::pair<int, std::pair<std::size_t, std::size_t>>> icc;  // sequence number -> payload range
  while (pos + 4 < b.size() && b[pos] == 0xFF) {
    const int marker = b[pos + 1];
    if (marker == 0xDA || marker == 0xD9) break;
    const std::size_t len = (std::size_t(b[pos + 2]) << 8) | b[pos + 3];
    if (pos + 2 + len > b.size()) break;
    if (marker == 0xE1 && len > 16 && std::memcmp(&b[pos + 4], "Exif\0\0", 6) == 0) {
      c.exif = readTiffExif(&b[pos + 10], len - 8);
    } else if (marker == 0xE2 && len > 16 && std::memcmp(&b[pos + 4], "ICC_PROFILE\0", 12) == 0) {
      icc.push_back({b[pos + 16], {pos + 18, pos + 2 + len}});
    }
    pos += 2 + len;
  }
  std::sort(icc.begin(), icc.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  for (const auto& s : icc) c.tags.icc.insert(c.tags.icc.end(), b.begin() + std::ptrdiff_t(s.second.first), b.begin() + std::ptrdiff_t(s.second.second));
  return c;
}

// The note of a space (non-sRGB space, fallback) goes to stderr and, on Android, logcat; once per file.
void logOnce(const std::filesystem::path& path, const std::string& line) {
  if (line.empty()) return;
  static std::mutex mu;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lk(mu);
  if (!seen.insert(path.string() + line).second) return;
  std::fprintf(stderr, "%s\n", line.c_str());
#ifdef __ANDROID__
  __android_log_write(ANDROID_LOG_INFO, "dapple_engine", line.c_str());
#endif
}

// Applies an EXIF orientation (1..8) so the result is upright.
ImageF applyOrientation(const ImageF& src, int orientation) {
  if (orientation <= 1 || orientation > 8) return src;
  const bool swap = orientation >= 5;
  ImageF dst(swap ? src.height : src.width, swap ? src.width : src.height);
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x) {
      int dx = x, dy = y;
      switch (orientation) {
        case 2: dx = src.width - 1 - x; break;
        case 3: dx = src.width - 1 - x; dy = src.height - 1 - y; break;
        case 4: dy = src.height - 1 - y; break;
        case 5: dx = y; dy = x; break;
        case 6: dx = src.height - 1 - y; dy = x; break;
        case 7: dx = src.height - 1 - y; dy = src.width - 1 - x; break;
        case 8: dx = y; dy = src.width - 1 - x; break;
        default: break;
      }
      std::memcpy(dst.row(dy) + 3 * dx, src.row(y) + 3 * x, 3 * sizeof(float));
    }
  return dst;
}

class JpegDecoder final : public ImageDecoder {
 public:
  const char* name() const override { return "jpeg"; }

  bool canDecode(const std::filesystem::path& path) const override {
    const std::string ext = lowerExtension(path);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
  }

  Result<ShotMetadata> probe(const std::filesystem::path& path) const override {
    auto bytes = readFile(path);
    if (!bytes) return std::unexpected(bytes.error());
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(bytes->data(), int(bytes->size()), &w, &h, &n))
      return fail("Not a decodable image: " + pathToUtf8(path));
    ShotMetadata m;
    m.sensorWidth = w;
    m.sensorHeight = h;
    m.cfa = CfaKind::None;
    m.bitsPerSample = stbi_is_16_bit_from_memory(bytes->data(), int(bytes->size())) ? 16 : 8;
    const ExifInfo exif = readContainer(*bytes).exif;
    m.make = exif.make;
    m.model = exif.model;
    if (exif.orientation >= 5) std::swap(m.sensorWidth, m.sensorHeight);
    return m;
  }

  Result<SourceImage> decode(const std::filesystem::path& path, const DecodeOptions&) const override {
    auto bytes = readFile(path);
    if (!bytes) return std::unexpected(bytes.error());
    int w = 0, h = 0, n = 0;
    const bool deep = stbi_is_16_bit_from_memory(bytes->data(), int(bytes->size())) != 0;
    std::unique_ptr<void, void (*)(void*)> px(nullptr, stbi_image_free);
    if (deep) px.reset(stbi_load_16_from_memory(bytes->data(), int(bytes->size()), &w, &h, &n, 3));
    else px.reset(stbi_load_from_memory(bytes->data(), int(bytes->size()), &w, &h, &n, 3));
    if (!px) return fail("JPEG decode failed: " + std::string(stbi_failure_reason()));

    // Colour space from the container's own tags / profile (sRGB when there are none: that path is unchanged, bit for bit).
    const Container ct = readContainer(*bytes);
    const color::SourceSpace space = color::resolveSourceSpace(ct.tags);
    logOnce(path, space.note);
    const auto& toPro = space.toProPhoto;
    const int levels = deep ? 65536 : 256;
    const std::vector<float> lut[3] = {space.lut(0, levels), space.lut(1, levels), space.lut(2, levels)};

    SourceImage out;
    out.sceneReferred = false;
    const ExifInfo& exif = ct.exif;
    out.meta.make = exif.make;
    out.meta.model = exif.model;
    out.meta.sensorWidth = w;
    out.meta.sensorHeight = h;
    out.meta.cfa = CfaKind::None;
    out.meta.bitsPerSample = deep ? 16 : 8;
    out.linear = ImageF(w, h);
    auto convert = [&](const auto* src) {
      parallelRows(h, [&](int y) {
        const auto* s = src + std::size_t(y) * w * 3;
        float* d = out.linear.row(y);
        for (int x = 0; x < w; ++x) {
          const float lin[3] = {lut[0][s[3 * x]], lut[1][s[3 * x + 1]], lut[2][s[3 * x + 2]]};
          toPro.apply(lin, d + 3 * x);
        }
      });
    };
    if (deep) convert(static_cast<const std::uint16_t*>(px.get()));
    else convert(static_cast<const stbi_uc*>(px.get()));
    if (exif.orientation > 1) {
      out.linear = applyOrientation(out.linear, exif.orientation);
      out.meta.sensorWidth = out.linear.width;
      out.meta.sensorHeight = out.linear.height;
    }
    return out;
  }
};

}  // namespace

Result<Image8> loadReferenceJpeg(const std::filesystem::path& path) {
  auto bytes = readFile(path);
  if (!bytes) return std::unexpected(bytes.error());
  int w = 0, h = 0, n = 0;
  std::unique_ptr<stbi_uc, void (*)(void*)> px(stbi_load_from_memory(bytes->data(), int(bytes->size()), &w, &h, &n, 3), stbi_image_free);
  if (!px) return fail("JPEG decode failed: " + std::string(stbi_failure_reason()));
  const ExifInfo exif = readContainer(*bytes).exif;
  Image8 out;
  if (exif.orientation > 1 && exif.orientation <= 8) {
    ImageF f(w, h);
    for (std::size_t i = 0; i < f.data.size(); ++i) f.data[i] = px.get()[i] / 255.f;
    f = applyOrientation(f, exif.orientation);
    out.width = f.width, out.height = f.height;
    out.data.resize(f.data.size());
    for (std::size_t i = 0; i < f.data.size(); ++i) out.data[i] = std::uint8_t(std::clamp(f.data[i], 0.f, 1.f) * 255.f + 0.5f);
  } else {
    out.width = w, out.height = h;
    out.data.assign(px.get(), px.get() + std::size_t(w) * h * 3);
  }
  return out;
}

std::unique_ptr<ImageDecoder> makeJpegDecoder() { return std::make_unique<JpegDecoder>(); }

DecoderRegistry DecoderRegistry::withBuiltins() {
  DecoderRegistry r;
  r.add(makeLibRawDecoder());
  r.add(makeJpegDecoder());
  return r;
}

void DecoderRegistry::add(std::unique_ptr<ImageDecoder> decoder) { decoders_.push_back(std::move(decoder)); }

const ImageDecoder* DecoderRegistry::find(const std::filesystem::path& path) const {
  for (const auto& d : decoders_)
    if (d->canDecode(path)) return d.get();
  return nullptr;
}

}  // namespace dapple
