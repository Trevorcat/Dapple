// Evidence sheets of local adjustments through the C API (not a regression test; docs/engine/pipeline-architecture.md section 40).
//   dapple_local_sheets <raw> <mask.png> <outdir> <tag> [cx cy r]
// Writes <outdir>/<tag>_<case>.jpg, cases gradient radial luminance brush raster raster_edges. The first five are three panels side by side: the original,
// the mask as a red overlay (dapple_session_mask_preview at the same size) and the result. raster_edges: three 300 x 300 crops at 100 % of a 4000 px render
// along the boundary of the mask (photo | overlay refine 0 | overlay refine 60).
// Exit 77 unless the environment variable DAPPLE_LOCAL_SHEETS is set (the ctest entries local_sheets.* run it that way).
#include <stb_image.h>
#include <stb_image_write.h>

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

namespace {

int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

struct Pic {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;  // RGBA
  bool ok() const { return w > 0 && h > 0 && !d.empty(); }
};

Pic take(DappleImage& im) {
  Pic p;
  p.w = im.width, p.h = im.height;
  if (im.rgba) p.d.assign(im.rgba, im.rgba + std::size_t(im.width) * im.height * 4);
  dapple_free_image(&im);
  return p;
}

Pic render(DappleSession* s, const std::string& params, int longEdge) {
  DappleImage im{};
  if (dapple_session_render(s, params.c_str(), "", longEdge, &im) != DAPPLE_OK) {
    std::printf("  render failed: %s\n", dapple_last_error());
    return Pic{};
  }
  return take(im);
}

Pic maskOf(DappleSession* s, const std::string& params, int longEdge) {
  DappleImage im{};
  if (dapple_session_mask_preview(s, params.c_str(), 0, longEdge, 0, &im) != DAPPLE_OK) {
    std::printf("  mask preview failed: %s\n", dapple_last_error());
    return Pic{};
  }
  return take(im);
}

// out = mix(px, red, 0.5 * mask)
std::vector<std::uint8_t> overlay(const Pic& photo, const Pic& mask, int x0 = 0, int y0 = 0, int w = -1, int h = -1) {
  if (w < 0) w = photo.w;
  if (h < 0) h = photo.h;
  std::vector<std::uint8_t> out(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const std::size_t i = (std::size_t(y0 + y) * photo.w + (x0 + x)) * 4;
      const float m = 0.5f * (mask.d[i] / 255.f);
      const float red[3] = {255.f, 0.f, 0.f};
      for (int c = 0; c < 3; ++c) out[(std::size_t(y) * w + x) * 3 + c] = std::uint8_t(std::clamp(photo.d[i + c] + (red[c] - photo.d[i + c]) * m + 0.5f, 0.f, 255.f));
    }
  return out;
}

std::vector<std::uint8_t> rgbOf(const Pic& p, int x0 = 0, int y0 = 0, int w = -1, int h = -1) {
  if (w < 0) w = p.w;
  if (h < 0) h = p.h;
  std::vector<std::uint8_t> out(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const std::size_t i = (std::size_t(y0 + y) * p.w + (x0 + x)) * 4;
      for (int c = 0; c < 3; ++c) out[(std::size_t(y) * w + x) * 3 + c] = p.d[i + c];
    }
  return out;
}

// paste an RGB tile into an RGB canvas
void paste(std::vector<std::uint8_t>& canvas, int cw, const std::vector<std::uint8_t>& tile, int tw, int th, int ox, int oy) {
  for (int y = 0; y < th; ++y) std::memcpy(&canvas[(std::size_t(oy + y) * cw + ox) * 3], &tile[std::size_t(y) * tw * 3], std::size_t(tw) * 3);
}

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.5g", v);
  return b;
}

// params JSON of one adjustment: mask components (already JSON) + the adjust object body
std::string edit(const std::string& comps, const std::string& adjust, const std::string& extra = "") {
  return "{" + extra + "\"local\":{\"v\":1,\"items\":[{\"id\":\"a\",\"strength\":100,\"mask\":[" + comps + "],\"adjust\":{" + adjust + "}}]}}";
}

