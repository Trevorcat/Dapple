// Evidence sheets of the portrait reshaping (docs/engine/pipeline-architecture.md section 45) through the C API. Not a regression test.
//   dapple_reshape_sheets <outdir> [names...]        (run from the repository root)
// Reads build/reshape/in/<name>/{photo.png, person.png, meta.json} (scripts/reshape/landmarks.py), writes into <outdir>:
//   <name>_face.jpg   rows per face operator (slim, small, chin +, chin -, eyes, nose, all at 50): original | 50 | 100 | |B| at 100 (face crop)
//   <name>_body.jpg   rows per body operator (waist, arms, legs, long legs, head, all at 50): original | 50 | 100 | |B| at 100 (whole photo)
//   <name>_push.jpg   a push stroke and a restore over half of it: original | push | push + restore
//   dapple_reshape_sheets <outdir> --bg [names...]   (section 48) per body operator at 100: the earlier single field next to the layered one; bg.txt has the share of
//                     changed background pixels and the shift of the drawn lines; names with 'shadow' also get the detected shadow variants
//   lines.txt         background bending per photo and operator: max |B| on background pixels (person plane < 0.5) within the protection band,
//                     beyond it, and the share of background pixels that move more than 0.5 px (at the 2048 px render)
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "json_value.h"
#include "warp_field.h"

