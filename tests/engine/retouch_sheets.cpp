// Evidence sheets of the portrait-retouch local ops through the C API (not a regression test; docs/engine/pipeline-architecture.md sections 40, 41).
//   dapple_retouch_sheets <outdir> [portrait names...]      (run from the repository root)
// Reads build/ai-eval/portraits/<name>.jpg and build/retouch/masks/<name>/{meta.json, f1_*.png}, writes into <outdir> per portrait:
//   <name>_ops.jpg      rows per op: original | op 50 | op 100 (2048 px renders, 640 px panels)
//   <name>_crops.jpg    200 % cheek crops of the full-resolution render: original | smooth 50 | smooth 100 | defaults
//   <name>_defaults.jpg original | defaults (1024 px wide each)
//   <name>_bands.jpg    |L_smooth100 - L_orig| x10 | |H_smooth100 - H_orig| x10 on the cheek crop; <name>_bands.txt the RMS numbers
//   <name>_legend.txt   what every panel / row is
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"

namespace fs = std::filesystem;
namespace {

struct Pic {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;  // RGB
  bool ok() const { return w > 0 && h > 0 && !d.empty(); }
};

Pic render(DappleSession* s, const std::string& params, int longEdge) {
  DappleImage im{};
  Pic p;
  if (dapple_session_render(s, params.c_str(), "", longEdge, &im) != DAPPLE_OK) {
    std::printf("  render failed: %s\n", dapple_last_error());
    return p;
  }
  p.w = im.width, p.h = im.height;
  p.d.resize(std::size_t(p.w) * p.h * 3);
  for (std::size_t i = 0; i < std::size_t(p.w) * p.h; ++i)
    for (int c = 0; c < 3; ++c) p.d[i * 3 + c] = im.rgba[i * 4 + c];
  dapple_free_image(&im);
  return p;
}

Pic crop(const Pic& a, int x0, int y0, int w, int h) {
  Pic o;
  o.w = w, o.h = h, o.d.resize(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) o.d[(std::size_t(y) * w + x) * 3 + c] = a.d[(std::size_t(std::clamp(y0 + y, 0, a.h - 1)) * a.w + std::clamp(x0 + x, 0, a.w - 1)) * 3 + c];
  return o;
}

Pic nearest2x(const Pic& a) {
  Pic o;
  o.w = a.w * 2, o.h = a.h * 2, o.d.resize(std::size_t(o.w) * o.h * 3);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      for (int c = 0; c < 3; ++c) o.d[(std::size_t(y) * o.w + x) * 3 + c] = a.d[(std::size_t(y / 2) * a.w + x / 2) * 3 + c];
  return o;
}

// area-average resize to width tw (aspect kept)
Pic resizeW(const Pic& a, int tw) {
  if (tw == a.w) return a;
  Pic o;
  o.w = tw, o.h = std::max(1, int(std::lround(double(a.h) * tw / a.w))), o.d.resize(std::size_t(o.w) * o.h * 3);
  const double sx = double(a.w) / o.w, sy = double(a.h) / o.h;
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x) {
      const int xa = int(x * sx), xb = std::max(xa + 1, std::min(a.w, int(std::ceil((x + 1) * sx)))), ya = int(y * sy),
                yb = std::max(ya + 1, std::min(a.h, int(std::ceil((y + 1) * sy))));
      double acc[3] = {0, 0, 0};
      int n = 0;
      for (int yy = ya; yy < yb; ++yy)
        for (int xx = xa; xx < xb; ++xx, ++n)
          for (int c = 0; c < 3; ++c) acc[c] += a.d[(std::size_t(yy) * a.w + xx) * 3 + c];
      for (int c = 0; c < 3; ++c) o.d[(std::size_t(y) * o.w + x) * 3 + c] = std::uint8_t(acc[c] / n + 0.5);
    }
  return o;
}