bool sheet(DappleSession* s, const fs::path& out, const std::string& params, int longEdge) {
  const Pic a = render(s, "{}", longEdge), r = render(s, params, longEdge), m = maskOf(s, params, longEdge);
  if (!a.ok() || !r.ok() || !m.ok() || a.w != r.w || a.h != r.h || m.w != a.w || m.h != a.h) {
    std::printf("  %s: sizes %dx%d %dx%d mask %dx%d do not pair\n", out.string().c_str(), a.w, a.h, r.w, r.h, m.w, m.h);
    ++g_failures;
    return false;
  }
  std::vector<std::uint8_t> canvas(std::size_t(3 * a.w) * a.h * 3);
  paste(canvas, 3 * a.w, rgbOf(a), a.w, a.h, 0, 0);
  paste(canvas, 3 * a.w, overlay(a, m), a.w, a.h, a.w, 0);
  paste(canvas, 3 * a.w, rgbOf(r), a.w, a.h, 2 * a.w, 0);
  const bool ok = stbi_write_jpg(out.string().c_str(), 3 * a.w, a.h, 3, canvas.data(), 92) != 0;
  std::printf("  wrote %s (%dx%d)%s\n", out.string().c_str(), 3 * a.w, a.h, ok ? "" : " FAILED");
  CHECK(ok);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!std::getenv("DAPPLE_LOCAL_SHEETS")) {
    std::printf("SKIP: set DAPPLE_LOCAL_SHEETS=1\n");
    return 77;
  }
  if (argc < 5) {
    std::printf("usage: dapple_local_sheets <raw> <mask.png> <outdir> <tag> [cx cy r]\n");
    return 2;
  }
  const fs::path raw = argv[1], maskPng = argv[2], outDir = argv[3];
  const std::string tag = argv[4];
  const double cx = argc > 7 ? std::atof(argv[5]) : 0.5, cy = argc > 7 ? std::atof(argv[6]) : 0.55, cr = argc > 7 ? std::atof(argv[7]) : 0.18;
  if (!fs::exists(raw) || !fs::exists(maskPng)) {
    std::printf("SKIP: %s or %s absent\n", raw.string().c_str(), maskPng.string().c_str());
    return 77;
  }
  fs::create_directories(outDir);
  DappleSession* s = dapple_session_open(raw.string().c_str(), DAPPLE_QUALITY_PREVIEW, 1600);
  if (!s) {
    std::printf("open failed: %s\n", dapple_last_error());
    return 1;
  }
  const int P = 1200;  // panel long edge
  auto path = [&](const char* c) { return outDir / (tag + "_" + c + ".jpg"); };

  // ---- orientation / aspect of the decoded RAW against the mask PNG ----
  int mw = 0, mh = 0, mc = 0;
  std::uint8_t* mpx = stbi_load(maskPng.string().c_str(), &mw, &mh, &mc, 1);
  if (!mpx) {
    std::printf("cannot read %s\n", maskPng.string().c_str());
    return 1;
  }
  {
    const Pic a = render(s, "{}", P);
    DappleImage pv{};
    int pw = 0, ph = 0;
    if (dapple_extract_preview(raw.string().c_str(), 0, &pv) == DAPPLE_OK) pw = pv.width, ph = pv.height;
    dapple_free_image(&pv);
    const double ra = double(a.w) / a.h, rm = double(mw) / mh;
    std::printf("  size check: decoded RAW render %dx%d (aspect %.4f), embedded JPEG preview %dx%d, mask PNG %dx%d (aspect %.4f) -> aspect %s%s\n", a.w, a.h, ra, pw,
                ph, mw, mh, rm, std::fabs(ra / rm - 1.0) < 0.01 ? "matches" : "DIFFERS",
                (a.w > a.h) == (mw > mh) ? ", same orientation" : ", ORIENTATION DIFFERS (portrait / landscape)");
  }

  // ---- the raster: area-downsample the PNG to 256 px long edge ----
  const int scale = std::max(mw, mh) > 256 ? 256 : std::max(mw, mh);
  const int rw = std::max(1, int(std::lround(double(mw) * scale / std::max(mw, mh)))), rh = std::max(1, int(std::lround(double(mh) * scale / std::max(mw, mh))));
  std::vector<std::uint8_t> low(std::size_t(rw) * rh);
  for (int y = 0; y < rh; ++y)
    for (int x = 0; x < rw; ++x) {
      const int xa = x * mw / rw, xb = std::max(xa + 1, (x + 1) * mw / rw), ya = y * mh / rh, yb = std::max(ya + 1, (y + 1) * mh / rh);
      double sum = 0;
      int n = 0;
      for (int yy = ya; yy < std::min(yb, mh); ++yy)
        for (int xx = xa; xx < std::min(xb, mw); ++xx) sum += mpx[std::size_t(yy) * mw + xx], ++n;
      low[std::size_t(y) * rw + x] = std::uint8_t(n ? sum / n + 0.5 : 0);
    }
  stbi_image_free(mpx);
  const std::string rid = "sheet_mask_" + tag;
  CHECK(dapple_mask_raster_set(rid.c_str(), rw, rh, low.data(), rw) == DAPPLE_OK);
  std::printf("  raster %s: %dx%d (from %dx%d)\n", rid.c_str(), rw, rh, mw, mh);

  // ---- the five panel sheets ----
  sheet(s, path("gradient"),
        edit(R"({"kind":"linear","op":"add","x0":0.5,"y0":0,"x1":0.5,"y1":0.55})", R"("exposure":-24,"highlights":-30,"dehaze":20)"), P);
  sheet(s, path("radial"),
        edit("{\"kind\":\"radial\",\"op\":\"add\",\"cx\":" + num(cx) + ",\"cy\":" + num(cy) + ",\"rx\":" + num(cr) + ",\"ry\":" + num(cr) + ",\"angle\":0,\"feather\":50}",
             R"("exposure":16,"shadows":30,"clarity":20)"),
        P);
  sheet(s, path("luminance"), edit(R"({"kind":"luminance","op":"add","lo":65,"hi":100,"feather":10})", R"("exposure":-16,"saturation":20)"), P);
  {
    std::string pts;
    for (int i = 0; i < 12; ++i) pts += (i ? "," : "") + num(0.1 + 0.8 * i / 11.0) + "," + num(0.83 + 0.03 * std::sin(double(i)));
    sheet(s, path("brush"),
          edit("{\"kind\":\"brush\",\"op\":\"add\",\"strokes\":[{\"size\":0.04,\"hardness\":50,\"flow\":100,\"erase\":false,\"pts\":[" + pts + "]}]}",
               R"("exposure":20,"saturation":30)"),
          P);
  }
  const std::string rasterEdit = [&](int refine) {
    return edit("{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"" + rid + "\",\"refine\":" + std::to_string(refine) + "}", R"("exposure":14,"clarity":20)");
  }(60);
  sheet(s, path("raster"), rasterEdit, P);
  dapple_session_close(s);

  // ---- raster_edges: 100 % crops along the mask boundary at 4000 px ----
  DappleSession* big = dapple_session_open(raw.string().c_str(), DAPPLE_QUALITY_PREVIEW, 4000);
  if (!big) {
    std::printf("open (4000) failed: %s\n", dapple_last_error());
    return 1;
  }
  const std::string e0 = [&](int refine) {
    return edit("{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"" + rid + "\",\"refine\":" + std::to_string(refine) + "}", R"("exposure":14)");
  }(0);
  const Pic ph = render(big, "{}", 4000);
  const Pic m0 = maskOf(big, e0, 4000), m60 = maskOf(big, rasterEdit, 4000);
  if (!ph.ok() || !m0.ok() || !m60.ok() || m0.w != ph.w || m60.w != ph.w || m0.h != ph.h || m60.h != ph.h) {
    std::printf("  raster_edges: sizes %dx%d / %dx%d / %dx%d do not pair\n", ph.w, ph.h, m0.w, m0.h, m60.w, m60.h);
    return 1;
  }
  std::printf("  raster_edges render %dx%d (the 4000 px request is limited by the half-size decode when smaller)\n", ph.w, ph.h);
  // boundary cells of the 256 px mask (value >= 128 with a 4-neighbour < 128), at a cell whose 300 px crop fits
  struct Pt {
    int x, y;
  };
  std::vector<Pt> bnd;
  auto lowAt = [&](int x, int y) { return low[std::size_t(std::clamp(y, 0, rh - 1)) * rw + std::clamp(x, 0, rw - 1)]; };
  for (int y = 1; y < rh - 1; ++y)
    for (int x = 1; x < rw - 1; ++x) {
      if (lowAt(x, y) < 128) continue;
      if (lowAt(x - 1, y) >= 128 && lowAt(x + 1, y) >= 128 && lowAt(x, y - 1) >= 128 && lowAt(x, y + 1) >= 128) continue;
      const int fx = int((x + 0.5) / rw * ph.w), fy = int((y + 0.5) / rh * ph.h);
      if (fx < 150 || fy < 150 || fx > ph.w - 150 || fy > ph.h - 150) continue;
      bnd.push_back({fx, fy});
    }
  std::printf("  mask boundary cells usable: %zu\n", bnd.size());
  CHECK(bnd.size() >= 3);
  if (bnd.size() < 3) return 1;
  // three points spread out: the boundary cell nearest the centroid, then farthest-point sampling
  double mx = 0, my = 0;
  for (const Pt& p : bnd) mx += p.x, my += p.y;
  mx /= double(bnd.size()), my /= double(bnd.size());
  std::vector<Pt> pick;
  {
    Pt best = bnd[0];
    double bd = 1e18;
    for (const Pt& p : bnd) {
      const double d = std::hypot(p.x - mx, p.y - my);
      if (d < bd) bd = d, best = p;
    }
    pick.push_back(best);
  }
  while (pick.size() < 3) {
    Pt best = bnd[0];
    double bd = -1;
    for (const Pt& p : bnd) {
      double dm = 1e18;
      for (const Pt& q : pick) dm = std::min(dm, std::hypot(double(p.x - q.x), double(p.y - q.y)));
      if (dm > bd) bd = dm, best = p;
    }
    pick.push_back(best);
  }
  std::vector<std::uint8_t> canvas(std::size_t(900) * 900 * 3);
  for (int r = 0; r < 3; ++r) {
    const int x0 = pick[std::size_t(r)].x - 150, y0 = pick[std::size_t(r)].y - 150;
    paste(canvas, 900, rgbOf(ph, x0, y0, 300, 300), 300, 300, 0, r * 300);
    paste(canvas, 900, overlay(ph, m0, x0, y0, 300, 300), 300, 300, 300, r * 300);
    paste(canvas, 900, overlay(ph, m60, x0, y0, 300, 300), 300, 300, 600, r * 300);
    std::printf("  crop %d at (%d, %d) of %dx%d\n", r, pick[std::size_t(r)].x, pick[std::size_t(r)].y, ph.w, ph.h);
  }
  const fs::path o = path("raster_edges");
  const bool ok = stbi_write_jpg(o.string().c_str(), 900, 900, 3, canvas.data(), 92) != 0;
  std::printf("  wrote %s (900x900)%s\n", o.string().c_str(), ok ? "" : " FAILED");
  CHECK(ok);
  dapple_session_close(big);
  dapple_mask_raster_remove(rid.c_str());
  std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
  return g_failures ? 1 : 0;
}
