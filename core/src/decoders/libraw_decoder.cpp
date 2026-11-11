// RAW decoding through LibRaw. Covers Sony ARW, Fujifilm RAF (X-Trans),
// Canon, Nikon, DNG and the rest of LibRaw's camera list.

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

#include <stb_image.h>

#include "dapple/decoder.h"
#include "file_util.h"
#include "parallel.h"

namespace dapple {

namespace {

std::string sonyRawFormat(const libraw_sony_info_t& s) {
  std::string kind;
  switch (s.SonyRawFileType) {
    case 0: kind = "uncompressed 14-bit"; break;
    case 1: kind = "uncompressed 12-bit"; break;
    case 2: kind = "compressed (lossy)"; break;
    case 3: kind = "lossless compressed"; break;
    case 4: kind = "lossless compressed v2"; break;
    default: return {};
  }
  switch (s.RawSizeType) {
    case 1: kind += " (L)"; break;
    case 2: kind += " (S)"; break;
    case 3: kind += " (M)"; break;
    default: break;
  }
  return kind;
}

// Fujifilm film simulation codes (FilmMode maker note), best effort.
std::string fujiFilmSimulation(unsigned mode) {
  switch (mode) {
    case 0x000: return "Provia/Standard";
    case 0x100: case 0x110: case 0x120: case 0x130: case 0x300: return "Studio Portrait";
    case 0x200: case 0x400: return "Velvia/Vivid";
    case 0x500: return "Pro Neg. Std";
    case 0x501: return "Pro Neg. Hi";
    case 0x600: return "Classic Chrome";
    case 0x700: return "Eterna";
    case 0x800: return "Classic Negative";
    case 0x900: return "Eterna Bleach Bypass";
    case 0xa00: return "Nostalgic Neg";
    case 0xb00: return "Reala Ace";
    default: return {};
  }
}

ShotMetadata readMetadata(const LibRaw& raw) {
  const auto& d = raw.imgdata;
  ShotMetadata m;
  m.make = d.idata.normalized_make[0] ? d.idata.normalized_make : d.idata.make;
  m.model = d.idata.normalized_model[0] ? d.idata.normalized_model : d.idata.model;
  m.lens = d.lens.Lens;
  m.iso = d.other.iso_speed;
  m.shutterSeconds = d.other.shutter;
  m.aperture = d.other.aperture;
  m.focalLengthMm = d.other.focal_len;
  m.timestamp = static_cast<long long>(d.other.timestamp);

  const bool swap = (d.sizes.flip & 4) != 0;
  m.sensorWidth = swap ? d.sizes.height : d.sizes.width;
  m.sensorHeight = swap ? d.sizes.width : d.sizes.height;

  if (d.idata.filters == 9) {
    m.cfa = CfaKind::XTrans;
  } else if (d.idata.filters != 0) {
    m.cfa = CfaKind::Bayer;
  } else if (d.idata.dng_version != 0) {
    m.cfa = CfaKind::None;  // linear DNG
  } else if (m.make == "Sony" && (d.makernotes.sony.RawSizeType == 2 || d.makernotes.sony.RawSizeType == 3)) {
    // Sony M/S RAW (e.g. A7R V "M" = 26 MP) is a binned, already 3-colour
    // image without a CFA mosaic, so half-size decoding gains nothing.
    m.cfa = CfaKind::None;
  } else {
    // LibRaw has not resolved the pattern at this stage; the camera profile
    // knows the sensor type.
    m.cfa = CfaKind::Unknown;
  }
  if (d.color.raw_bps > 0 && d.color.raw_bps <= 32) {
    m.bitsPerSample = int(d.color.raw_bps);
  } else {
    const unsigned maxValue = d.color.maximum;
    m.bitsPerSample = maxValue > 0 ? int(std::ceil(std::log2(double(maxValue) + 1))) : 0;
  }

  const float g = d.color.cam_mul[1] > 0 ? d.color.cam_mul[1] : 1.f;
  for (int c = 0; c < 3; ++c) m.asShotWb[c] = d.color.cam_mul[c] > 0 ? d.color.cam_mul[c] / g : 1.f;

  if (m.make == "Sony") m.rawFormat = sonyRawFormat(d.makernotes.sony);
  if (m.make == "Fujifilm") m.inCameraLook = fujiFilmSimulation(d.makernotes.fuji.FilmMode);
  return m;
}

// LibRaw datastream over std::filebuf opened with a std::filesystem::path, so
// non-ASCII paths work on Windows and LibRaw only reads the bytes it needs
// (no 60-120 MB copy for probing or thumbnail extraction).
class PathDatastream final : public LibRaw_abstract_datastream {
 public:
  explicit PathDatastream(const std::filesystem::path& path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (!ec) size_ = static_cast<INT64>(sz);
    buf_.open(path, std::ios::in | std::ios::binary);
  }
  int valid() override { return buf_.is_open() ? 1 : 0; }
  int read(void* ptr, size_t size, size_t nmemb) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    return int(buf_.sgetn(static_cast<char*>(ptr), std::streamsize(size * nmemb)) / (size > 0 ? size : 1));
  }
  int seek(INT64 o, int whence) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    std::ios_base::seekdir dir = whence == SEEK_CUR ? std::ios_base::cur
                                 : whence == SEEK_END ? std::ios_base::end
                                                      : std::ios_base::beg;
    return buf_.pubseekoff(std::streamoff(o), dir) < 0;
  }
  INT64 tell() override { return buf_.pubseekoff(0, std::ios_base::cur); }
  INT64 size() override { return size_; }
  int get_char() override { return buf_.sbumpc(); }
  char* gets(char* str, int sz) override {
    if (sz < 1) return nullptr;
    std::istream is(&buf_);
    is.getline(str, sz);
    return is.fail() ? nullptr : str;
  }
  int scanf_one(const char* fmt, void* val) override {
    std::istream is(&buf_);
    if (std::strcmp(fmt, "%d") == 0) {
      int d;
      is >> d;
      if (is.fail()) return EOF;
      *static_cast<int*>(val) = d;
    } else {
      float f;
      is >> f;
      if (is.fail()) return EOF;
      *static_cast<float*>(val) = f;
    }
    return 1;
  }
  int eof() override { return buf_.sgetc() == EOF; }