namespace fs = std::filesystem;
namespace {

struct Pic {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;
  bool ok() const { return w > 0; }
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
      for (int c = 0; c < 3; ++c)
        o.d[(std::size_t(y) * w + x) * 3 + c] = a.d[(std::size_t(std::clamp(y0 + y, 0, a.h - 1)) * a.w + std::clamp(x0 + x, 0, a.w - 1)) * 3 + c];
  return o;
}
Pic resizeW(const Pic& a, int tw) {
  if (tw == a.w) return a;
  Pic o;
  o.w = tw, o.h = std::max(1, int(std::lround(double(a.h) * tw / a.w))), o.d.resize(std::size_t(o.w) * o.h * 3);
  const double sx = double(a.w) / o.w, sy = double(a.h) / o.h;
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x) {
      if (sx < 1) {  // enlarge: nearest
        for (int c = 0; c < 3; ++c) o.d[(std::size_t(y) * o.w + x) * 3 + c] = a.d[(std::size_t(std::min(a.h - 1, int(y * sy))) * a.w + std::min(a.w - 1, int(x * sx))) * 3 + c];
        continue;
      }
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
Pic hcat(const std::vector<Pic>& t, int gap = 6) {
  Pic o;
  for (const Pic& p : t) o.w += p.w + gap, o.h = std::max(o.h, p.h);
  o.d.assign(std::size_t(o.w) * o.h * 3, 255);
  int ox = 0;
  for (const Pic& p : t) {
    for (int y = 0; y < p.h; ++y) std::memcpy(&o.d[(std::size_t(y) * o.w + ox) * 3], &p.d[std::size_t(y) * p.w * 3], std::size_t(p.w) * 3);
    ox += p.w + gap;
  }
  return o;
}
Pic vcat(const std::vector<Pic>& t, int gap = 6) {
  Pic o;
  for (const Pic& p : t) o.h += p.h + gap, o.w = std::max(o.w, p.w);
  o.d.assign(std::size_t(o.w) * o.h * 3, 255);
  int oy = 0;
  for (const Pic& p : t) {
    for (int y = 0; y < p.h; ++y) std::memcpy(&o.d[(std::size_t(oy + y) * o.w) * 3], &p.d[std::size_t(y) * p.w * 3], std::size_t(p.w) * 3);
    oy += p.h + gap;
  }
  return o;
}
bool save(const Pic& p, const fs::path& f) {
  const bool ok = stbi_write_jpg(f.string().c_str(), p.w, p.h, 3, p.d.data(), 90) != 0;
  std::printf("  wrote %s (%dx%d)%s\n", f.string().c_str(), p.w, p.h, ok ? "" : " FAILED");
  return ok;
}

struct Plane {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;
};
Plane loadPlane(const fs::path& f) {
  Plane m;
  int c = 0;
  std::uint8_t* px = stbi_load(f.string().c_str(), &m.w, &m.h, &c, 1);
  if (!px) return Plane{};
  m.d.assign(px, px + std::size_t(m.w) * m.h);
  stbi_image_free(px);
  return m;
}

std::string arr(const std::vector<double>& v) {
  std::string o = "[";
  char b[32];
  for (std::size_t i = 0; i < v.size(); ++i) std::snprintf(b, sizeof b, i ? ",%.6g" : "%.6g", v[i]), o += b;
  return o + "]";
}

// |B| heat map of the field over the render (black = 0, red .. yellow = up to `full` px), with the person plane's outline in cyan
Pic heat(const dapple::warp::Field& f, int w, int h, const Plane& person) {
  Pic o;
  o.w = w, o.h = h, o.d.assign(std::size_t(w) * h * 3, 0);
  const double L = std::max(w, h);
  double mx = 0;
  std::vector<double> m(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double dx, dy;
      dapple::warp::sampleUV(f, (x + 0.5) / w, (y + 0.5) / h, dx, dy);
      m[std::size_t(y) * w + x] = std::hypot(dx, dy) * L;
      mx = std::max(mx, m[std::size_t(y) * w + x]);
    }
  auto pm = [&](int x, int y) {
    if (!person.w) return 0;
    return int(person.d[std::size_t(std::clamp(y * person.h / h, 0, person.h - 1)) * person.w + std::clamp(x * person.w / w, 0, person.w - 1)]);
  };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double t = mx > 0 ? std::min(1.0, m[std::size_t(y) * w + x] / mx) : 0.0;
      std::uint8_t* p = &o.d[(std::size_t(y) * w + x) * 3];
      p[0] = std::uint8_t(255 * std::min(1.0, 2 * t)), p[1] = std::uint8_t(255 * std::max(0.0, 2 * t - 1)), p[2] = 0;
      if (m[std::size_t(y) * w + x] > 0 && t < 0.02) p[0] = std::max<std::uint8_t>(p[0], 60);  // the support, faintly
      const bool edge = x > 0 && y > 0 && ((pm(x, y) >= 128) != (pm(x - 1, y) >= 128) || (pm(x, y) >= 128) != (pm(x, y - 1) >= 128));
      if (edge) p[0] = 0, p[1] = 200, p[2] = 255;
    }
  return o;
}

struct BgStats {
  double nearMax = 0, farMax = 0, movedShare = 0;
};
BgStats background(const dapple::warp::Field& f, int w, int h, const Plane& person) {
  BgStats s;
  if (!person.w) return s;
  const double L = std::max(w, h);
  // distance to the person on the plane grid (brute force over a window, plane px)
  const int pw = person.w, ph = person.h;
  std::vector<float> dist(std::size_t(pw) * ph, 1e9f);
  for (int y = 0; y < ph; ++y)
    for (int x = 0; x < pw; ++x)
      if (person.d[std::size_t(y) * pw + x] >= 128) dist[std::size_t(y) * pw + x] = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (int yy = 0; yy < ph; ++yy)
      for (int xx = 0; xx < pw; ++xx) {
        const int y = pass ? ph - 1 - yy : yy, x = pass ? pw - 1 - xx : xx, d = pass ? 1 : -1;
        float& v = dist[std::size_t(y) * pw + x];
        if (x + d >= 0 && x + d < pw) v = std::min(v, dist[std::size_t(y) * pw + x + d] + 1);
        if (y + d >= 0 && y + d < ph) v = std::min(v, dist[std::size_t(y + d) * pw + x] + 1);
        if (x + d >= 0 && x + d < pw && y + d >= 0 && y + d < ph) v = std::min(v, dist[std::size_t(y + d) * pw + x + d] + 1.4142f);
      }
  std::size_t bg = 0, moved = 0;
  const double band = f.delta * std::max(pw, ph);  // plane px
  for (int y = 0; y < h; y += 2)
    for (int x = 0; x < w; x += 2) {
      const int px = std::clamp(x * pw / w, 0, pw - 1), py = std::clamp(y * ph / h, 0, ph - 1);
      if (person.d[std::size_t(py) * pw + px] >= 128) continue;
      double dx, dy;
      dapple::warp::sampleUV(f, (x + 0.5) / w, (y + 0.5) / h, dx, dy);
      const double m = std::hypot(dx, dy) * L;
      ++bg, moved += m > 0.5;
      if (dist[std::size_t(py) * pw + px] <= band + 1.5) s.nearMax = std::max(s.nearMax, m);
      else s.farMax = std::max(s.farMax, m);
    }
  s.movedShare = bg ? double(moved) / bg : 0;
  return s;
}

struct Input {
  std::string name;
  int pw = 0, ph = 0;
  std::vector<std::vector<double>> faces;  // 108 numbers each
  std::vector<double> pose;                // 99 numbers or empty
};
bool loadInput(const fs::path& dir, Input& in) {
  std::ifstream f(dir / "meta.json");
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string t = ss.str();
  dapple::jsonlite::JValue v;
  dapple::jsonlite::JParser p(t);
  if (!p.parse(v)) return false;
  if (const auto* fs = v.get("faces"))
    for (const auto& fc : fs->items) {
      std::vector<double> pts;
      if (const auto* a = fc.get("pts"))
        for (const auto& n : a->items) pts.push_back(n.number);
      if (pts.size() == 108) in.faces.push_back(pts);
    }
  if (const auto* ps = v.get("pose"); ps && ps->kind == dapple::jsonlite::JValue::Object)
    if (const auto* a = ps->get("pts"))
      for (const auto& n : a->items) in.pose.push_back(n.number);
  return true;
}

// the input folder: build/reshape/in, or DAPPLE_RESHAPE_IN (scripts/reshape/landmarks.py writes the same variable's folder)
std::string inRoot() {
  const char* e = std::getenv("DAPPLE_RESHAPE_IN");
  return e && *e ? e : "build/reshape/in";
}

int run(const fs::path& out, const std::string& name, std::ofstream& lines) {
  const fs::path dir = fs::path(inRoot()) / name;
  Input in;
  in.name = name;
  if (!loadInput(dir, in)) {
    std::printf("%s: no meta.json\n", name.c_str());
    return 1;
  }
  const Plane person = loadPlane(dir / "person.png");
  const std::string matteId = "rs:" + name + ":person";
  if (person.w) dapple_mask_raster_set(matteId.c_str(), person.w, person.h, person.d.data(), person.w);
  DappleSession* s = dapple_session_open((dir / "photo.png").string().c_str(), DAPPLE_QUALITY_FULL, 0);
  if (!s) {
    std::printf("%s: open failed: %s\n", name.c_str(), dapple_last_error());
    return 1;
  }
  const int LE = 2048;
  const Pic orig = render(s, "{}", LE);
  if (!orig.ok()) return 1;
  std::printf("%s: %dx%d, %zu face(s), pose %s\n", name.c_str(), orig.w, orig.h, in.faces.size(), in.pose.empty() ? "no" : "yes");
  auto faceJson = [&](const std::vector<double>& pts, const std::string& ops) { return "{\"i\":1," + ops + (ops.empty() ? "" : ",") + "\"pts\":" + arr(pts) + "}"; };
  auto edit = [&](const std::string& faces, const std::string& bodies, const std::string& push) {
    std::string r = "{\"reshape\":{\"v\":2,\"matte\":\"" + matteId + "\"";
    if (!faces.empty()) r += ",\"faces\":[" + faces + "]";
    if (!bodies.empty()) r += ",\"bodies\":[" + bodies + "]";
    if (!push.empty()) r += ",\"push\":[" + push + "]";
    return r + "}}";
  };
  auto fieldOf = [&](const std::string& json) -> std::shared_ptr<const dapple::warp::Field> {
    auto p = dapple::fromJson(json);
    if (!p) return nullptr;
    return dapple::warp::bind(p->reshape, orig.w, orig.h, false);
  };
  auto statsLine = [&](const std::string& what, const std::string& json) {
    auto f = fieldOf(json);
    if (!f) return;
    const BgStats b = background(*f, orig.w, orig.h, person);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%-26s %-14s max|B| %6.2f px  bg near (<= delta %4.1f px) %6.2f px  bg beyond %5.2f px  bg moved > 0.5 px %5.2f %%%s\n", name.c_str(),
                  what.c_str(), f->maxDisp * std::max(orig.w, orig.h), f->delta * std::max(orig.w, orig.h), b.nearMax, b.farMax, 100 * b.movedShare,
                  f->gradLimited ? "  (limited)" : "");
    lines << buf;
    std::printf("%s", buf);
  };