Pic hcat(const std::vector<Pic>& t) {
  Pic o;
  for (const Pic& p : t) o.w += p.w, o.h = std::max(o.h, p.h);
  o.d.assign(std::size_t(o.w) * o.h * 3, 0);
  int ox = 0;
  for (const Pic& p : t) {
    for (int y = 0; y < p.h; ++y) std::memcpy(&o.d[(std::size_t(y) * o.w + ox) * 3], &p.d[std::size_t(y) * p.w * 3], std::size_t(p.w) * 3);
    ox += p.w;
  }
  return o;
}
Pic vcat(const std::vector<Pic>& t) {
  Pic o;
  for (const Pic& p : t) o.h += p.h, o.w = std::max(o.w, p.w);
  o.d.assign(std::size_t(o.w) * o.h * 3, 0);
  int oy = 0;
  for (const Pic& p : t) {
    for (int y = 0; y < p.h; ++y) std::memcpy(&o.d[(std::size_t(oy + y) * o.w) * 3], &p.d[std::size_t(y) * p.w * 3], std::size_t(p.w) * 3);
    oy += p.h;
  }
  return o;
}

bool save(const Pic& p, const fs::path& f) {
  const bool ok = stbi_write_jpg(f.string().c_str(), p.w, p.h, 3, p.d.data(), 93) != 0;
  std::printf("  wrote %s (%dx%d)%s\n", f.string().c_str(), p.w, p.h, ok ? "" : " FAILED");
  return ok;
}

struct Mask {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;
  bool ok() const { return w > 0; }
  int at(double fx, double fy) const {
    const int x = std::clamp(int(fx * w), 0, w - 1), y = std::clamp(int(fy * h), 0, h - 1);
    return d[std::size_t(y) * w + x];
  }
};
Mask loadMask(const fs::path& f) {
  Mask m;
  int c = 0;
  std::uint8_t* px = stbi_load(f.string().c_str(), &m.w, &m.h, &c, 1);
  if (!px) return Mask{};
  m.d.assign(px, px + std::size_t(m.w) * m.h);
  stbi_image_free(px);
  return m;
}

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

// the first n numbers after "key": in a JSON text, searching from `from`
std::vector<double> numsAfter(const std::string& t, const std::string& key, std::size_t n, std::size_t from = 0) {
  std::vector<double> v;
  std::size_t p = t.find("\"" + key + "\"", from);
  if (p == std::string::npos) return v;
  p = t.find(':', p) + 1;
  while (v.size() < n && p < t.size()) {
    while (p < t.size() && !(std::isdigit(static_cast<unsigned char>(t[p])) || t[p] == '-' || t[p] == '.')) ++p;
    char* e = nullptr;
    v.push_back(std::strtod(t.c_str() + p, &e));
    p = std::size_t(e - t.c_str());
  }
  return v;
}

struct Item {
  std::string mask;  // skin undereye teeth sclera iris lips face
  std::string adjust;
};
using Edit = std::vector<Item>;

struct Ctx {
  std::string name;
  double iod = 0;
  std::vector<std::string> have;
};

std::string params(const Ctx& c, const Edit& e) {
  if (e.empty()) return "{}";
  std::string o = "{\"local\":{\"v\":2,\"items\":[";
  int n = 0;
  for (const Item& it : e) {
    if (n) o += ",";
    o += "{\"id\":\"i" + std::to_string(n++) + "\",\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"rt_" + c.name + "_" + it.mask +
         "\",\"refine\":40}],\"adjust\":{" + it.adjust + "},\"retouch\":{\"face\":" + num(c.iod) + ",\"sens\":25,\"spots\":[]}}";
  }
  return o + "]}}";
}

std::string a1(const char* k, double v) { return std::string("\"") + k + "\":" + num(v); }

Edit opEdit(const std::string& op, double s) {
  if (op == "smooth" || op == "even" || op == "redness" || op == "shine" || op == "heal") return {{"skin", a1(op.c_str(), s)}};
  if (op == "undereye") return {{"undereye", a1("undereye", s)}};
  if (op == "facelight") return {{"face", a1("facelight", s)}};
  if (op == "whiten") return {{"teeth", a1("whiten", s)}, {"sclera", a1("whiten", s)}};
  return {};
}

Edit defaultsEdit() {
  return {{"skin", "\"smooth\":30,\"even\":25,\"redness\":30,\"heal\":50,\"shine\":30"},
          {"undereye", "\"undereye\":40"},
          {"teeth", "\"whiten\":30"},
          {"sclera", "\"whiten\":30"}};
}

// the eye clarity and lips presets: a second edit, written when the cap was 4 raster planes an edit (now 12, section 40.4)
Edit presetsEdit() { return {{"iris", "\"clarity\":25,\"saturation\":10"}, {"lips", "\"saturation\":10"}}; }