 private:
  std::filebuf buf_;
  INT64 size_ = 0;
};

class LibRawDecoder final : public ImageDecoder {
 public:
  const char* name() const override { return "libraw"; }

  bool canDecode(const std::filesystem::path& path) const override {
    static const char* kExts[] = {".arw", ".srf", ".sr2", ".raf", ".dng", ".cr2", ".cr3", ".nef",
                                  ".nrw", ".orf", ".rw2", ".pef", ".srw", ".3fr", ".iiq", ".erf"};
    const std::string ext = lowerExtension(path);
    return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
  }

  Result<ShotMetadata> probe(const std::filesystem::path& path) const override {
    PathDatastream stream(path);
    if (!stream.valid()) return fail("Cannot open " + pathToUtf8(path));
    auto raw = std::make_unique<LibRaw>();
    if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
      return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
    return readMetadata(*raw);
  }

  Result<SourceImage> decode(const std::filesystem::path& path, const DecodeOptions& options) const override {
    PathDatastream stream(path);
    if (!stream.valid()) return fail("Cannot open " + pathToUtf8(path));

    auto raw = std::make_unique<LibRaw>();  // large object, keep off the stack
    auto& params = raw->imgdata.params;
    params.output_color = 4;  // ProPhoto RGB
    params.output_bps = 16;
    params.gamm[0] = 1.0;     // linear output
    params.gamm[1] = 1.0;
    params.no_auto_bright = 1;
    params.use_camera_wb = 1;
    params.adjust_maximum_thr = 0.f;  // keep white level = sensor clipping point (no per-frame brightening)
    params.highlight = 0;     // clip; highlight reconstruction is done by us later
    params.half_size = options.quality == DecodeQuality::Preview ? 1 : 0;
    params.user_qual = 3;     // AHD for Bayer; X-Trans uses its own 3-pass method

    if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
      return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
    if (int rc = raw->unpack(); rc != LIBRAW_SUCCESS)
      return fail("LibRaw unpack failed: " + std::string(libraw_strerror(rc)));
    const ShotMetadata meta = readMetadata(*raw);  // before processing alters sizes/filters
    // Linear DNGs written by Dapple's HDR/panorama merge store value / headroom and record
    // BaselineExposure = log2(headroom); undo it so merged values above 1.0 survive a round trip.
    float dappleGain = 1.f;
    if (std::strcmp(raw->imgdata.color.UniqueCameraModel, "Dapple Linear Merge") == 0 &&
        raw->imgdata.color.dng_levels.baseline_exposure > -50.f && raw->imgdata.color.dng_levels.baseline_exposure < 16.f)
      dappleGain = std::exp2(raw->imgdata.color.dng_levels.baseline_exposure);
    if (int rc = raw->dcraw_process(); rc != LIBRAW_SUCCESS)
      return fail("LibRaw processing failed: " + std::string(libraw_strerror(rc)));

    int rc = 0;
    libraw_processed_image_t* mem = raw->dcraw_make_mem_image(&rc);
    if (!mem || rc != LIBRAW_SUCCESS)
      return fail("LibRaw image export failed: " + std::string(libraw_strerror(rc)));
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> guard(
        mem, [](libraw_processed_image_t* p) { LibRaw::dcraw_clear_mem(p); });
    if (mem->type != LIBRAW_IMAGE_BITMAP || mem->colors != 3 || mem->bits != 16)
      return fail("Unexpected LibRaw output layout");

    // Free LibRaw's own image buffers before allocating the float copy.
    raw->recycle();

    SourceImage out;
    out.meta = meta;
    out.sceneReferred = true;
    out.linear = ImageF(mem->width, mem->height);
    const auto* src = reinterpret_cast<const std::uint16_t*>(mem->data);
    const int w = mem->width;
    const float kScale = dappleGain / 65535.f;
    parallelRows(mem->height, [&](int y) {
      const std::uint16_t* s = src + std::size_t(y) * w * 3;
      float* d = out.linear.row(y);
      for (int i = 0; i < w * 3; ++i) d[i] = s[i] * kScale;
    });
    guard.reset();  // release the 16-bit copy
    return out;
  }
};