  // ---- faces: every face gets the operator (several people: per face)
  if (!in.faces.empty()) {
    const std::vector<double>& f0 = in.faces[0];
    double x0 = 1, y0 = 1, x1 = 0, y1 = 0;
    for (const auto& fc : in.faces)
      for (int k = 0; k < 36; ++k) x0 = std::min(x0, fc[2 * k]), x1 = std::max(x1, fc[2 * k]), y0 = std::min(y0, fc[2 * k + 1]), y1 = std::max(y1, fc[2 * k + 1]);
    (void)f0;
    const double cx = 0.5 * (x0 + x1) * orig.w, cy = 0.5 * (y0 + y1) * orig.h;
    const double side = std::max((x1 - x0) * orig.w, (y1 - y0) * orig.h) * 1.7;
    const int cs = int(std::min<double>(std::min(orig.w, orig.h), side));
    const int cx0 = std::clamp(int(cx - cs / 2), 0, orig.w - cs), cy0 = std::clamp(int(cy - cs / 2), 0, orig.h - cs);
    const int panel = 420;
    struct Op {
      const char* label;
      std::function<std::string(double)> ops;
    };
    const Op ops[] = {{"slim", [](double s) { return "\"slim\":" + std::to_string(int(s)); }},
                      {"small", [](double s) { return "\"small\":" + std::to_string(int(s)); }},
                      {"chin+", [](double s) { return "\"chin\":" + std::to_string(int(s)); }},
                      {"chin-", [](double s) { return "\"chin\":" + std::to_string(-int(s)); }},
                      {"eyes", [](double s) { return "\"eyes\":" + std::to_string(int(s)); }},
                      {"nose", [](double s) { return "\"nose\":" + std::to_string(int(s)); }},
                      {"all", [](double s) {
                         const int v = int(s / 2);
                         return "\"slim\":" + std::to_string(v) + ",\"small\":" + std::to_string(v) + ",\"chin\":" + std::to_string(v / 2) + ",\"eyes\":" + std::to_string(v) + ",\"nose\":" + std::to_string(v);
                       }}};
    std::vector<Pic> rows;
    const Pic o = resizeW(crop(orig, cx0, cy0, cs, cs), panel);
    for (const Op& op : ops) {
      std::vector<Pic> cols{o};
      std::shared_ptr<const dapple::warp::Field> f100;
      for (double st : {50.0, 100.0}) {
        std::string fl;
        for (std::size_t i = 0; i < in.faces.size(); ++i) fl += (i ? "," : "") + faceJson(in.faces[i], op.ops(st));
        const std::string js = edit(fl, "", "");
        const Pic r = render(s, js, LE);
        if (!r.ok()) continue;
        cols.push_back(resizeW(crop(r, cx0, cy0, cs, cs), panel));
        if (st == 100) {
          f100 = fieldOf(js);
          statsLine(std::string("face ") + op.label, js);
        }
      }
      if (f100) cols.push_back(resizeW(crop(heat(*f100, orig.w, orig.h, person), cx0, cy0, cs, cs), panel));
      rows.push_back(hcat(cols));
    }
    save(vcat(rows), out / (name + "_face.jpg"));
  }

