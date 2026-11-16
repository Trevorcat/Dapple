// Interactive colour-lattice size: how much does a smaller lattice change a drag frame?
//   dapple_lattice_size_bench <film pack .dfp> [long edge=810] [film] [image ...]      (no image: the public samples under build/rawsamples)
// For every image x colour edit (each with and without a film stock) it renders the interactive frame (the app's flags: INTERACTIVE | ANALYSIS, tone strength 60)
// with lattice sizes 33 .. 57 and the settled frame (not interactive, 65), and prints dE2000 (mean / p99 / max, CIEDE2000 of the 8-bit sRGB output) of every size
// against the 57 interactive frame and against the settled frame. The last block is the worst case of each size over all images x edits and the verdict of the rule
// that chose the engine's interactive size (docs/engine/pipeline-architecture.md 57): p99 vs 57 <= 1.5, mean vs 57 <= 0.25, mean vs settled at most 0.1 above
// what 57 has. DAPPLE_INTERACTIVE_LATTICE=0 is set here so that the sizes are baked as asked.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"

namespace {
void lab(const uint8_t* p, double* L, double* a, double* b) {
  auto lin = [](double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  const double r = lin(p[0]), g = lin(p[1]), bl = lin(p[2]);
  double x = (0.4124 * r + 0.3576 * g + 0.1805 * bl) / 0.95047, y = 0.2126 * r + 0.7152 * g + 0.0722 * bl, z = (0.0193 * r + 0.1192 * g + 0.9505 * bl) / 1.08883;
  auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
  x = f(x), y = f(y), z = f(z);
  *L = 116.0 * y - 16.0, *a = 500.0 * (x - y), *b = 200.0 * (y - z);
}
double de2000(double L1, double a1, double b1, double L2, double a2, double b2) {
  const double kPi = 3.14159265358979323846, rad = kPi / 180.0;
  const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2), Cb = (C1 + C2) / 2, G = 0.5 * (1 - std::sqrt(std::pow(Cb, 7) / (std::pow(Cb, 7) + std::pow(25.0, 7))));
  const double ap1 = (1 + G) * a1, ap2 = (1 + G) * a2, Cp1 = std::hypot(ap1, b1), Cp2 = std::hypot(ap2, b2);
  double h1 = std::atan2(b1, ap1) / rad, h2 = std::atan2(b2, ap2) / rad;
  if (h1 < 0) h1 += 360;
  if (h2 < 0) h2 += 360;
  const double dL = L2 - L1, dC = Cp2 - Cp1;
  double dh = h2 - h1;
  if (Cp1 * Cp2 == 0) dh = 0; else if (dh > 180) dh -= 360; else if (dh < -180) dh += 360;
  const double dH = 2 * std::sqrt(Cp1 * Cp2) * std::sin(dh * rad / 2);
  const double Lb = (L1 + L2) / 2, Cpb = (Cp1 + Cp2) / 2;
  double hb = h1 + h2;
  if (Cp1 * Cp2 == 0) hb = h1 + h2; else if (std::fabs(h1 - h2) <= 180) hb /= 2; else hb = (hb + (hb < 360 ? 360 : -360)) / 2;
  const double T = 1 - 0.17 * std::cos((hb - 30) * rad) + 0.24 * std::cos(2 * hb * rad) + 0.32 * std::cos((3 * hb + 6) * rad) - 0.20 * std::cos((4 * hb - 63) * rad);
  const double dTh = 30 * std::exp(-std::pow((hb - 275) / 25, 2)), Rc = 2 * std::sqrt(std::pow(Cpb, 7) / (std::pow(Cpb, 7) + std::pow(25.0, 7)));
  const double Sl = 1 + 0.015 * std::pow(Lb - 50, 2) / std::sqrt(20 + std::pow(Lb - 50, 2)), Sc = 1 + 0.045 * Cpb, Sh = 1 + 0.015 * Cpb * T, Rt = -std::sin(2 * dTh * rad) * Rc;
  return std::sqrt(std::pow(dL / Sl, 2) + std::pow(dC / Sc, 2) + std::pow(dH / Sh, 2) + Rt * (dC / Sc) * (dH / Sh));
}
struct Stat {
  double mean = 0, p99 = 0, max = 0;
};
Stat compare(const DappleImage& a, const DappleImage& b) {
  Stat s;
  if (a.width != b.width || a.height != b.height) return {1e9, 1e9, 1e9};
  const size_t n = size_t(a.width) * a.height;
  std::vector<float> d(n);
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double l1, a1, b1, l2, a2, b2;
    lab(a.rgba + 4 * i, &l1, &a1, &b1);
    lab(b.rgba + 4 * i, &l2, &a2, &b2);
    d[i] = float(de2000(l1, a1, b1, l2, a2, b2));
    sum += d[i];
  }
  std::sort(d.begin(), d.end());
  s.mean = sum / double(n);
  s.p99 = d[std::min(n - 1, size_t(double(n) * 0.99))];
  s.max = d[n - 1];
  return s;
}
struct Edit {
  std::string name, key, json;
};
struct Worst {
  double v = -1;
  std::string where;
  void note(double x, const std::string& w) {
    if (x > v) v = x, where = w;
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return std::fprintf(stderr, "usage: %s pack.dfp [long_edge] [image ...]\n", argv[0]), 2;
#if defined(_WIN32)
  _putenv_s("DAPPLE_INTERACTIVE_LATTICE", "0");
#else
  setenv("DAPPLE_INTERACTIVE_LATTICE", "0", 1);
#endif
  if (dapple_film_load_pack(argv[1]) != DAPPLE_OK) return std::fprintf(stderr, "pack: %s\n", dapple_last_error()), 1;
  const int le = argc > 2 ? std::atoi(argv[2]) : 810;
  std::vector<std::string> images;
  bool onlyFilm = false;  // argument "film": only the cases with a film stock
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "film") onlyFilm = true;
    else images.push_back(argv[i]);
  }
  if (images.empty())
    images = {"build/rawsamples/Sony/ILCE-6700/DSC00002.ARW",
              "build/rawsamples/Sony/ILCE-7RM4/DSC00396.ARW",
              "build/rawsamples/Fujifilm/X-S10/Fujifilm-X-S10-lossless-compressed.RAF",
              "build/rawsamples/Canon/EOS-R5/Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3",
              "build/rawsamples/Nikon/Z-8/Nikon_Z8_raw_14_bit_lossless_compression.NEF",
              "build/rawsamples/Apple/iPhone-12-Pro/IMG_1361.DNG"};
  std::vector<Edit> edits = {
      {"saturation+60", "saturation", "\"saturation\":60"},
      {"saturation-60", "saturation", "\"saturation\":-60"},
      {"vibrance+60", "vibrance", "\"vibrance\":60"},
      {"pad(t+,c+)", "vibrance", "\"temp\":30,\"vibrance\":55,\"saturation\":25"},
      {"pad(t+,c-)", "vibrance", "\"temp\":30,\"vibrance\":-55,\"saturation\":-25"},
      {"pad(t-,c+)", "vibrance", "\"temp\":-30,\"vibrance\":55,\"saturation\":25"},
      {"pad(t-,c-)", "vibrance", "\"temp\":-30,\"vibrance\":-55,\"saturation\":-25"},
      {"hsl", "hsl_red_s", "\"hsl\":{\"red\":[0,80,0],\"green\":[-60,70,-30],\"blue\":[50,-80,40],\"magenta\":[-40,60,20]}"},
      {"look teal", "look", "\"look\":\"teal\""},
  };
  const int sizes[] = {33, 37, 41, 45, 49, 51, 53, 55, 57};
  const int nS = int(sizeof sizes / sizeof sizes[0]);
  // worst over all cases per size: p99 / mean vs 57, mean vs settled minus what 57 has
  std::vector<Worst> wP99(nS), wMean(nS), wMax(nS), wJump(nS), wSetMean(nS);
  std::vector<bool> ok(nS, true);
  using Clk = std::chrono::steady_clock;
  const auto t0 = Clk::now();
  auto render = [&](DappleSession* s, const std::string& json, bool interactive, int size, const char* key, DappleImage* out) {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = le;
    rq.flags = DAPPLE_FRAME_ANALYSIS | (interactive ? DAPPLE_FRAME_INTERACTIVE : 0);
    rq.bake_size = interactive ? size : 0;
    rq.active_param = interactive ? key : nullptr;
    char* an = nullptr;
    const bool r = dapple_session_render_frame(s, json.c_str(), nullptr, &rq, out, &an) == DAPPLE_OK;
    if (an) dapple_free_string(an);
    if (!r) std::fprintf(stderr, "render: %s\n", dapple_last_error());
    return r;
  };
  for (const std::string& path : images) {
    DappleSession* s = dapple_session_open(path.c_str(), 0, 2048);
    if (!s) {
      std::fprintf(stderr, "skip %s: %s\n", path.c_str(), dapple_last_error());
      continue;
    }
    const std::string base = path.substr(path.find_last_of('/') + 1);
    std::printf("== %s ==\n", base.c_str());
    for (int film = onlyFilm ? 1 : 0; film < 3; ++film)
      for (const Edit& e : edits) {
        // film 1: the stock at its default strength (the colour lattice is then a stub: the film carries the colour), film 2: strength 60 (blended with the colour stage)
        const std::string json = std::string("{") + (film ? "\"film\":\"kodak_portra_400\"," : "") + (film == 2 ? "\"film_strength\":60," : "") + "\"toneStrength\":60," + e.json + "}";
        const std::string label = base + " " + e.name + (film == 2 ? " +film60" : film ? " +film" : "");
        // a fresh session per case: the lattice of a colour stage is shared by every edit that only differs in white balance or tone
        DappleSession* s = dapple_session_open(path.c_str(), 0, 2048);
        DappleImage img[16]{}, settled{};
        if (film) {  // the first interactive frame of a film also bakes its coarse print tables: render it once so that no size pays for it in the comparison
          DappleImage w{};
          render(s, std::string("{\"film\":\"kodak_portra_400\",") + (film == 2 ? "\"film_strength\":60," : "") + "\"toneStrength\":60,\"saturation\":3}", true, 57, e.key.c_str(), &w);
          dapple_free_image(&w);
        }  // ascending sizes: a size is baked only when no finer lattice of this colour stage is cached yet
        bool good = true;
        for (int k = 0; k < nS && good; ++k) good = render(s, json, true, sizes[k], e.key.c_str(), &img[k]);
        good = good && render(s, json, false, 0, "", &settled);
        if (!good) {
          std::fprintf(stderr, "case failed: %s\n", label.c_str());
          for (auto& i : img) dapple_free_image(&i);
          dapple_free_image(&settled);
          dapple_session_close(s);
          continue;
        }
        const Stat ref57settled = compare(img[nS - 1], settled);
        std::printf("  %-26s", (e.name + (film == 2 ? " +film60" : film ? " +film" : "")).c_str());
        for (int k = 0; k < nS; ++k) {
          const Stat v = compare(img[k], img[nS - 1]), t = compare(img[k], settled);
          std::printf(" | %d: p99 %.2f mean %.3f max %.1f set %.3f", sizes[k], v.p99, v.mean, v.max, t.mean);
          if (k == nS - 1) continue;
          wP99[k].note(v.p99, label);
          wMean[k].note(v.mean, label);
          wMax[k].note(v.max, label);
          wJump[k].note(t.mean - ref57settled.mean, label);
          wSetMean[k].note(t.mean, label);
          if (v.p99 > 1.5 || v.mean > 0.25 || t.mean - ref57settled.mean > 0.1) ok[k] = false;
        }
        std::printf(" | 57 vs settled mean %.3f p99 %.2f\n", ref57settled.mean, ref57settled.p99);
        std::fflush(stdout);
        for (auto& i : img) dapple_free_image(&i);
        dapple_free_image(&settled);
        dapple_session_close(s);
      }
  }
  std::printf("\n== worst case per size (all images x edits), dE2000 against the 57 interactive frame ==\n");
  std::printf("size   p99 (case)   mean (case)   max   mean-vs-settled minus 57's (case)   verdict\n");
  for (int k = 0; k < nS - 1; ++k)
    std::printf("%3d   %.2f (%s)   %.3f (%s)   %.1f   %+.3f (%s)   %s\n", sizes[k], wP99[k].v, wP99[k].where.c_str(), wMean[k].v, wMean[k].where.c_str(), wMax[k].v, wJump[k].v,
                wJump[k].where.c_str(), ok[k] ? "PASS" : "fail");
  int pick = 57;
  for (int k = nS - 2; k >= 0; --k) {
    if (!ok[k]) break;
    pick = sizes[k];
  }
  std::printf("smallest size that passes (all larger sizes tested pass too): %d   [%.0f s]\n", pick, std::chrono::duration<double>(Clk::now() - t0).count());
  return 0;
}
