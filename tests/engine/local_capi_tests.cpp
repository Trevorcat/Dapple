// Local adjustments through the C API (docs/engine/pipeline-architecture.md section 40.5): the same edit through every render / export call, the UI helpers
// (mask preview, point mapping, range sampling, raster store) and the Motion Photo cover render path. The C API is compiled in (as in uhdr_tests).
//   dapple_local_capi_tests <id>   exit 77: build/rawsamples/Fujifilm/X-T50/DSCF0118.RAF absent (ids render exports ui)
//     render   dapple_session_render with and without a local radial (+2 EV): brighter inside, unchanged far outside
//     exports  export_jpeg, export_jpeg_ex (p3), export_jpeg_hdr and the base previews: the same check on decoded files
//     ui       mask preview (output frame / photo space), map_point round trips, mask_sample_json, raster store
//     video    the video-frame render path of a Motion Photo cover (dapple_video_edit_render, no RAW needed)
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

static fs::path root() { return fs::path(DAPPLE_SOURCE_DIR); }

static fs::path rafPath() {
  const fs::path p = root() / "build" / "rawsamples" / "Fujifilm" / "X-T50" / "DSCF0118.RAF";
  return fs::exists(p) ? p : fs::path();
}

struct Pic {
  int w = 0, h = 0, c = 4;
  std::vector<std::uint8_t> d;
  bool ok() const { return w > 0 && h > 0 && !d.empty(); }
  float luma(int x, int y) const {
    const std::uint8_t* p = &d[(std::size_t(y) * w + x) * c];
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
  }
};
static Pic fromImage(DappleImage& im) {
  Pic p;
  p.w = im.width, p.h = im.height, p.c = 4;
  if (im.rgba) p.d.assign(im.rgba, im.rgba + std::size_t(im.width) * im.height * 4);
  dapple_free_image(&im);
  return p;
}
static Pic decodeFile(const fs::path& f) {
  Pic p;
  int c = 0;
  std::uint8_t* px = stbi_load(f.string().c_str(), &p.w, &p.h, &c, 4);
  if (!px) return Pic{};
  p.c = 4;
  p.d.assign(px, px + std::size_t(p.w) * p.h * 4);
  stbi_image_free(px);
  return p;
}

// the edits: A = nothing, B = one radial +2 EV (JSON exposure = EV x 20) at (0.2, 0.25), radius 0.12, feather 20
static const char* kA = "{}";
static const char* kB =
    R"({"local":{"v":1,"items":[{"id":"r","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.2,"cy":0.25,"rx":0.12,"ry":0.12,"angle":0,"feather":20}],"adjust":{"exposure":40}}]}})";

struct Probe {
  double inside = 0, outside = 0;  // mean luma difference B - A in the radial's core, mean |difference| of a far block
};
static double blockMean(const Pic& a, const Pic& b, float cu, float cv, int half, bool absolute) {
  const int cx = int(cu * float(a.w)), cy = int(cv * float(a.h));
  double s = 0;
  int n = 0;
  for (int y = std::max(0, cy - half); y < std::min(a.h, cy + half); ++y)
    for (int x = std::max(0, cx - half); x < std::min(a.w, cx + half); ++x) {
      const double d = double(b.luma(x, y)) - double(a.luma(x, y));
      s += absolute ? std::fabs(d) : d, ++n;
    }
  return n ? s / n : 0;
}
static Probe probe(const Pic& a, const Pic& b, int half) {
  Probe p;
  p.inside = blockMean(a, b, 0.2f, 0.25f, half, false);
  p.outside = blockMean(a, b, 0.8f, 0.8f, half, true);
  return p;
}

struct Session {
  DappleSession* s = nullptr;
  explicit Session(const fs::path& f, int longEdge) { s = dapple_session_open(f.string().c_str(), DAPPLE_QUALITY_PREVIEW, longEdge); }
  ~Session() {
    if (s) dapple_session_close(s);
  }
};

static void skipWithoutRaw() {
  if (rafPath().empty()) {
    std::printf("  SKIP no build/rawsamples/Fujifilm/X-T50/DSCF0118.RAF\n");
    std::exit(77);
  }
}

static void testRender() {
  skipWithoutRaw();
  Session se(rafPath(), 1600);
  CHECK(se.s != nullptr);
  if (!se.s) return;
  DappleImage ia{}, ib{};
  CHECK(dapple_session_render(se.s, kA, "", 1200, &ia) == DAPPLE_OK);
  CHECK(dapple_session_render(se.s, kB, "", 1200, &ib) == DAPPLE_OK);
  const Pic a = fromImage(ia), b = fromImage(ib);
  CHECK(a.ok() && b.ok() && a.w == b.w && a.h == b.h);
  if (!a.ok() || !b.ok() || a.w != b.w || a.h != b.h) return;
  const Probe p = probe(a, b, 50);
  std::printf("  render %dx%d: radial core mean luma +%.1f levels, far block mean |diff| %.3f\n", a.w, a.h, p.inside, p.outside);
  CHECK(p.inside >= 15.0);
  CHECK(p.outside <= 1.0);
}

