// Ultra HDR cover of an HDR Motion Photo (docs/engine/pipeline-architecture.md section 30.14), host side, no FFmpeg:
//   dapple_video_edit_cover_uhdr (HLG cover frame + the still's SDR JPEG) -> gain-map JPEG whose base image is the SDR cover byte for byte;
//   buildMotionPhoto accepts it: one XMP directory Primary / GainMap / MotionPhoto, MPF index, video at the tail; both readers' rules are checked;
//   dapple_motion_photo_validate_json reports and rejects the combination.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"
#include "dapple/encode.h"
#include "dapple/motion_photo.h"
#include "dapple/ultrahdr.h"
#include "../test_tmp.h"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::uint16_t toHalf(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  const std::uint32_t s = (b >> 16) & 0x8000u;
  int e = int((b >> 23) & 0xff) - 127 + 15;
  std::uint32_t m = b & 0x7fffffu;
  if (e <= 0) return std::uint16_t(s);
  if (e >= 31) return std::uint16_t(s | 0x7bffu);
  std::uint32_t r = s | (std::uint32_t(e) << 10) | (m >> 13);
  if (m & 0x1000u) ++r;
  return std::uint16_t(r);
}
static Bytes load(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void save(const fs::path& p, const Bytes& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static std::uint32_t be32(const Bytes& b, size_t o) { return (std::uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]; }

// 320x180 clip frame: grey ramp, a patch (x 200..239, y 60..99) at `patch` x SDR white
static std::vector<std::uint16_t> frame(float patch) {
  const int w = 320, h = 180;
  std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const bool inPatch = x >= 200 && x < 240 && y >= 60 && y < 100;
      const float v = inPatch ? patch : 0.18f + 0.1f * float(x) / w;
      std::uint16_t* p = &px[(std::size_t(y) * w + x) * 4];
      p[0] = p[1] = p[2] = toHalf(v);
      p[3] = 0x3c00;
    }
  return px;
}
// the still of the edit "rotate 90" of that frame is upright 180x320; a plain 360x640 gradient (the same aspect) stands in for the SDR cover
static Bytes sdrCover(const fs::path& path) {
  dapple::Image8 img;
  img.width = 360;
  img.height = 640;
  img.data.resize(std::size_t(360) * 640 * 3);
  for (int y = 0; y < 640; ++y)
    for (int x = 0; x < 360; ++x)
      for (int c = 0; c < 3; ++c) img.data[(std::size_t(y) * 360 + x) * 3 + c] = std::uint8_t(60 + (x * 100) / 360 + (y * 60) / 640 + c);
  auto b = dapple::encodeJpeg(img);
  CHECK(b.has_value(), "encode the SDR cover");
  save(path, *b);
  return *b;
}

struct Run {
  int rc = 1;
  DappleHdrInfo info{};
};
static Run cover(const fs::path& sdr, const fs::path& out, float patch, int kind, int force, const char* params = "{\"rot\":90}") {
  Run r;
  DappleVideoEdit* e = dapple_video_edit_open(params, nullptr, "{\"stillLongEdge\":640}");
  CHECK(e != nullptr, "open: %s", dapple_video_edit_last_error());
  if (!e) return r;
  const auto px = frame(patch);
  r.rc = dapple_video_edit_cover_uhdr(e, px.data(), 320, 180, kind, 0, sdr.string().c_str(), out.string().c_str(), force, &r.info);
  if (r.rc != DAPPLE_OK) std::printf("  cover_uhdr: %s\n", dapple_video_edit_last_error());
  dapple_video_edit_close(e);
  return r;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string dir = argc > 1 ? argv[1] : std::string(DAPPLE_SOURCE_DIR) + "/tests/data/video";
  const fs::path tmp = fs::temp_directory_path() / ("dapple_cover_uhdr_" + testUniqueTag());
  fs::create_directories(tmp);
  const Bytes base = sdrCover(tmp / "sdr.jpg");

  // 1. HLG cover frame with a 4x SDR white patch, edit = rotate 90: a gain map; the base image is the SDR cover byte for byte
  Run a = cover(tmp / "sdr.jpg", tmp / "cover_hdr.jpg", 4.f, 2, 0);
  CHECK(a.rc == DAPPLE_OK && a.info.written == 1 && a.info.reason == 0, "HLG cover: rc %d written %d reason %d", a.rc, a.info.written, a.info.reason);
  std::printf("cover HLG patch 4x: written %d, peak %.2f, gain max %.3f stops, capacity %.3f\n", a.info.written, a.info.peak, a.info.gain_max, a.info.capacity_max);
  CHECK(a.info.peak > 3.f && a.info.gain_max > 1.f, "headroom: peak %.2f gain %.3f", a.info.peak, a.info.gain_max);
  const Bytes uhdr = load(tmp / "cover_hdr.jpg");
  auto parsed = dapple::uhdr::parse(uhdr);
  CHECK(parsed.has_value(), "the cover parses as Ultra HDR: %s", parsed ? "" : parsed.error().message.c_str());
  if (parsed) {
    CHECK(parsed->base == base, "base image differs from the SDR cover (%zu vs %zu bytes)", parsed->base.size(), base.size());
    CHECK(parsed->gainSemantic == "GainMap" && parsed->directoryLength == parsed->gainSize, "directory GainMap length %zu vs gain map %zu", parsed->directoryLength,
          parsed->gainSize);
    CHECK(std::fabs(parsed->meta.gainMax - a.info.gain_max) < 0.002f, "gain max in the file %.3f vs %.3f", parsed->meta.gainMax, a.info.gain_max);
    CHECK(parsed->gainComponents == 1 && parsed->gainWidth >= 90, "gain map %dx%d, %d components", parsed->gainWidth, parsed->gainHeight, parsed->gainComponents);
    std::printf("  gain map %dx%d, base %zu bytes unchanged, primary with segments %zu, gain JPEG %zu\n", parsed->gainWidth, parsed->gainHeight, base.size(), parsed->primaryEnd,
                parsed->gainSize);
  }

  // 2. no headroom / SDR clip: the SDR cover is copied unchanged, written = 0
  Run b = cover(tmp / "sdr.jpg", tmp / "cover_flat.jpg", 0.9f, 2, 0);
  CHECK(b.rc == DAPPLE_OK && b.info.written == 0 && b.info.reason == 1, "no headroom: written %d reason %d peak %.2f", b.info.written, b.info.reason, b.info.peak);
  CHECK(load(tmp / "cover_flat.jpg") == base, "no headroom: the cover is the SDR still");
  Run s = cover(tmp / "sdr.jpg", tmp / "cover_sdrclip.jpg", 4.f, 0, 1);
  CHECK(s.rc == DAPPLE_OK && s.info.written == 0 && s.info.reason == 1, "SDR clip: written %d reason %d", s.info.written, s.info.reason);
  CHECK(load(tmp / "cover_sdrclip.jpg") == base, "SDR clip: the cover is the SDR still");
  // the export option lowers the threshold: a 1.3x highlight is not worth it by itself, it is when the user asked for HDR
  Run m0 = cover(tmp / "sdr.jpg", tmp / "cover_m0.jpg", 1.3f, 2, 0), m1 = cover(tmp / "sdr.jpg", tmp / "cover_m1.jpg", 1.3f, 2, 1);
  std::printf("modest highlight 1.3x: auto written %d (peak %.2f), HDR option written %d\n", m0.info.written, m0.info.peak, m1.info.written);
  CHECK(m0.info.written == 0 && m1.info.written == 1, "force_hdr: auto %d, option %d", m0.info.written, m1.info.written);
  // a cover of another shape is not paired
  {
    dapple::Image8 wide;
    wide.width = 640;
    wide.height = 360;
    wide.data.assign(std::size_t(640) * 360 * 3, 128);
    save(tmp / "wide.jpg", *dapple::encodeJpeg(wide));
    Run w = cover(tmp / "wide.jpg", tmp / "cover_wide.jpg", 4.f, 2, 0);
    CHECK(w.rc == DAPPLE_OK && w.info.written == 0 && w.info.reason == 3, "unpaired: written %d reason %d", w.info.written, w.info.reason);
  }
  // PQ clip
  Run g = cover(tmp / "sdr.jpg", tmp / "cover_pq.jpg", 4.f, 3, 0);
  CHECK(g.rc == DAPPLE_OK && g.info.written == 1, "PQ cover: written %d reason %d", g.info.written, g.info.reason);

  // 3. the Motion Photo with this cover
  Bytes mp4 = load(dir + "/hw_h264_420_8.mp4");
  CHECK(!mp4.empty(), "fixture clip missing");
  if (mp4.empty() || !parsed) return 2;
  dapple::MotionPhotoOptions opt;
  opt.presentationTimestampUs = 300000;
  auto built = dapple::buildMotionPhoto(uhdr, mp4, opt);
  CHECK(built.has_value(), "build: %s", built ? "" : built.error().message.c_str());
  if (!built) return 1;
  const Bytes& f = *built;
  const auto info = dapple::validateMotionPhoto(f);
  for (auto& p : info.problems) std::printf("  problem: %s\n", p.c_str());
  CHECK(info.valid && info.hasGainMap, "validator: valid %d gain map %d", int(info.valid), int(info.hasGainMap));
  // Motion Photo reader (Google): the video is the tail, Item:Length of the last item, ftyp there
  CHECK(info.videoLength == mp4.size() && info.videoOffset == f.size() - mp4.size() && std::memcmp(f.data() + info.videoOffset, mp4.data(), mp4.size()) == 0,
        "video at the tail, intact");
  CHECK(std::memcmp(f.data() + info.videoOffset + 4, "ftyp", 4) == 0, "ftyp at the video offset");
  // Ultra HDR reader: the MPF index points at the gain map, which is the gain-map JPEG of the cover byte for byte; the directory agrees
  CHECK(info.gainMapOffset == info.jpegLength && info.gainMapLength == parsed->gainSize, "gain map at %llu (+%llu), expected %llu (+%zu)",
        (unsigned long long)info.gainMapOffset, (unsigned long long)info.gainMapLength, (unsigned long long)info.jpegLength, parsed->gainSize);
  CHECK(std::equal(parsed->gainJpeg.begin(), parsed->gainJpeg.end(), f.begin() + std::ptrdiff_t(info.gainMapOffset)), "gain map JPEG changed");
  CHECK(info.gainMapOffset + info.gainMapLength == info.videoOffset, "gain map ends where the video starts");
  const std::string s0(f.begin(), f.begin() + std::ptrdiff_t(info.jpegLength));
  for (const char* t : {"Item:Semantic=\"Primary\"", "Item:Semantic=\"GainMap\"", "Item:Semantic=\"MotionPhoto\"", "hdrgm:Version=\"1.0\"", "GCamera:MotionPhoto=\"1\""})
    CHECK(s0.find(t) != std::string::npos, "XMP lacks %s", t);
  CHECK(s0.find("Item:Semantic=\"GainMap\"") < s0.find("Item:Semantic=\"MotionPhoto\""), "directory order Primary, GainMap, MotionPhoto");
  CHECK(s0.find("http://ns.adobe.com/xap/1.0/") == s0.rfind("http://ns.adobe.com/xap/1.0/"), "exactly one XMP packet in the primary");
  // the Ultra HDR parser reads the head of the Motion Photo (everything before the video) and finds the original base
  auto p2 = dapple::uhdr::parse(Bytes(f.begin(), f.begin() + std::ptrdiff_t(info.videoOffset)));
  CHECK(p2.has_value() && p2->base == base, "Ultra HDR parse of the Motion Photo head: %s", p2 ? "base differs" : p2.error().message.c_str());
  CHECK(p2.has_value() && p2->mpfGainOffsetAbs == info.gainMapOffset && p2->mpfGainSize == info.gainMapLength, "MPF via the Ultra HDR parser: offset %zu size %u",
        p2 ? std::size_t(p2->mpfGainOffsetAbs) : std::size_t(0), p2 ? p2->mpfGainSize : 0u);
  std::printf("Motion Photo with Ultra HDR cover: %zu bytes = primary %llu + gain map %llu + video %llu; valid, MPF and directory agree\n", f.size(),
              (unsigned long long)info.jpegLength, (unsigned long long)info.gainMapLength, (unsigned long long)info.videoLength);

  // 4. validate_json: the combination is checked; broken pieces are named
  save(tmp / "hdr_MP.jpg", f);
  char* js = dapple_motion_photo_validate_json((tmp / "hdr_MP.jpg").string().c_str());
  const std::string j = js ? js : "";
  if (js) dapple_free_string(js);
  CHECK(j.find("\"valid\":true") != std::string::npos && j.find("\"gainMap\":true") != std::string::npos &&
            j.find("\"gainMapOffset\":" + std::to_string(info.gainMapOffset)) != std::string::npos,
        "validate_json: %s", j.c_str());
  {
    Bytes bad = f;
    const std::string hay(bad.begin(), bad.begin() + std::ptrdiff_t(info.jpegLength));
    const size_t mpf = hay.find(std::string("MPF\0", 4));
    CHECK(mpf != std::string::npos, "MPF segment");
    if (mpf != std::string::npos) {
      const size_t T = mpf + 4, ifd = T + be32(bad, T + 4);
      const unsigned n = (bad[ifd] << 8) | bad[ifd + 1];
      for (unsigned i = 0; i < n; i++) {
        const size_t e = ifd + 2 + i * 12;
        if (((bad[e] << 8) | bad[e + 1]) != 0xB002) continue;
        const size_t arr = T + be32(bad, e + 8);
        bad[arr + 16 + 11] ^= 0x04;  // gain map offset off by 4
      }
      const auto bi = dapple::validateMotionPhoto(bad);
      bool named = false;
      for (auto& p : bi.problems) named |= p.find("MPF gain map offset") != std::string::npos;
      CHECK(!bi.valid && named, "a wrong MPF gain map offset must be reported (valid %d)", int(bi.valid));
    }
    Bytes cut = f;  // the gain map item lies about its length: the directory no longer adds up
    const std::string h2(cut.begin(), cut.begin() + std::ptrdiff_t(info.jpegLength));
    const size_t at = h2.find("Item:Semantic=\"GainMap\"");
    const size_t len = at == std::string::npos ? std::string::npos : h2.find("Item:Length=\"", at);
    CHECK(len != std::string::npos, "gain item length attribute");
    if (len != std::string::npos) {
      cut[len + 13] = cut[len + 13] == '9' ? '8' : char(cut[len + 13] + 1);
      CHECK(!dapple::validateMotionPhoto(cut).valid, "a wrong gain map Item:Length must be rejected");
    }
  }
  // a plain SDR cover is still a valid, gain-map-free Motion Photo
  auto plain = dapple::buildMotionPhoto(base, mp4, opt);
  const auto pi = plain ? dapple::validateMotionPhoto(*plain) : dapple::MotionPhotoInfo{};
  CHECK(plain.has_value() && pi.valid && !pi.hasGainMap, "plain cover: valid %d gainMap %d", int(pi.valid), int(pi.hasGainMap));

  if (g_fail) {
    std::printf("cover_uhdr: %d check(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("cover_uhdr: PASS\n");
  return 0;
}