  // ---- body (the main person's pose)
  if (in.pose.size() == 99) {
    const int panel = 480;
    struct Op {
      const char* key;
    };
    const char* keys[] = {"waist", "arms", "legs", "longlegs", "head", "all"};
    std::vector<Pic> rows;
    const Pic o = resizeW(orig, panel);
    for (const char* k : keys) {
      std::vector<Pic> cols{o};
      std::shared_ptr<const dapple::warp::Field> f100;
      for (double st : {50.0, 100.0}) {
        std::string ops;
        if (std::strcmp(k, "all") == 0) {
          const int v = int(st / 2);
          ops = "\"waist\":" + std::to_string(v) + ",\"arms\":" + std::to_string(v) + ",\"legs\":" + std::to_string(v) + ",\"longlegs\":" + std::to_string(v) + ",\"head\":" + std::to_string(v);
        } else {
          ops = std::string("\"") + k + "\":" + std::to_string(int(st));
        }
        const std::string js = edit("", "{" + ops + ",\"pts\":" + arr(in.pose) + "}", "");
        const Pic r = render(s, js, LE);
        if (!r.ok()) continue;
        cols.push_back(resizeW(r, panel));
        if (st == 100) {
          f100 = fieldOf(js);
          statsLine(std::string("body ") + k, js);
        }
      }
      if (f100) cols.push_back(resizeW(heat(*f100, orig.w, orig.h, person), panel));
      rows.push_back(hcat(cols));
    }
    save(vcat(rows), out / (name + "_body.jpg"));
  }

  // ---- push and restore: a drag across the person's side, then restore over the second half
  {
    double ax = 0.5, ay = 0.5;
    if (!in.faces.empty()) ax = in.faces[0][2 * 8], ay = in.faces[0][2 * 8 + 1];  // the cheek (oval 234)
    else if (in.pose.size() == 99) ax = in.pose[3 * 23], ay = in.pose[3 * 23 + 1];  // a hip
    const double r = 0.06;
    char p1[256], p2[256];
    std::snprintf(p1, sizeof p1, "{\"r\":%.4f,\"pts\":[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f]}", r, ax, ay, ax + 0.015, ay, ax + 0.03, ay);
    std::snprintf(p2, sizeof p2, "{\"r\":%.4f,\"restore\":true,\"pts\":[%.5f,%.5f,%.5f,%.5f]}", r * 0.7, ax + 0.03, ay + 0.03, ax + 0.03, ay - 0.06);
    const Pic a = render(s, edit("", "", p1), LE), b = render(s, edit("", "", std::string(p1) + "," + p2), LE);
    const int cs = int(0.35 * std::max(orig.w, orig.h));
    const int cx0 = std::clamp(int(ax * orig.w) - cs / 2, 0, std::max(0, orig.w - cs)), cy0 = std::clamp(int(ay * orig.h) - cs / 2, 0, std::max(0, orig.h - cs));
    if (a.ok() && b.ok()) save(hcat({resizeW(crop(orig, cx0, cy0, cs, cs), 420), resizeW(crop(a, cx0, cy0, cs, cs), 420), resizeW(crop(b, cx0, cy0, cs, cs), 420)}), out / (name + "_push.jpg"));
  }
  dapple_session_close(s);
  return 0;
}


// ======================================================================================================================================
// --bg: background and shadow evidence (docs/engine/pipeline-architecture.md section 48)
// ======================================================================================================================================
double lumaAt(const Pic& p, int x, int y) {
  const std::uint8_t* q = &p.d[(std::size_t(y) * p.w + x) * 3];
  return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
}
// the person plane at render pixel (x, y), 0..255 (nearest)
int planeAt(const Plane& m, int w, int h, int x, int y) {
  if (!m.w) return 0;
  return m.d[std::size_t(std::clamp(int((y + 0.5) * m.h / h), 0, m.h - 1)) * m.w + std::clamp(int((x + 0.5) * m.w / w), 0, m.w - 1)];
}
// distance (render px) from the marked pixels, chamfer
std::vector<float> distFrom(const std::vector<char>& in, int w, int h) {
  std::vector<float> d(std::size_t(w) * h, 1e9f);
  for (std::size_t i = 0; i < d.size(); ++i)
    if (in[i]) d[i] = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float& v = d[std::size_t(y) * w + x];
      if (x > 0) v = std::min(v, d[std::size_t(y) * w + x - 1] + 1);
      if (y > 0) {
        v = std::min(v, d[std::size_t(y - 1) * w + x] + 1);
        if (x > 0) v = std::min(v, d[std::size_t(y - 1) * w + x - 1] + 1.4142f);
        if (x < w - 1) v = std::min(v, d[std::size_t(y - 1) * w + x + 1] + 1.4142f);
      }
    }
  for (int y = h - 1; y >= 0; --y)
    for (int x = w - 1; x >= 0; --x) {
      float& v = d[std::size_t(y) * w + x];
      if (x < w - 1) v = std::min(v, d[std::size_t(y) * w + x + 1] + 1);
      if (y < h - 1) {
        v = std::min(v, d[std::size_t(y + 1) * w + x] + 1);
        if (x < w - 1) v = std::min(v, d[std::size_t(y + 1) * w + x + 1] + 1.4142f);
        if (x > 0) v = std::min(v, d[std::size_t(y + 1) * w + x - 1] + 1.4142f);
      }
    }
  return d;
}

