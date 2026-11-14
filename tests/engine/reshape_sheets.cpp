// Evidence sheets of the portrait reshaping (docs/engine/pipeline-architecture.md section 45) through the C API. Not a regression test.
//   dapple_reshape_sheets <outdir> [names...]        (run from the repository root)
// Reads build/reshape/in/<name>/{photo.png, person.png, meta.json} (scripts/reshape/landmarks.py), writes into <outdir>:
//   <name>_face.jpg   rows per face operator (slim, small, chin +, chin -, eyes, nose, all at 50): original | 50 | 100 | |B| at 100 (face crop)
//   <name>_body.jpg   rows per body operator (waist, arms, legs, long legs, head, all at 50): original | 50 | 100 | |B| at 100 (whole photo)
//   <name>_push.jpg   a push stroke and a restore over half of it: original | push | push + restore
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

int run(const fs::path& out, const std::string& name, std::ofstream& lines) {
  const fs::path dir = fs::path("build/reshape/in") / name;
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
    std::string r = "{\"reshape\":{\"v\":1,\"matte\":\"" + matteId + "\"";
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
  for (int i = 2; i < argc; ++i) names.push_back(argv[i]);
  if (names.empty())
    for (const auto& e : fs::directory_iterator("build/reshape/in"))
      if (e.is_directory()) names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  std::ofstream lines(out / "lines.txt");
  int rc = 0;
  for (const auto& n : names) rc |= run(out, n, lines);
  return rc;
}