static const char* rcName(int rc) { return rc == DAPPLE_OK ? "OK" : "ERROR"; }

static void testExports() {
  skipWithoutRaw();
  Session se(rafPath(), 1600);
  CHECK(se.s != nullptr);
  if (!se.s) return;
  const fs::path dir = root() / "build" / "masks" / "capi";
  fs::create_directories(dir);
  struct Kind {
    const char* name;
    int rc[2];
    DappleHdrInfo info[2];
    Pic pic[2];
  };
  auto run = [&](const char* name, auto&& fn) {
    Kind k{};
    k.name = name;
    for (int i = 0; i < 2; ++i) {
      const char* pj = i ? kB : kA;
      const fs::path f = dir / (std::string(name) + (i ? "_B.jpg" : "_A.jpg"));
      std::error_code ec;
      fs::remove(f, ec);
      k.rc[i] = fn(pj, f.string().c_str(), k.info[i]);
      if (k.rc[i] != DAPPLE_OK) std::printf("  %s: %s\n", name, dapple_last_error());
      else k.pic[i] = decodeFile(f);
    }
    const bool ok = k.rc[0] == DAPPLE_OK && k.rc[1] == DAPPLE_OK && k.pic[0].ok() && k.pic[1].ok() && k.pic[0].w == k.pic[1].w && k.pic[0].h == k.pic[1].h;
    CHECK(ok);
    if (!ok) return;
    const Probe p = probe(k.pic[0], k.pic[1], 50);
    std::printf("  %-14s rc %s/%s  %dx%d  core +%.1f levels, far block |diff| %.3f  (gain map written %d/%d, reason %d/%d)\n", name, rcName(k.rc[0]), rcName(k.rc[1]),
                k.pic[0].w, k.pic[0].h, p.inside, p.outside, k.info[0].written, k.info[1].written, k.info[0].reason, k.info[1].reason);
    CHECK(p.inside >= 15.0);
    CHECK(p.outside <= 1.0);
  };
  run("export_jpeg", [&](const char* pj, const char* path, DappleHdrInfo&) { return dapple_session_export_jpeg(se.s, pj, "", 1200, 92, path); });
  run("export_jpeg_ex", [&](const char* pj, const char* path, DappleHdrInfo&) {
    return dapple_session_export_jpeg_ex(se.s, pj, "", 1200, 92, path, "{\"metaSource\":\"\",\"colorSpace\":\"srgb\"}");
  });
  // wide gamut: the header says "srgb" only; report what the call returns
  {
    DappleHdrInfo none{};
    const fs::path f = dir / "p3_probe.jpg";
    const int rc = dapple_session_export_jpeg_ex(se.s, kB, "", 1200, 92, f.string().c_str(), "{\"colorSpace\":\"p3\"}");
    std::printf("  export_jpeg_ex colorSpace p3: rc %s%s%s\n", rcName(rc), rc ? " - " : "", rc ? dapple_last_error() : "");
    if (rc == DAPPLE_OK)
      run("export_ex_p3", [&](const char* pj, const char* path, DappleHdrInfo&) {
        return dapple_session_export_jpeg_ex(se.s, pj, "", 1200, 92, path, "{\"colorSpace\":\"p3\"}");
      });
    (void)none;
  }
  run("export_hdr", [&](const char* pj, const char* path, DappleHdrInfo& info) {
    return dapple_session_export_jpeg_hdr(se.s, pj, "", 1200, 92, path, 0.f, &info);
  });
  // batch previews: the standard rendering and a modern one, 256 px (interactive tier, GPU or CPU frames)
  {
    const char* ids[2] = {"", "modern_clean"};
    DappleImage oa[2]{}, ob[2]{};
    CHECK(dapple_session_base_previews(se.s, kA, ids, 2, 256, oa) == DAPPLE_OK);
    CHECK(dapple_session_base_previews(se.s, kB, ids, 2, 256, ob) == DAPPLE_OK);
    for (int i = 0; i < 2; ++i) {
      const Pic a = fromImage(oa[i]), b = fromImage(ob[i]);
      CHECK(a.ok() && b.ok() && a.w == b.w && a.h == b.h);
      if (!a.ok() || !b.ok() || a.w != b.w || a.h != b.h) continue;
      const Probe p = probe(a, b, 12);
      std::printf("  base_previews %-12s %dx%d  core +%.1f levels, far block |diff| %.3f\n", ids[i][0] ? ids[i] : "(standard)", a.w, a.h, p.inside, p.outside);
      CHECK(p.inside >= 8.0);
      CHECK(p.outside <= 2.0);
    }
  }
}

