// Camera JPEGs (and other 8-bit images). They are display-referred sRGB, so
// they are linearized and moved into the common linear ProPhoto space.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <cstring>
#include <memory>
#include <vector>

#include "../color.h"
#include "dapple/decoder.h"
#include "file_util.h"
#include "parallel.h"

namespace dapple {

namespace {

struct ExifInfo {
  int orientation = 1;
  std::string make, model;
};

// Minimal EXIF reader: IFD0 Make/Model/Orientation from the APP1 segment.
ExifInfo readExif(const std::vector<std::uint8_t>& b) {
  ExifInfo info;
  std::size_t pos = 2;
  while (pos + 4 < b.size() && b[pos] == 0xFF) {
    const int marker = b[pos + 1];
    if (marker == 0xDA || marker == 0xD9) break;
    const std::size_t len = (std::size_t(b[pos + 2]) << 8) | b[pos + 3];
    if (marker == 0xE1 && len > 16 && pos + 2 + len <= b.size() && std::memcmp(&b[pos + 4], "Exif\0\0", 6) == 0) {
      const std::uint8_t* t = &b[pos + 10];
      const std::size_t tlen = len - 8;
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
      break;
    }
    pos += 2 + len;
  }
  return info;
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
    m.bitsPerSample = 8;
    const ExifInfo exif = readExif(*bytes);
    m.make = exif.make;
    m.model = exif.model;
    if (exif.orientation >= 5) std::swap(m.sensorWidth, m.sensorHeight);
    return m;
  }

  Result<SourceImage> decode(const std::filesystem::path& path, const DecodeOptions&) const override {
    auto bytes = readFile(path);
    if (!bytes) return std::unexpected(bytes.error());
    int w = 0, h = 0, n = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> px(
        stbi_load_from_memory(bytes->data(), int(bytes->size()), &w, &h, &n, 3), stbi_image_free);
    if (!px) return fail("JPEG decode failed: " + std::string(stbi_failure_reason()));

    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = color::srgbDecode(i / 255.f);
    const auto& toPro = color::srgbToProPhoto();

    SourceImage out;
    out.sceneReferred = false;
    const ExifInfo exif = readExif(*bytes);
    out.meta.make = exif.make;
    out.meta.model = exif.model;
    out.meta.sensorWidth = w;
    out.meta.sensorHeight = h;
    out.meta.cfa = CfaKind::None;
    out.meta.bitsPerSample = 8;
    out.linear = ImageF(w, h);
    parallelRows(h, [&](int y) {
      const stbi_uc* s = px.get() + std::size_t(y) * w * 3;
      float* d = out.linear.row(y);
      for (int x = 0; x < w; ++x) {
        const float lin[3] = {lut[s[3 * x]], lut[s[3 * x + 1]], lut[s[3 * x + 2]]};
        toPro.apply(lin, d + 3 * x);
      }
    });
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
  const ExifInfo exif = readExif(*bytes);
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
