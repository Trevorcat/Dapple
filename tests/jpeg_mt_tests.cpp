// The strip-parallel baseline JPEG encoder (core/src/encode.cpp): the same decoded pixels as the single-threaded stb encode, decodable by two independent
// decoders (stb_image and core/src/decoders/jpeg_dc.cpp, which handles restart markers on its own), writeJpeg and encodeJpeg byte-identical.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#include "dapple/encode.h"
#include "decoders/jpeg_dc.h"
#include "test_tmp.h"

namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

dapple::Image8 synth(int w, int h, unsigned seed) {
  dapple::Image8 im;
  im.width = w, im.height = h;
  im.data.resize(std::size_t(w) * h * 3);
  std::mt19937 rng(seed);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double r2 = std::hypot(x - w / 2.0, y - h / 2.0);
      const int v = int(127.5 + 120 * std::sin(r2 * r2 / 40000.0));  // zone plate: every frequency
      std::uint8_t* p = &im.data[(std::size_t(y) * w + x) * 3];
      p[0] = std::uint8_t(std::min(255, std::max(0, v + int(rng() % 9) - 4)));
      p[1] = std::uint8_t(std::min(255, std::max(0, (x * 255) / w + int(rng() % 5) - 2)));
      p[2] = std::uint8_t(std::min(255, std::max(0, (y * 255) / h + int(rng() % 5) - 2)));
    }
  return im;
}
void sink(void* c, void* d, int n) { static_cast<std::vector<std::uint8_t>*>(c)->insert(static_cast<std::vector<std::uint8_t>*>(c)->end(), (const std::uint8_t*)d, (const std::uint8_t*)d + n); }
double ms(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
}  // namespace

int main() {
  const int sizes[][2] = {{1800, 1701}, {3001, 1100}, {2500, 2600}};
  for (const auto& sz : sizes)
    for (int q : {92, 85}) {
      const dapple::Image8 im = synth(sz[0], sz[1], unsigned(sz[0] + q));
      dapple::JpegOptions jo;
      jo.quality = q;
      auto t0 = std::chrono::steady_clock::now();
      auto a = dapple::encodeJpeg(im, jo);
      const double msA = ms(t0);
      std::vector<std::uint8_t> b;
      t0 = std::chrono::steady_clock::now();
      stbi_write_jpg_to_func(sink, &b, im.width, im.height, 3, im.data.data(), q);
      const double msB = ms(t0);
      CHECK(bool(a), "encode failed");
      if (!a) continue;
      bool dri = false;
      for (std::size_t i = 0; i + 3 < a->size() && i < 700; ++i) dri |= (*a)[i] == 0xFF && (*a)[i + 1] == 0xDD;
      CHECK(dri, "no restart interval in the threaded stream");
      CHECK(a->size() > 2 && (*a)[a->size() - 2] == 0xFF && a->back() == 0xD9, "no EOI");
      int w1, h1, n1, w2, h2, n2;
      stbi_uc* pa = stbi_load_from_memory(a->data(), int(a->size()), &w1, &h1, &n1, 3);
      stbi_uc* pb = stbi_load_from_memory(b.data(), int(b.size()), &w2, &h2, &n2, 3);
      CHECK(pa && pb && w1 == w2 && h1 == h2, "stb_image decode failed or sizes differ");
      long diff = 0;
      if (pa && pb && w1 == w2 && h1 == h2) diff = std::memcmp(pa, pb, std::size_t(w1) * h1 * 3) != 0;
      CHECK(diff == 0, "decoded pixels differ between the strip encode and the single-threaded encode");
      stbi_image_free(pa);
      stbi_image_free(pb);
      dapple::JpegDcImage da, db;  // independent decoder: restart handling
      const bool oka = dapple::decodeJpegDcScale(a->data(), a->size(), da), okb = dapple::decodeJpegDcScale(b.data(), b.size(), db);
      CHECK(oka && okb && da.width == db.width && da.height == db.height && da.rgb == db.rgb, "second decoder: reduced pictures differ (%d %d)", oka, okb);
      const auto path = std::filesystem::temp_directory_path() / ("dapple_jpeg_mt_" + testUniqueTag() + ".jpg");
      CHECK(bool(dapple::writeJpeg(im, path, jo)), "writeJpeg");
      std::ifstream f(path, std::ios::binary);
      std::vector<std::uint8_t> fb((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      CHECK(fb == *a, "writeJpeg and encodeJpeg bytes differ");
      std::error_code ec;
      std::filesystem::remove(path, ec);
      std::printf("  %dx%d q%d: strips %.0f ms (%zu bytes), single %.0f ms (%zu bytes)\n", sz[0], sz[1], q, msA, a->size(), msB, b.size());
    }
  std::printf("%s\n", g_fail ? "FAIL jpeg_mt" : "PASS jpeg_mt");
  return g_fail ? 1 : 0;
}