static void testUi() {
  skipWithoutRaw();
  Session se(rafPath(), 1600);
  CHECK(se.s != nullptr);
  if (!se.s) return;
  for (int flags = 0; flags < 2; ++flags) {
    DappleImage im{};
    CHECK(dapple_session_mask_preview(se.s, kB, 0, 512, flags, &im) == DAPPLE_OK);
    const Pic m = fromImage(im);
    CHECK(m.ok() && std::max(m.w, m.h) == 512);
    if (!m.ok()) continue;
    const int cx = int(0.2f * float(m.w)), cy = int(0.25f * float(m.h)), ox = int(0.8f * float(m.w)), oy = int(0.8f * float(m.h));
    const int centre = m.d[(std::size_t(cy) * m.w + cx) * 4], far = m.d[(std::size_t(oy) * m.w + ox) * 4];
    std::printf("  mask_preview flags %d: %dx%d, centre %d, far %d, r=g=b %s, alpha %d\n", flags, m.w, m.h, centre, far,
                m.d[(std::size_t(cy) * m.w + cx) * 4 + 1] == centre && m.d[(std::size_t(cy) * m.w + cx) * 4 + 2] == centre ? "yes" : "no",
                m.d[(std::size_t(cy) * m.w + cx) * 4 + 3]);
    CHECK(centre >= 250);
    CHECK(far == 0);
    CHECK(m.d[(std::size_t(cy) * m.w + cx) * 4 + 3] == 255);
  }
  {
    DappleImage im{};
    CHECK(dapple_session_mask_preview(se.s, kB, 5, 512, 0, &im) == DAPPLE_ERROR);  // no such adjustment
    dapple_free_image(&im);
  }
  // point mapping
  {
    const char* geo = R"({"crop":"4:5","rot":90,"straighten":5})";
    const float pts[][2] = {{100.f, 200.f}, {333.3f, 444.4f}, {50.f, 700.f}, {0.f, 0.f}, {-30.f, 20.f}};
    double worst = 0;
    for (const auto& pt : pts) {
      float u = 0, v = 0, x = 0, y = 0;
      CHECK(dapple_session_map_point(se.s, geo, 800, pt[0], pt[1], 0, &u, &v) == DAPPLE_OK);
      CHECK(dapple_session_map_point(se.s, geo, 800, u, v, 1, &x, &y) == DAPPLE_OK);
      worst = std::max(worst, double(std::hypot(x - pt[0], y - pt[1])));
    }
    std::printf("  map_point crop 4:5 rot 90 straighten 5: frame -> photo -> frame worst error %.5f px\n", worst);
    CHECK(worst < 0.01);
    float u = 0, v = 0;
    CHECK(dapple_session_map_point(se.s, kA, 1000, 500.f, 333.5f, 0, &u, &v) == DAPPLE_OK);
    std::printf("  map_point uncropped frame centre -> photo (%.5f, %.5f)\n", u, v);
    CHECK(std::fabs(u - 0.5f) < 1e-3f && std::fabs(v - 0.5f) < 1e-3f);
  }
  // range sampling
  {
    char* s = dapple_session_mask_sample_json(se.s, kA, 0.5f, 0.5f);
    CHECK(s != nullptr);
    if (s) {
      std::printf("  mask_sample_json: %s\n", s);
      float L = -1.f;
      const char* k = std::strstr(s, "\"L\":");
      CHECK(s[0] == '{' && s[std::strlen(s) - 1] == '}' && k != nullptr);
      if (k) L = float(std::atof(k + 4));
      CHECK(L >= 0.f && L <= 100.f && std::strstr(s, "\"hue\":") && std::strstr(s, "\"sat\":"));
      dapple_free_string(s);
    }
  }
  // raster store
  {
    std::vector<std::uint8_t> px(16 * 8, 200);
    CHECK(dapple_mask_raster_has("capi_t") == 0);
    CHECK(dapple_mask_raster_set("capi_t", 16, 8, px.data(), 0) == DAPPLE_OK);
    CHECK(dapple_mask_raster_has("capi_t") == 1);
    std::vector<std::uint8_t> strided(24 * 8, 7);  // 16 px used of a 24 byte stride
    CHECK(dapple_mask_raster_set("capi_t2", 16, 8, strided.data(), 24) == DAPPLE_OK);
    // a raster adjustment through the preview: uniform 200 -> mask about 0.78 everywhere
    const char* rj = R"({"local":{"v":1,"items":[{"id":"x","mask":[{"kind":"raster","ref":"capi_t","refine":0}],"adjust":{"exposure":20}}]}})";
    DappleImage im{};
    CHECK(dapple_session_mask_preview(se.s, rj, 0, 256, 0, &im) == DAPPLE_OK);
    const Pic m = fromImage(im);
    if (m.ok()) {
      const int v = m.d[(std::size_t(m.h / 2) * m.w + m.w / 2) * 4];
      std::printf("  raster set -> preview centre %d (expect 200)\n", v);
      CHECK(std::abs(v - 200) <= 2);
    }
    CHECK(dapple_mask_raster_remove("capi_t") == DAPPLE_OK);
    CHECK(dapple_mask_raster_has("capi_t") == 0);
    CHECK(dapple_mask_raster_remove("capi_t2") == DAPPLE_OK);
    CHECK(dapple_mask_raster_set("capi_bad", 0, 8, px.data(), 0) == DAPPLE_ERROR);
  }
}