struct BgMetrics {
  double bgChanged = 0, bgChangedFar = 0;  // % of background pixels whose colour changed by more than 4 levels; the same beyond 8 px of the silhouette
  double lineMax = 0, lineMoved = 0;       // max |displacement| of the drawn lines (px), share of line samples displaced more than 1 px (%)
  int lines = 0;
  double nearShadow = -1, farShadow = -1;  // % of the shadow pixels (near the person / farther) whose colour changed
  double bgChangedOutside = 0;             // % of pixels outside person + shadow (beyond 8 px) that changed
  double lineDev = 0, lineSlope = 0, frameChanged = 0;  // max deviation of a drawn line from its best straight line (px), max |slope| of that line, % of the frame that changed
};

// the drawn lines of the lines photos: rows where most background columns are dark
std::vector<int> findLines(const Pic& o, const Plane& person) {
  std::vector<int> rows;
  for (int y = 0; y < o.h; ++y) {
    int nb = 0, nd = 0;
    for (int x = 0; x < o.w; x += 2) {
      if (planeAt(person, o.w, o.h, x, y) >= 40) continue;
      ++nb, nd += lumaAt(o, x, y) < 45;
    }
    rows.push_back(nb > o.w / 8 && nd * 10 > nb * 6 ? 1 : 0);
  }
  std::vector<int> c;
  for (int y = 0; y < o.h;) {
    if (!rows[y]) {
      ++y;
      continue;
    }
    int y1 = y;
    while (y1 < o.h && rows[y1]) ++y1;
    c.push_back((y + y1 - 1) / 2);
    y = y1;
  }
  return c;
}

BgMetrics measure(const Pic& o, const Pic& r, const Plane& person, const std::vector<int>& lines, const std::vector<char>* shadowNear, const std::vector<char>* shadowFar, const dapple::warp::Field* fld) {
  BgMetrics m;
  const int w = o.w, h = o.h;
  std::vector<char> inS(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) inS[std::size_t(y) * w + x] = planeAt(person, w, h, x, y) >= 128;
  const std::vector<float> dist = distFrom(inS, w, h);
  auto changed = [&](int x, int y) {
    const std::uint8_t *a = &o.d[(std::size_t(y) * w + x) * 3], *b = &r.d[(std::size_t(y) * w + x) * 3];
    return std::abs(int(a[0]) - b[0]) > 4 || std::abs(int(a[1]) - b[1]) > 4 || std::abs(int(a[2]) - b[2]) > 4;
  };
  std::size_t nb = 0, cb = 0, nf = 0, cf = 0, nn = 0, cn = 0, nfs = 0, cfs = 0, no = 0, co = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      if (planeAt(person, w, h, x, y) >= 77) continue;  // background = plane < 0.3
      const bool c = changed(x, y);
      ++nb, cb += c;
      if (dist[i] > 8) ++nf, cf += c;
      const bool sn = shadowNear && (*shadowNear)[i], sf = shadowFar && (*shadowFar)[i];
      if (sn) ++nn, cn += c;
      else if (sf) ++nfs, cfs += c;
      else if (dist[i] > 8) ++no, co += c;
    }
  m.bgChanged = nb ? 100.0 * double(cb) / double(nb) : 0, m.bgChangedFar = nf ? 100.0 * double(cf) / double(nf) : 0;
  m.bgChangedOutside = no ? 100.0 * double(co) / double(no) : 0;
  if (shadowNear) m.nearShadow = nn ? 100.0 * double(cn) / double(nn) : 0, m.farShadow = nfs ? 100.0 * double(cfs) / double(nfs) : 0;
  {
    std::size_t nfr = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) nfr += changed(x, y);
    m.frameChanged = 100.0 * double(nfr) / (double(w) * h);
  }
  if (!lines.empty() && fld) {
    // where the drawn lines go: every sample of a line (background columns) is carried through the field's forward map (the same per-pixel rule as the
    // renderer), then the best straight line through the carried samples gives the deviation and the slope
    std::size_t ns = 0, nm = 0;
    for (int yc : lines) {
      std::vector<std::pair<double, double>> q;
      for (int x = 0; x < w; x += 4) {
        bool bg = true;
        for (int dy = -3; dy <= 3 && bg; ++dy) bg = planeAt(person, w, h, x, std::clamp(yc + dy, 0, h - 1)) < 40;
        if (!bg || dist[std::size_t(yc) * w + x] < 12) continue;
        double uo, vo;
        dapple::warp::forwardUV(*fld, (x + 0.5) / w, (yc + 0.5) / h, uo, vo);
        const double xo = uo * w - 0.5, yo = vo * h - 0.5;
        q.emplace_back(xo, yo);
        const double d = std::hypot(xo - x, yo - yc);
        ++ns, nm += d > 1;
        m.lineMax = std::max(m.lineMax, d);
      }
      if (q.size() < 20) continue;
      double sx = 0, sy = 0, sxx = 0, sxy = 0;
      for (const auto& t : q) sx += t.first, sy += t.second, sxx += t.first * t.first, sxy += t.first * t.second;
      const double n = double(q.size()), den = n * sxx - sx * sx;
      const double sl = den != 0 ? (n * sxy - sx * sy) / den : 0, ic = (sy - sl * sx) / n;
      double dv = 0;
      for (const auto& t : q) dv = std::max(dv, std::fabs(t.second - (sl * t.first + ic)));
      m.lineDev = std::max(m.lineDev, dv), m.lineSlope = std::max(m.lineSlope, std::fabs(sl));
    }
    m.lineMoved = ns ? 100.0 * double(nm) / double(ns) : 0;
  }
  m.lines = int(lines.size());
  return m;
}