// luminance (sRGB weights on 8-bit / 255)
std::vector<float> lum(const Pic& p) {
  std::vector<float> y(std::size_t(p.w) * p.h);
  for (std::size_t i = 0; i < y.size(); ++i) y[i] = (0.2126f * p.d[i * 3] + 0.7152f * p.d[i * 3 + 1] + 0.0722f * p.d[i * 3 + 2]) / 255.f;
  return y;
}
// box blur radius r (window 2r+1, clamped edges)
void box(std::vector<float>& a, int w, int h, int r) {
  std::vector<float> t(a.size());
  const double inv = 1.0 / double(2 * r + 1);
  for (int y = 0; y < h; ++y) {
    const float* s = &a[std::size_t(y) * w];
    double acc = 0;
    for (int k = -r; k <= r; ++k) acc += s[std::clamp(k, 0, w - 1)];
    for (int x = 0; x < w; ++x) {
      t[std::size_t(y) * w + x] = float(acc * inv);
      acc += s[std::min(x + r + 1, w - 1)] - s[std::max(x - r, 0)];
    }
  }
  for (int x = 0; x < w; ++x) {
    double acc = 0;
    for (int k = -r; k <= r; ++k) acc += t[std::size_t(std::clamp(k, 0, h - 1)) * w + x];
    for (int y = 0; y < h; ++y) {
      a[std::size_t(y) * w + x] = float(acc * inv);
      acc += t[std::size_t(std::min(y + r + 1, h - 1)) * w + x] - t[std::size_t(std::max(y - r, 0)) * w + x];
    }
  }
}