Image8 rotate90(const Image8& src, bool clockwise) {
  Image8 dst;
  dst.width = src.height;
  dst.height = src.width;
  dst.data.resize(src.data.size());
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x) {
      const int dx = clockwise ? src.height - 1 - y : y;
      const int dy = clockwise ? x : src.width - 1 - x;
      std::memcpy(&dst.data[(std::size_t(dy) * dst.width + dx) * 3], &src.data[(std::size_t(y) * src.width + x) * 3], 3);
    }
  return dst;
}

}  // namespace

Result<Image8> extractEmbeddedPreview(const std::filesystem::path& rawPath, int longEdge) {
  PathDatastream stream(rawPath);
  if (!stream.valid()) return fail("Cannot open " + pathToUtf8(rawPath));
  auto raw = std::make_unique<LibRaw>();
  if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
    return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
  if (int rc = raw->unpack_thumb(); rc != LIBRAW_SUCCESS)
    return fail("No embedded preview: " + std::string(libraw_strerror(rc)));
  const auto& th = raw->imgdata.thumbnail;
  if (th.tformat != LIBRAW_THUMBNAIL_JPEG || !th.thumb || th.tlength == 0)
    return fail("Embedded preview is not a JPEG");
  int w = 0, h = 0, n = 0;
  std::unique_ptr<stbi_uc, void (*)(void*)> px(
      stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(th.thumb), int(th.tlength), &w, &h, &n, 3),
      stbi_image_free);
  if (!px) return fail("Preview decode failed: " + std::string(stbi_failure_reason()));

  Image8 img;
  img.width = w;
  img.height = h;
  img.data.assign(px.get(), px.get() + std::size_t(w) * h * 3);

  // Embedded previews usually keep sensor orientation; rotate when the file
  // says portrait but the preview is landscape.
  const int flip = raw->imgdata.sizes.flip;
  if ((flip & 4) != 0 && w > h) img = rotate90(img, flip == 6);

  if (longEdge > 0 && std::max(img.width, img.height) > longEdge) {
    ImageF f(img.width, img.height);
    for (std::size_t i = 0; i < f.data.size(); ++i) f.data[i] = img.data[i] / 255.f;
    f = resizeToLongEdge(f, longEdge);
    img.width = f.width;
    img.height = f.height;
    img.data.resize(f.data.size());
    for (std::size_t i = 0; i < f.data.size(); ++i) img.data[i] = std::uint8_t(std::clamp(f.data[i], 0.f, 1.f) * 255.f + 0.5f);
  }
  return img;
}

std::unique_ptr<ImageDecoder> makeLibRawDecoder() { return std::make_unique<LibRawDecoder>(); }

}  // namespace dapple
