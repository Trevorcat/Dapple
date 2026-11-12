// Host test (no FFmpeg): an Ultra HDR cover (primary + MPF + gain map) must survive buildMotionPhoto with the gain map
// listed in the Container directory, the MPF index re-pointed at it, and the video as the last item.
// Build + run: tools/ffmpeg/test-host.sh
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dapple/motion_photo.h"

using Bytes = std::vector<std::uint8_t>;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::printf("  FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static Bytes load(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::uint32_t be32(const Bytes& b, size_t o) { return (std::uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]; }

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  Bytes cover = load(dir + "/ultrahdr_cover.jpg"), mp4 = load(dir + "/hw_h264_420_8.mp4");
  CHECK(!cover.empty() && !mp4.empty(), "fixtures missing");
  if (cover.empty() || mp4.empty()) return 2;
  dapple::MotionPhotoOptions opt;
  opt.presentationTimestampUs = 300000;  // inside the 0.6 s fixture clip
  auto r = dapple::buildMotionPhoto(cover, mp4, opt);
  CHECK(r.has_value(), "build: %s", r ? "" : r.error().message.c_str());
  if (!r) return 1;
  const Bytes& f = *r;
  auto info = dapple::validateMotionPhoto(f);
  for (auto& p : info.problems) std::printf("  problem: %s\n", p.c_str());
  CHECK(info.valid, "validator rejects the Ultra HDR motion photo");
  CHECK(info.videoLength == mp4.size() && info.videoOffset + info.videoLength == f.size(), "video is the file tail");
  CHECK(f.size() > mp4.size() && std::memcmp(f.data() + f.size() - mp4.size(), mp4.data(), mp4.size()) == 0, "video bytes intact");
  std::string s(f.begin(), f.end());
  size_t gm = (size_t)info.jpegLength;
  CHECK(f[gm] == 0xFF && f[gm + 1] == 0xD8, "gain map JPEG right after the primary (offset %zu)", gm);
  CHECK(s.find("Semantic=\"GainMap\"") != std::string::npos && s.find("Semantic=\"MotionPhoto\"") != std::string::npos, "directory lists the gain map and the video");
  CHECK(s.find("hdrgm:Version=\"1.0\"") != std::string::npos, "hdrgm attributes kept");
  CHECK(s.find("GCamera:MotionPhoto=\"1\"") != std::string::npos, "motion photo flag");
  size_t m = s.find(std::string("MPF\0", 4));
  CHECK(m != std::string::npos, "MPF present");
  if (m != std::string::npos) {
    size_t T = m + 4;
    size_t ifd = T + be32(f, T + 4);
    unsigned n = (f[ifd] << 8) | f[ifd + 1];
    bool found = false;
    for (unsigned i = 0; i < n; i++) {
      size_t e = ifd + 2 + i * 12;
      if (((f[e] << 8) | f[e + 1]) != 0xB002) continue;
      size_t arr = T + be32(f, e + 8);
      found = true;
      CHECK(be32(f, arr + 4) == info.jpegLength, "MPF primary size %u vs %llu", be32(f, arr + 4), (unsigned long long)info.jpegLength);
      CHECK(T + be32(f, arr + 16 + 8) == gm, "MPF gain map offset points at %zu, gain map is at %zu", T + be32(f, arr + 16 + 8), gm);
    }
    CHECK(found, "MP entry tag");
  }
  // a cover without a second image is a plain Motion Photo
  Bytes plain(cover.begin(), cover.begin() + (std::ptrdiff_t)info.jpegLength);
  dapple::MotionPhotoOptions o2;
  auto p = dapple::buildMotionPhoto(plain, mp4, o2);
  CHECK(p.has_value() && dapple::validateMotionPhoto(*p).valid, "plain cover");
  std::printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