Pic zoom2(const Pic& a, int cx, int cy, int half) {
  const Pic c = crop(a, cx - half, cy - half, 2 * half, 2 * half);
  Pic o;
  o.w = c.w * 2, o.h = c.h * 2, o.d.resize(std::size_t(o.w) * o.h * 3);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      for (int k = 0; k < 3; ++k) o.d[(std::size_t(y) * o.w + x) * 3 + k] = c.d[(std::size_t(y / 2) * c.w + x / 2) * 3 + k];
  return o;
}

// contact / cast shadow from the picture and the person plane (no model): dark relative to the local background and connected to the silhouette.
// near = within 0.03 L of the silhouette, far = the rest of that connected shadow.
void detectShadow(const Pic& o, const Plane& person, std::vector<char>& nearM, std::vector<char>& farM) {
  const int w = o.w, h = o.h;
  const int L = std::max(w, h);
  std::vector<char> inS(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) inS[std::size_t(y) * w + x] = planeAt(person, w, h, x, y) >= 40;
  // masked box blur of the background luminance (integral images at 1/4 resolution), radius 0.08 L
  const int q = 4, gw = w / q, gh = h / q, rad = std::max(2, int(0.08 * L / q));
  std::vector<double> I((std::size_t(gw) + 1) * (gh + 1), 0.0), C((std::size_t(gw) + 1) * (gh + 1), 0.0);
  for (int y = 0; y < gh; ++y)
    for (int x = 0; x < gw; ++x) {
      const int sx = x * q + q / 2, sy = y * q + q / 2;
      const bool bg = !inS[std::size_t(sy) * w + sx];
      const double l = bg ? lumaAt(o, sx, sy) : 0.0;
      const std::size_t a = std::size_t(y + 1) * (gw + 1) + x + 1, b = std::size_t(y) * (gw + 1) + x + 1, c = std::size_t(y + 1) * (gw + 1) + x, d = std::size_t(y) * (gw + 1) + x;
      I[a] = l + I[b] + I[c] - I[d];
      C[a] = (bg ? 1 : 0) + C[b] + C[c] - C[d];
    }
  auto box = [&](const std::vector<double>& T, int x, int y) {
    const int x0 = std::max(0, x - rad), x1 = std::min(gw, x + rad + 1), y0 = std::max(0, y - rad), y1 = std::min(gh, y + rad + 1);
    return T[std::size_t(y1) * (gw + 1) + x1] - T[std::size_t(y0) * (gw + 1) + x1] - T[std::size_t(y1) * (gw + 1) + x0] + T[std::size_t(y0) * (gw + 1) + x0];
  };
  std::vector<char> cand(std::size_t(w) * h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (inS[std::size_t(y) * w + x]) continue;
      const int gx = std::min(gw - 1, x / q), gy = std::min(gh - 1, y / q);
      const double c = box(C, gx, gy);
      if (c < 4) continue;
      cand[std::size_t(y) * w + x] = lumaAt(o, x, y) < 0.5 * (box(I, gx, gy) / c);
    }
  const std::vector<float> d = distFrom(inS, w, h);
  std::vector<int> stack;
  std::vector<char> seen(std::size_t(w) * h, 0);
  for (std::size_t i = 0; i < cand.size(); ++i)
    if (cand[i] && d[i] <= 3) seen[i] = 1, stack.push_back(int(i));
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    const int x = i % w, y = i / w;
    const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
    for (int k = 0; k < 4; ++k) {
      if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
      const std::size_t j = std::size_t(ny[k]) * w + nx[k];
      if (cand[j] && !seen[j]) seen[j] = 1, stack.push_back(int(j));
    }
  }
  nearM.assign(std::size_t(w) * h, 0), farM.assign(std::size_t(w) * h, 0);
  for (std::size_t i = 0; i < seen.size(); ++i)
    if (seen[i]) (d[i] <= 0.03 * L ? nearM : farM)[i] = 1;
}

// --grad: raw fold gradient and peak move of every body operator at 100 (no rendering)
int runGrad(const std::string& name) {
  const fs::path dir = fs::path(inRoot()) / name;
  Input in;
  if (!loadInput(dir, in) || in.pose.size() != 99) return 1;
  const Plane person = loadPlane(dir / "person.png");
  const std::string matteId = "rs:" + name + ":person";
  if (person.w) dapple_mask_raster_set(matteId.c_str(), person.w, person.h, person.d.data(), person.w);
  int pw = 2048, ph = 1365;
  {
    int c = 0;
    stbi_info((dir / "photo.png").string().c_str(), &pw, &ph, &c);
    const double k = 2048.0 / std::max(pw, ph);
    pw = int(pw * k), ph = int(ph * k);
  }
  for (const char* k : {"waist", "arms", "legs", "longlegs", "head"}) {
    const std::string js = "{\"reshape\":{\"v\":2,\"matte\":\"" + matteId + "\",\"bodies\":[{\"" + k + "\":" + std::string(std::getenv("DAPPLE_GRAD_VALUE") ? std::getenv("DAPPLE_GRAD_VALUE") : "100") + ",\"pts\":" + arr(in.pose) + "}]}}";
    auto p = dapple::fromJson(js);
    if (!p) continue;
    auto f = dapple::warp::bind(p->reshape, pw, ph, false);
    if (f) {
      // the peak of the field over the pixels that show the person (original plane >= 0.5): the field also peaks just outside the silhouette, where the
      // layered mode does not use it
      double pp = 0;
      for (int j = 0; j < f->gh; ++j)
        for (int i = 0; i < f->gw; ++i) {
          const double u = double(i) / (f->gw - 1), v = double(j) / (f->gh - 1);
          if (!person.w || planeAt(person, 1000, 1000, int(u * 999), int(v * 999)) < 128) continue;
          double dx, dy;
          dapple::warp::sampleUV(*f, u, v, dx, dy);
          if (f->layered) {  // the person layer itself
            const std::size_t o = 2 * (std::size_t(j) * f->gw + i);
            dx = f->dp[o], dy = f->dp[o + 1];
          }
          pp = std::max(pp, std::hypot(dx, dy));
        }
      std::printf("%-22s %-9s raw grad %.3f%s peak move %.1f px (on person pixels %.1f px) layered %d\n", name.c_str(), k, f->rawGrad, f->gradLimited ? " LIMITED" : "", f->maxMove * std::max(pw, ph),
                  pp * std::max(pw, ph), int(f->layered));
    }
  }
  return 0;
}