// ---- the Motion Photo cover through the video-frame path -------------------------------------------------------------------------------
static std::uint16_t toHalf(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  const std::uint32_t s = (b >> 16) & 0x8000u;
  const int e = int((b >> 23) & 0xff) - 127 + 15;
  const std::uint32_t m = b & 0x7fffffu;
  if (e <= 0) return std::uint16_t(s);
  if (e >= 31) return std::uint16_t(s | 0x7bffu);
  std::uint32_t r = s | (std::uint32_t(e) << 10) | (m >> 13);
  if (m & 0x1000u) ++r;
  return std::uint16_t(r);
}

static void testVideo() {
  const int w = 320, h = 180;
  std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float v = 0.12f + 0.1f * float(x) / w;
      std::uint16_t* p = &px[(std::size_t(y) * w + x) * 4];
      p[0] = p[1] = p[2] = toHalf(v);
      p[3] = 0x3c00;
    }
  // the same edit as above, centred
  const char* pb =
      R"({"local":{"v":1,"items":[{"id":"r","mask":[{"kind":"radial","op":"add","cx":0.5,"cy":0.5,"rx":0.2,"ry":0.2,"angle":0,"feather":20}],"adjust":{"exposure":40}}]}})";
  std::vector<std::uint8_t> out[2];
  int ow = 0, oh = 0;
  for (int i = 0; i < 2; ++i) {
    DappleVideoEdit* e = dapple_video_edit_open(i ? pb : "{}", nullptr, "{\"stillLongEdge\":640}");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(dapple_video_edit_set_reference(e, px.data(), w, h, 0, 0) == DAPPLE_OK);
    int32_t wh[2] = {0, 0};
    CHECK(dapple_video_edit_output_size(e, wh) == DAPPLE_OK);
    ow = wh[0], oh = wh[1];
    const int64_t n = dapple_video_edit_frame_bytes(e, 0);
    out[i].assign(std::size_t(std::max<int64_t>(n, 0)), 0);
    CHECK(dapple_video_edit_render(e, px.data(), w, h, 0, 0, 0, 0, out[i].data(), n) == DAPPLE_OK);
    dapple_video_edit_close(e);
  }
  auto lumaAt = [&](int i, int x, int y) {
    const std::uint8_t* p = &out[i][(std::size_t(y) * ow + x) * 4];
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
  };
  if (out[0].size() != std::size_t(ow) * oh * 4 || out[1].size() != out[0].size()) {
    CHECK(false);
    return;
  }
  const double inside = lumaAt(1, ow / 2, oh / 2) - lumaAt(0, ow / 2, oh / 2);
  const double outside = std::fabs(lumaAt(1, ow / 10, oh / 10) - lumaAt(0, ow / 10, oh / 10));
  std::printf("  video frame path %dx%d (cover = frame_index 0): radial centre +%.1f levels, far corner |diff| %.2f\n", ow, oh, inside, outside);
  CHECK(inside >= 15.0);
  CHECK(outside <= 1.0);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"render", testRender}, {"exports", testExports}, {"ui", testUi}, {"video", testVideo}};
  const std::string id = argc > 1 ? argv[1] : "";
  bool ran = false;
  for (const T& t : tests)
    if (id.empty() || id == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      ran = true;
    }
  if (!ran) {
    std::printf("unknown test id '%s'\n", id.c_str());
    return 2;
  }
  std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
  return g_failures ? 1 : 0;
}