bool run(const fs::path& outDir, const std::string& name) {
  const fs::path src = fs::path("build/ai-eval/portraits") / (name + ".jpg"), md = fs::path("build/retouch/masks") / name;
  std::ifstream mf(md / "meta.json");
  std::stringstream ss;
  ss << mf.rdbuf();
  const std::string meta = ss.str();
  const std::size_t f1 = meta.find("\"faces\"");
  const auto iodv = numsAfter(meta, "iod", 1, f1), eR = numsAfter(meta, "eyeR", 2, f1), eL = numsAfter(meta, "eyeL", 2, f1);
  if (iodv.size() < 1 || eR.size() < 2 || eL.size() < 2 || !fs::exists(src)) {
    std::printf("%s: meta or source missing\n", name.c_str());
    return false;
  }
  Ctx c;
  c.name = name, c.iod = iodv[0];
  std::printf("== %s iod %.5f\n", name.c_str(), c.iod);
  std::vector<std::string> ids;
  Mask skin;
  for (const char* k : {"skin", "undereye", "teeth", "sclera", "iris", "lips", "face"}) {
    const Mask m = loadMask(md / (std::string("f1_") + k + ".png"));
    if (!m.ok()) {
      std::printf("  mask %s absent\n", k);
      continue;
    }
    const std::string id = "rt_" + name + "_" + k;
    if (dapple_mask_raster_set(id.c_str(), m.w, m.h, m.d.data(), m.w) != DAPPLE_OK) std::printf("  raster_set %s failed: %s\n", id.c_str(), dapple_last_error());
    ids.push_back(id);
    c.have.push_back(k);
    if (!std::strcmp(k, "skin")) skin = m;
  }
  auto has = [&](const std::string& k) { return std::find(c.have.begin(), c.have.end(), k) != c.have.end(); };
  auto filter = [&](const Edit& e) {
    Edit o;
    for (const Item& i : e)
      if (has(i.mask)) o.push_back(i);
    return o;
  };

  DappleSession* s = dapple_session_open(src.string().c_str(), DAPPLE_QUALITY_FULL, 0);
  if (!s) {
    std::printf("  open failed (JPEG not openable?): %s\n", dapple_last_error());
    return false;
  }
  std::ofstream leg(outDir / (name + "_legend.txt"));
  const char* ops[] = {"smooth", "even", "redness", "undereye", "shine", "facelight", "heal", "whiten"};

  // ---- ops sheet and defaults sheet at 2048 ----
  const int E = 2048, PW = 640;
  const Pic orig = render(s, "{}", E);
  if (!orig.ok()) return false;
  std::vector<Pic> rows, frows;
  // face window of the 2048 render: square of 4 iod centred 0.6 iod below the eye midpoint, shown at 512 px
  const int fwin = std::clamp(int(4 * c.iod * orig.w), 64, std::min(orig.w, orig.h));
  const int fx0 = std::clamp(int((eR[0] + eL[0]) / 2 * orig.w) - fwin / 2, 0, orig.w - fwin),
            fy0 = std::clamp(int((eR[1] + eL[1]) / 2 * orig.h + 0.6 * c.iod * orig.w) - fwin / 2, 0, orig.h - fwin);
  auto fcrop = [&](const Pic& p) { return resizeW(crop(p, fx0, fy0, fwin, fwin), 512); };
  leg << name << "_ops.jpg rows (top to bottom), columns original | 50 | 100:\n";
  for (const char* op : ops) {
    const Pic p50 = render(s, params(c, filter(opEdit(op, 50))), E), p100 = render(s, params(c, filter(opEdit(op, 100))), E);
    if (!p50.ok() || !p100.ok()) continue;
    rows.push_back(hcat({resizeW(orig, PW), resizeW(p50, PW), resizeW(p100, PW)}));
    frows.push_back(hcat({fcrop(orig), fcrop(p50), fcrop(p100)}));
    leg << "  " << op << "\n";
  }
  save(vcat(rows), outDir / (name + "_ops.jpg"));
  save(vcat(frows), outDir / (name + "_opsface.jpg"));
  leg << name << "_opsface.jpg: same rows and columns as _ops, face window (" << fwin << " px of the 2048 render at " << fx0 << "," << fy0 << ") at 512 px\n";
  const Pic def = render(s, params(c, filter(defaultsEdit())), E);
  const Pic pre = render(s, params(c, filter(presetsEdit())), E);
  if (def.ok() && pre.ok()) save(hcat({resizeW(orig, 1024), resizeW(def, 1024), resizeW(pre, 1024)}), outDir / (name + "_defaults.jpg"));
  leg << name
      << "_defaults.jpg: original | defaults (smooth 30 even 25 redness 30 heal 50 shine 30 on skin; undereye 40; whiten 30 teeth + sclera; iris clarity 25 sat 10; "
         "lips sat 10)\n";

  // ---- full-resolution cheek crops ----
  int sw = 0, sh = 0, sc = 0;
  stbi_info(src.string().c_str(), &sw, &sh, &sc);
  const int longEdge = std::max(sw, sh);
  const Pic fo = render(s, "{}", longEdge);
  const Pic fs50 = render(s, params(c, filter(opEdit("smooth", 50))), longEdge), fs100 = render(s, params(c, filter(opEdit("smooth", 100))), longEdge),
            fdef = render(s, params(c, filter(defaultsEdit())), longEdge);
  if (!fo.ok() || !fs50.ok() || !fs100.ok() || !fdef.ok()) return false;
  const double iodPx = c.iod * longEdge;
  double vx = (eL[0] - eR[0]) * fo.w, vy = (eL[1] - eR[1]) * fo.h;
  const double vl = std::hypot(vx, vy);
  vx /= vl, vy /= vl;
  const double px = -vy, py = vx;  // perpendicular, toward the mouth
  double cx = 0, cy = 0, used = 0;
  bool found = false;
  for (double k : {0.45, 0.40, 0.50, 0.35, 0.55, 0.30, 0.60, 0.25, 0.65}) {
    cx = eR[0] * fo.w + k * iodPx * px, cy = eR[1] * fo.h + k * iodPx * py;
    bool all = skin.ok();
    for (int dy = -128; dy <= 128 && all; dy += 32)
      for (int dx = -128; dx <= 128; dx += 32)
        if (skin.at((cx + dx) / fo.w, (cy + dy) / fo.h) < 200) {
          all = false;
          break;
        }
    if (all) {
      found = true, used = k;
      break;
    }
  }
  if (!found) {
    used = 0.45;
    cx = eR[0] * fo.w + used * iodPx * px, cy = eR[1] * fo.h + used * iodPx * py;
    std::printf("  WARNING: no cheek position with the whole 256 px crop on skin; using 0.45 iod\n");
  }
  const int x0 = std::clamp(int(cx) - 128, 0, fo.w - 256), y0 = std::clamp(int(cy) - 128, 0, fo.h - 256);
  std::printf("  full %dx%d, cheek crop at (%d,%d) 256x256, offset %.2f iod, wholly on skin: %s\n", fo.w, fo.h, x0, y0, used, found ? "yes" : "NO");
  const Pic co = crop(fo, x0, y0, 256, 256), c50 = crop(fs50, x0, y0, 256, 256), c100 = crop(fs100, x0, y0, 256, 256), cd = crop(fdef, x0, y0, 256, 256);
  save(hcat({nearest2x(co), nearest2x(c50), nearest2x(c100), nearest2x(cd)}), outDir / (name + "_crops.jpg"));
  leg << name << "_crops.jpg: 200 % cheek crop (256 px at " << fo.w << "x" << fo.h << " full resolution, origin " << x0 << "," << y0 << ", " << used
      << " iod below the eye): original | smooth 50 | smooth 100 | defaults\n";

  // ---- bands ----
  const int r = std::max(1, int(std::lround(0.010 * c.iod * longEdge)));
  const std::vector<float> Yo = lum(fo), Ys = lum(fs100);
  std::vector<float> Lo = Yo, Ls = Ys;
  box(Lo, fo.w, fo.h, r), box(Lo, fo.w, fo.h, r), box(Ls, fo.w, fo.h, r), box(Ls, fo.w, fo.h, r);
  Pic bl, bh;
  bl.w = bh.w = bl.h = bh.h = 256;
  bl.d.resize(256 * 256 * 3), bh.d.resize(256 * 256 * 3);
  double sl = 0, shh = 0, sho = 0;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const std::size_t i = std::size_t(y0 + y) * fo.w + std::size_t(x0 + x);
      const float dl = Ls[i] - Lo[i], ho = Yo[i] - Lo[i], hs = Ys[i] - Ls[i], dh = hs - ho;
      sl += double(dl) * dl, shh += double(dh) * dh, sho += double(ho) * ho;
      const std::uint8_t a = std::uint8_t(std::clamp(std::fabs(dl) * 10.f * 255.f, 0.f, 255.f)), b = std::uint8_t(std::clamp(std::fabs(dh) * 10.f * 255.f, 0.f, 255.f));
      for (int k = 0; k < 3; ++k) bl.d[(std::size_t(y) * 256 + x) * 3 + k] = a, bh.d[(std::size_t(y) * 256 + x) * 3 + k] = b;
    }
  const double n = 256.0 * 256.0, rl = std::sqrt(sl / n), rh = std::sqrt(shh / n), rho = std::sqrt(sho / n);
  save(hcat({nearest2x(bl), nearest2x(bh)}), outDir / (name + "_bands.jpg"));
  {
    std::ofstream bt(outDir / (name + "_bands.txt"));
    char b[800];
    std::snprintf(b, sizeof b,
                  "%s smooth 100, cheek crop 256x256 at (%d,%d) of %dx%d, box radius r=%d (0.010 * iod %.5f * %d px), Y = sRGB weights on 8-bit/255\n"
                  "RMS |L_smooth - L_orig| = %.6f (%.3f of 255)\nRMS |H_smooth - H_orig| = %.6f (%.3f of 255)\nRMS H_orig = %.6f (%.3f of 255)\n"
                  "ratio RMS dH / RMS H_orig = %.3f, RMS dL / RMS H_orig = %.3f\nsheet: dL x10 | dH x10 (same grey scale), 2x nearest\n",
                  name.c_str(), x0, y0, fo.w, fo.h, r, c.iod, longEdge, rl, rl * 255, rh, rh * 255, rho, rho * 255, rho > 0 ? rh / rho : 0.0, rho > 0 ? rl / rho : 0.0);
    bt << b;
    std::printf("%s", b);
  }
  dapple_session_close(s);
  for (const std::string& id : ids) dapple_mask_raster_remove(id.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: dapple_retouch_sheets <outdir> [portrait names...] (from the repository root)\n");
    return 2;
  }
  const fs::path out = argv[1];
  fs::create_directories(out);
  std::vector<std::string> names;
  for (int i = 2; i < argc; ++i) names.push_back(argv[i]);
  if (names.empty()) names = {"p1_glasses_flag", "p2_dark_skin_dreads", "p3_curly_glasses_outdoor", "p5_headscarf", "p6_asian_woman_grey"};
  int bad = 0;
  for (const std::string& n : names) bad += run(out, n) ? 0 : 1;
  return bad ? 1 : 0;
}