int runBg(const fs::path& out, const std::string& name, std::ofstream& txt, bool legsMode = false) {
  const fs::path dir = fs::path(inRoot()) / name;
  Input in;
  in.name = name;
  if (!loadInput(dir, in)) return 1;
  const Plane person = loadPlane(dir / "person.png");
  const std::string matteId = "rs:" + name + ":person";
  if (person.w) dapple_mask_raster_set(matteId.c_str(), person.w, person.h, person.d.data(), person.w);
  DappleSession* s = dapple_session_open((dir / "photo.png").string().c_str(), DAPPLE_QUALITY_FULL, 0);
  if (!s) return 1;
  const int LE = 2048;
  const Pic orig = render(s, "{}", LE);
  if (!orig.ok() || in.pose.size() != 99) return 1;
  const std::vector<int> lines = findLines(orig, person);
  const bool shadowCase = name.find("shadow") != std::string::npos;
  std::vector<char> nearM, farM;
  std::string matteU = matteId;
  if (shadowCase) {
    detectShadow(orig, person, nearM, farM);
    // variant (b): the person plane united with the near shadow
    Plane u = person;
    for (int y = 0; y < u.h; ++y)
      for (int x = 0; x < u.w; ++x) {
        const int ox = std::clamp(int((x + 0.5) * orig.w / u.w), 0, orig.w - 1), oy = std::clamp(int((y + 0.5) * orig.h / u.h), 0, orig.h - 1);
        if (nearM[std::size_t(oy) * orig.w + ox]) u.d[std::size_t(y) * u.w + x] = 255;
      }
    matteU = matteId + "+shadow";
    dapple_mask_raster_set(matteU.c_str(), u.w, u.h, u.d.data(), u.w);
    Pic sp = orig;
    for (std::size_t i = 0; i < nearM.size(); ++i) {
      if (nearM[i]) sp.d[3 * i] = 255, sp.d[3 * i + 1] = 60, sp.d[3 * i + 2] = 60;
      else if (farM[i]) sp.d[3 * i] = 60, sp.d[3 * i + 1] = 60, sp.d[3 * i + 2] = 255;
    }
    save(resizeW(sp, 700), out / (name + "_shadow_detected.jpg"));
  }
  std::printf("%s: %dx%d, %zu lines detected\n", name.c_str(), orig.w, orig.h, lines.size());
  const char* keys[] = {"waist", "arms", "legs", "longlegs", "head"};
  struct Var {
    const char* label;
    bool layered;
    double band;
    bool uni;
    int leg;
  };
  std::vector<Var> vars = {{"old", false, 0, false, 1}, {"new", true, 0, false, 1}};
  if (shadowCase) vars = {{"old", false, 0, false, 1}, {"old band 0.01", false, 0.01, false, 1}, {"old band 0.02", false, 0.02, false, 1}, {"new", true, 0, false, 1}, {"new+shadow plane", true, 0, true, 1}};
  std::vector<const char*> keyList(std::begin(keys), std::end(keys));
  if (legsMode) {
    vars = {{"W windowed", false, 0, false, 0}, {"L windowed+layer", true, 0, false, 1}, {"A leg axis", false, 0, false, 2}, {"B vertical", false, 0, false, 3}};
    keyList = {"longlegs"};
  }
  const int panel = shadowCase ? 260 : 360;
  std::vector<Pic> rows;
  for (const char* k : keyList) {
    const std::string ops = std::string("\"") + k + "\":100";
    std::vector<Pic> cols{resizeW(orig, panel)};
    std::vector<Pic> rs;
    for (const Var& v : vars) {
      dapple::warp::setLayered(v.layered);
      dapple::warp::setBandOverride(v.band);
      dapple::warp::setLegMode(v.leg);
      const std::string mid = v.uni ? matteU : matteId;
      const std::string js = "{\"reshape\":{\"v\":2,\"matte\":\"" + mid + "\",\"bodies\":[{" + ops + ",\"pts\":" + arr(in.pose) + "}]}}";
      const Pic r = render(s, js, LE);
      dapple::warp::setLayered(true);
      dapple::warp::setBandOverride(0);
      dapple::warp::setLegMode(3);
      if (!r.ok()) continue;
      std::shared_ptr<const dapple::warp::Field> fieldPtr;
      if (auto pp = dapple::fromJson(js)) {
        dapple::warp::setLayered(v.layered);
        dapple::warp::setBandOverride(v.band);
        dapple::warp::setLegMode(v.leg);
        fieldPtr = dapple::warp::bind(pp->reshape, orig.w, orig.h, false);
        dapple::warp::setLayered(true);
        dapple::warp::setBandOverride(0);
        dapple::warp::setLegMode(3);
      }
      const BgMetrics m = measure(orig, r, person, lines, shadowCase ? &nearM : nullptr, shadowCase ? &farM : nullptr, fieldPtr.get());
      char b[640];
      std::snprintf(b, sizeof b, "%-22s %-9s %-18s bg changed %6.2f %% (beyond 8 px of the silhouette %6.2f %%)  lines %2d: max shift %5.1f px, %5.1f %% of samples > 1 px", name.c_str(), k, v.label,
                    m.bgChanged, m.bgChangedFar, m.lines, m.lineMax, m.lineMoved);
      std::string line = b;
      std::snprintf(b, sizeof b, "  | straightness: max deviation %5.2f px, max slope %.3f; frame changed %5.1f %%", m.lineDev, m.lineSlope, m.frameChanged);
      line += b;
      if (shadowCase) {
        std::snprintf(b, sizeof b, "  | near shadow changed %5.1f %%, far shadow %5.1f %%, other bg beyond 8 px %6.2f %%", m.nearShadow, m.farShadow, m.bgChangedOutside);
        line += b;
      }
      if (fieldPtr) line += "  [raw grad " + std::to_string(fieldPtr->rawGrad).substr(0, 5) + (fieldPtr->gradLimited ? " LIMITED" : "") + ", max |B| " + std::to_string(fieldPtr->maxMove * LE).substr(0, 5) + " px]";
      txt << line << "\n";
      std::printf("%s\n", line.c_str());
      cols.push_back(resizeW(r, panel));
      rs.push_back(r);
    }
    // 100 % zoom (x2 nearest) on the silhouette edge: the centroid of the changed pixels that lie within 14 px inside the old silhouette (the strip the
    // person left); when there is none (longlegs: the person grows), of the changed pixels within 14 px outside it
    const Pic& ref = rs.size() >= 2 ? rs[shadowCase ? 3 : 1] : orig;
    std::vector<char> inO(std::size_t(orig.w) * orig.h), outO(std::size_t(orig.w) * orig.h);
    for (int y = 0; y < orig.h; ++y)
      for (int x = 0; x < orig.w; ++x) inO[std::size_t(y) * orig.w + x] = planeAt(person, orig.w, orig.h, x, y) >= 128, outO[std::size_t(y) * orig.w + x] = !inO[std::size_t(y) * orig.w + x];
    const std::vector<float> dIn = distFrom(outO, orig.w, orig.h), dOut = distFrom(inO, orig.w, orig.h);
    double sx = 0, sy = 0, n = 0, sx2 = 0, sy2 = 0, n2 = 0;
    for (int y = 0; y < ref.h; ++y)
      for (int x = 0; x < ref.w; ++x) {
        const std::size_t i = std::size_t(y) * ref.w + x;
        const std::uint8_t *a0 = &orig.d[i * 3], *c = &ref.d[i * 3];
        if (std::abs(int(a0[0]) - c[0]) + std::abs(int(a0[1]) - c[1]) + std::abs(int(a0[2]) - c[2]) <= 40) continue;
        if (inO[i] && dIn[i] <= 14) sx += x, sy += y, ++n;
        else if (!inO[i] && dOut[i] <= 14) sx2 += x, sy2 += y, ++n2;
      }
    int ccx = orig.w / 2, ccy = orig.h / 2;
    if (n > 20) ccx = int(sx / n), ccy = int(sy / n);
    else if (n2 > 20) ccx = int(sx2 / n2), ccy = int(sy2 / n2);
    std::vector<Pic> zc{zoom2(orig, ccx, ccy, 100)};
    for (const Pic& r : rs) zc.push_back(zoom2(r, ccx, ccy, 100));
    rows.push_back(hcat(cols));
    rows.push_back(hcat(zc));
  }
  save(vcat(rows), out / (name + (legsMode ? "_legs.jpg" : "_bg.jpg")));
  dapple_session_close(s);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: dapple_reshape_sheets <outdir> [names...]\n");
    return 2;
  }
  const fs::path out = argv[1];
  fs::create_directories(out);
  std::vector<std::string> names;
  bool bgMode = false, gradMode = false, legsOnly = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bg") == 0) bgMode = true;
    else if (std::strcmp(argv[i], "--legs") == 0) bgMode = true, legsOnly = true;
    else if (std::strcmp(argv[i], "--grad") == 0) gradMode = true;
    else names.push_back(argv[i]);
  }
  if (names.empty())
    for (const auto& e : fs::directory_iterator(inRoot()))
      if (e.is_directory()) names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  if (gradMode) {
    for (const auto& n : names) runGrad(n);
    return 0;
  }
  if (bgMode) {
    std::ofstream txt(out / (legsOnly ? "legs.txt" : "bg.txt"));
    int rc2 = 0;
    for (const auto& n : names) rc2 |= runBg(out, n, txt, legsOnly);
    return rc2;
  }
  std::ofstream lines(out / "lines.txt");
  int rc = 0;
  for (const auto& n : names) rc |= run(out, n, lines);
  return rc;
}
