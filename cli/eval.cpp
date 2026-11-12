// `dapple eval`: quantitative comparison of Dapple's output with the camera's own JPEG.
//
//   dapple eval --dir D:/photos [--step 10] [--offset 0] [--out build/eval] [--edge 1280]
//               [--cmp-edge 640] [--cache DIR] [--no-match] [--no-sheets] [--max N]
//
// For every N-th ARW: decode, render the untouched look and the --auto look at `edge`,
// compare with the embedded camera JPEG (resampled onto the same grid after an alignment
// check), in CIELAB (D65) at `cmp-edge`: CIEDE2000, dL, chroma ratio, hue error per sector.
// Writes eval.csv, eval_sectors.csv, summary.txt and contact sheets to --out.

#include "eval.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

#include "colour_metrics.h"
#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/developer.h"
#include "dapple/encode.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#include "dapple/tone.h"
#include "eval_common.h"

namespace dapple::evaltool {

using namespace metrics;

namespace {

constexpr int kNS = kSectorCount;

struct SectorAcc {
  double hueSum = 0, hueAbs = 0, cOurs = 0, cCam = 0;
  double dL = 0, dE = 0;
  long n = 0;
  void merge(const SectorAcc& o) {
    hueSum += o.hueSum; hueAbs += o.hueAbs; cOurs += o.cOurs; cCam += o.cCam;
    dL += o.dL; dE += o.dE; n += o.n;
  }
};

constexpr int kHistBins = 1200;  // 0.05 dE per bin up to 60
struct Acc {
  double dESum = 0, dLSum = 0, cOurs = 0, cCam = 0;
  long n = 0, nChroma = 0;
  double nDa = 0, nDb = 0;  // neutral pixels (camera chroma < 6, L* 20..85): mean a*, b* error
  long nNeutral = 0;
  std::vector<long> hist = std::vector<long>(kHistBins + 1, 0);
  std::array<SectorAcc, kNS> sec{};
  void merge(const Acc& o) {
    nDa += o.nDa; nDb += o.nDb; nNeutral += o.nNeutral;
    dESum += o.dESum; dLSum += o.dLSum; cOurs += o.cOurs; cCam += o.cCam; n += o.n; nChroma += o.nChroma;
    for (int i = 0; i <= kHistBins; ++i) hist[i] += o.hist[i];
    for (int s = 0; s < kNS; ++s) sec[s].merge(o.sec[s]);
  }
  double mean() const { return n ? dESum / n : 0; }
  double pct(double q) const {
    long acc = 0;
    for (int i = 0; i <= kHistBins; ++i) {
      acc += hist[i];
      if (acc >= q * n) return (i + 0.5) * 0.05;
    }
    return 60;
  }
  double dL() const { return n ? dLSum / n : 0; }
  double chromaRatio() const { return cCam > 0 ? cOurs / cCam : 0; }
};

Acc compare(const ImageF& ours, const ImageF& cam, int margin) {
  Acc a;
  const std::size_t px = std::size_t(ours.width) * ours.height;
  for (std::size_t i = 0; i < px; ++i) {
    const int xx = int(i % std::size_t(ours.width)), yy = int(i / std::size_t(ours.width));
    if (xx < margin || yy < margin || xx >= ours.width - margin || yy >= ours.height - margin) continue;
    const Lab lo = srgbToLab(&ours.data[3 * i]);
    const Lab lc = srgbToLab(&cam.data[3 * i]);
    const double de = deltaE2000(lc, lo);
    a.dESum += de;
    a.dLSum += lo.L - lc.L;
    a.hist[std::min(kHistBins, int(de / 0.05))]++;
    a.n++;
    const double Cc = lc.C(), Co = lo.C();
    if (Cc < 6 && lc.L > 20 && lc.L < 85) {
      a.nDa += lo.a - lc.a;
      a.nDb += lo.b - lc.b;
      a.nNeutral++;
    }
    if (Cc >= 5) {
      a.cOurs += Co;
      a.cCam += Cc;
      a.nChroma++;
    }
    if (Cc >= 10) {
      SectorAcc& s = a.sec[sectorOf(lc.h())];
      const double dh = wrapDeg(lo.h() - lc.h());
      s.hueSum += dh;
      s.hueAbs += std::fabs(dh);
      s.cOurs += Co;
      s.cCam += Cc;
      s.dL += lo.L - lc.L;
      s.dE += de;
      s.n++;
    }
  }
  return a;
}

// ---- contact sheet ----
const char* kDigits[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                           "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"};

void drawNumber(Image8& im, int x0, int y0, int value, int scale) {
  const std::string s = std::to_string(value);
  int x = x0;
  for (char ch : s) {
    const char* g = kDigits[ch - '0'];
    for (int gy = 0; gy < 5; ++gy)
      for (int gx = 0; gx < 3; ++gx)
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) {
            const int px = x + gx * scale + sx, py = y0 + gy * scale + sy;
            if (px < 0 || py < 0 || px >= im.width || py >= im.height) continue;
            std::uint8_t* p = &im.data[(std::size_t(py) * im.width + px) * 3];
            const bool on = g[gy * 3 + gx] == '1';
            p[0] = p[1] = p[2] = on ? 255 : 0;
          }
    x += 4 * scale;
  }
}

void blit(Image8& sheet, const ImageF& im, int x0, int y0, int cw, int ch) {
  const double s = std::min(double(cw) / im.width, double(ch) / im.height);
  const int dw = std::max(1, int(im.width * s)), dh = std::max(1, int(im.height * s));
  const ImageF r = resizeExact(im, dw, dh);
  const int ox = x0 + (cw - dw) / 2, oy = y0 + (ch - dh) / 2;
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x)
      for (int c = 0; c < 3; ++c)
        sheet.data[(std::size_t(oy + y) * sheet.width + ox + x) * 3 + c] =
            std::uint8_t(std::clamp(r.data[(std::size_t(y) * dw + x) * 3 + c], 0.f, 1.f) * 255.f + 0.5f);
}

struct Row {
  int index = 0;
  std::string name;
  Alignment align;
  Acc ours, autoV;
  EditParams autoParams;
  ToneAnalysis tone;
  std::string notes;
};

std::string f2(double v, int d = 2) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}

}  // namespace

int cmdEval(const std::vector<std::string>& args) {
  std::string dir, out = "build/eval", cache;
  int step = 10, offset = 0, edge = 1280, cmpEdge = 640, maxFiles = 0;
  bool matchOff = false, sheets = true;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto val = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
    const std::string& a = args[i];
    if (a == "--dir") dir = val();
    else if (a == "--out") out = val();
    else if (a == "--cache") cache = val();
    else if (a == "--step") step = std::atoi(val().c_str());
    else if (a == "--offset") offset = std::atoi(val().c_str());
    else if (a == "--edge") edge = std::atoi(val().c_str());
    else if (a == "--cmp-edge") cmpEdge = std::atoi(val().c_str());
    else if (a == "--max") maxFiles = std::atoi(val().c_str());
    else if (a == "--no-match") matchOff = true;
    else if (a == "--no-sheets") sheets = false;
    else {
      std::fprintf(stderr, "eval: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (dir.empty() || step < 1) {
    std::fputs("usage: dapple eval --dir <folder with ARW> [--step 10] [--offset 0] [--out DIR] [--edge 1280]\n"
               "                    [--cmp-edge 640] [--cache DIR] [--no-match] [--no-sheets] [--max N]\n",
               stderr);
    return 2;
  }
  if (cache.empty()) cache = out + "/cache";
  const Developer dev;
  const auto all = listArw(fromU8(dir));
  std::vector<fs::path> files;
  for (std::size_t i = std::size_t(offset); i < all.size(); i += std::size_t(step)) files.push_back(all[i]);
  if (maxFiles > 0 && files.size() > std::size_t(maxFiles)) files.resize(std::size_t(maxFiles));
  if (files.empty()) {
    std::fputs("eval: no ARW files found\n", stderr);
    return 1;
  }
  std::fprintf(stderr, "eval: %zu of %zu files (every %d, offset %d), edge %d, compare at %d\n", files.size(),
               all.size(), step, offset, edge, cmpEdge);
  fs::create_directories(fromU8(out));
  const std::vector<Case> cases = loadCases(dev, files, edge, fromU8(cache));

  std::vector<Row> rows;
  Acc poolOurs, poolAuto;
  std::vector<Image8> sheetsImg;
  constexpr int kCellW = 400, kCellH = 280, kPerSheet = 6;
  Image8 sheet;
  int sheetRows = 0, sheetNo = 0;
  auto flushSheet = [&] {
    if (sheetRows == 0) return;
    sheet.height = sheetRows * kCellH;
    sheet.data.resize(std::size_t(sheet.width) * sheet.height * 3);
    char nm[64];
    std::snprintf(nm, sizeof nm, "/sheet_%02d.jpg", ++sheetNo);
    writeJpeg(sheet, fromU8(out + nm), JpegOptions{88});
    sheetRows = 0;
  };

  for (std::size_t idx = 0; idx < cases.size(); ++idx) {
    const Case& c = cases[idx];
    CameraProfile cam = dev.cameras().resolve(c.src.meta.make, c.src.meta.model);
    if (matchOff) cam.colorMatch = {};
    Row row;
    row.index = int(idx);
    row.name = c.name;

    const AutoResult ar = computeAutoAdjust(c.src, cam, AutoOptions{});
    row.autoParams = ar.correction;
    row.notes = "";
    for (const auto& n : ar.notes) row.notes += (row.notes.empty() ? "" : "; ") + n;
    row.tone = ar.tone;

    ImageF oursF, autoF;
    if (!engine::renderImageF(EditParams{}, cam, nullptr, c.src, true, oursF) ||
        !engine::renderImageF(ar.correction, cam, nullptr, c.src, true, autoF)) {
      std::fprintf(stderr, "eval: render failed\n");
      std::exit(1);
    }

    row.align = c.align;

    const int cw = std::min(cmpEdge, std::max(oursF.width, oursF.height));
    const double sc = double(cw) / std::max(oursF.width, oursF.height);
    const int dw = std::max(1, int(std::lround(oursF.width * sc))), dh = std::max(1, int(std::lround(oursF.height * sc)));
    const ImageF camS = resizeExact(c.cam, dw, dh);
    const ImageF oursS = resizeExact(oursF, dw, dh);
    const ImageF autoS = resizeExact(autoF, dw, dh);
    const int mg = int(std::ceil(c.align.margin * sc));
    row.ours = compare(oursS, camS, mg);
    row.autoV = compare(autoS, camS, mg);
    poolOurs.merge(row.ours);
    poolAuto.merge(row.autoV);

    if (sheets) {
      if (sheetRows == 0) {
        sheet = Image8();
        sheet.width = 3 * kCellW;
        sheet.height = kPerSheet * kCellH;
        sheet.data.assign(std::size_t(sheet.width) * sheet.height * 3, 20);
      }
      blit(sheet, c.cam, 0, sheetRows * kCellH, kCellW, kCellH);
      blit(sheet, oursF, kCellW, sheetRows * kCellH, kCellW, kCellH);
      blit(sheet, autoF, 2 * kCellW, sheetRows * kCellH, kCellW, kCellH);
      drawNumber(sheet, 4, sheetRows * kCellH + 4, int(idx), 3);
      if (++sheetRows == kPerSheet) flushSheet();
    }
    rows.push_back(std::move(row));
    std::fprintf(stderr, "\r  evaluated %zu/%zu", idx + 1, cases.size());
  }
  flushSheet();
  std::fputc('\n', stderr);

  // ---- CSV ----
  {
    std::ofstream csv(fromU8(out + "/eval.csv"));
    csv << "index,file,variant,dE_mean,dE_p95,dL_mean,chroma_ratio,align_scale,align_k,align_tx,align_ty,align_ncc_identity,align_ncc_registered,"
           "auto_exposure,auto_temperature,auto_tint,auto_highlights,auto_shadows,tone_key,tone_range,tone_nearest,"
           "tone_p5,tone_p50,tone_p95,auto_tone_strength";
    for (int s = 0; s < kNS; ++s)
      csv << ",hue_err_" << sectorName(s) << ",chroma_ratio_" << sectorName(s) << ",n_" << sectorName(s);
    csv << "\n";
    for (const Row& r : rows)
      for (int v = 0; v < 2; ++v) {
        const Acc& a = v ? r.autoV : r.ours;
        csv << r.index << "," << r.name << "," << (v ? "auto" : "ours") << "," << f2(a.mean(), 3) << ","
            << f2(a.pct(0.95), 2) << "," << f2(a.dL(), 3) << "," << f2(a.chromaRatio(), 3) << "," << f2(r.align.scale, 4) << ","
            << f2(r.align.k, 3) << "," << f2(r.align.tx, 1) << "," << f2(r.align.ty, 1) << "," << f2(r.align.ncc0, 3)
            << "," << f2(r.align.ncc, 3) << ","
            << f2(v ? r.autoParams.exposure : 0, 3) << "," << f2(v ? r.autoParams.temperature : 0, 1) << ","
            << f2(v ? r.autoParams.tint : 0, 1) << "," << f2(v ? r.autoParams.highlights : 0, 1) << ","
            << f2(v ? r.autoParams.shadows : 0, 1) << "," << r.tone.key << "," << r.tone.range << "," << r.tone.nearest
            << "," << f2(r.tone.p5, 4) << "," << f2(r.tone.p50, 4) << "," << f2(r.tone.p95, 4) << ","
            << f2(v ? r.autoParams.toneStrength : 0, 1);
        for (int s = 0; s < kNS; ++s) {
          const SectorAcc& sa = a.sec[s];
          csv << "," << (sa.n ? f2(sa.hueSum / sa.n, 2) : "") << "," << (sa.cCam > 0 ? f2(sa.cOurs / sa.cCam, 3) : "")
              << "," << sa.n;
        }
        csv << "\n";
      }
  }

  // ---- summary ----
  std::ostringstream sum;
  const double n = double(rows.size());
  auto mean = [&](auto get) {
    double s = 0;
    for (const Row& r : rows) s += get(r);
    return s / n;
  };
  int improved = 0, same = 0, worse = 0, worseBig = 0, worseStrict = 0, lowNcc = 0;
  double minScale = 9, maxScale = 0, minNcc = 2, meanNcc0 = 0, meanNcc = 0;
  for (const Row& r : rows) {
    const double d = r.autoV.mean() - r.ours.mean();
    if (d < -0.1) ++improved;
    else if (d > 0.1) ++worse;
    else ++same;
    if (d > 0.5) ++worseBig;
    if (d > 0.05) ++worseStrict;
    if (r.align.ncc < 0.5) ++lowNcc;
    minScale = std::min<double>(minScale, r.align.scale);
    maxScale = std::max<double>(maxScale, r.align.scale);
    minNcc = std::min<double>(minNcc, r.align.ncc);
    meanNcc0 += r.align.ncc0 / n;
    meanNcc += r.align.ncc / n;
  }
  sum << "Dapple eval: " << rows.size() << " images, render " << edge << " px, compare " << cmpEdge << " px"
      << (matchOff ? ", colour match profile OFF" : "") << "\n\n";
  sum << "Framing: the camera JPEG is registered to our render per image (zoom + radial term + shift; the in-camera\n"
         "lens corrections change the framing). High-passed luma NCC: identity mapping mean " << f2(meanNcc0, 3)
      << " -> registered mean " << f2(meanNcc, 3) << " (min " << f2(minNcc, 3) << "; " << lowNcc
      << " image(s) < 0.5); camera zoom " << f2(minScale, 3) << " .. " << f2(maxScale, 3) << "\n\n";
  sum << "                         untouched     auto\n";
  sum << "dE2000 mean (per image)  " << f2(mean([](const Row& r) { return r.ours.mean(); }), 3) << "         "
      << f2(mean([](const Row& r) { return r.autoV.mean(); }), 3) << "\n";
  sum << "dE2000 p95  (per image)  " << f2(mean([](const Row& r) { return r.ours.pct(0.95); }), 3) << "         "
      << f2(mean([](const Row& r) { return r.autoV.pct(0.95); }), 3) << "\n";
  sum << "dE2000 mean (pooled)     " << f2(poolOurs.mean(), 3) << "         " << f2(poolAuto.mean(), 3) << "\n";
  sum << "dE2000 p95  (pooled)     " << f2(poolOurs.pct(0.95), 3) << "         " << f2(poolAuto.pct(0.95), 3) << "\n";
  sum << "mean dL (ours - cam)     " << f2(mean([](const Row& r) { return r.ours.dL(); }), 3) << "         "
      << f2(mean([](const Row& r) { return r.autoV.dL(); }), 3) << "\n";
  sum << "mean |dL| per image      " << f2(mean([](const Row& r) { return std::fabs(r.ours.dL()); }), 3) << "         "
      << f2(mean([](const Row& r) { return std::fabs(r.autoV.dL()); }), 3) << "\n";
  sum << "neutrals: mean a*/b* error (ours - cam)  untouched " << f2(poolOurs.nDa / std::max(1L, poolOurs.nNeutral), 2)
      << " / " << f2(poolOurs.nDb / std::max(1L, poolOurs.nNeutral), 2) << "  auto " << f2(poolAuto.nDa / std::max(1L, poolAuto.nNeutral), 2)
      << " / " << f2(poolAuto.nDb / std::max(1L, poolAuto.nNeutral), 2) << "  (" << f2(100.0 * poolOurs.nNeutral / std::max(1L, poolOurs.n), 1) << "% of pixels)\n";
  sum << "chroma ratio (pooled)    " << f2(poolOurs.chromaRatio(), 3) << "         " << f2(poolAuto.chromaRatio(), 3)
      << "\n\n";
  sum << "auto vs untouched, per image dE: improved(>0.1) " << improved << ", same " << same << ", worse(>0.1) "
      << worse << " (worse by >0.5: " << worseBig << "); not worse (within +0.1 dE, far below a just-noticeable difference) = " << f2(100.0 * (improved + same) / n, 1) << "%; strict, within +0.05 dE = " << f2(100.0 * (rows.size() - worseStrict) / n, 1) << "%\n\n";
  sum << "Hue error and chroma ratio per hue sector (pooled, camera chroma >= 10; hue error = ours - camera, degrees,\n"
         "positive = counter-clockwise in CIELAB a*b*)\n";
  sum << "sector     share%   hue err (untouched / auto)   |hue err|   chroma ratio (untouched / auto)   mean dE\n";
  long totalSec = 0;
  for (int s = 0; s < kNS; ++s) totalSec += poolOurs.sec[s].n;
  std::ofstream sec(fromU8(out + "/eval_sectors.csv"));
  sec << "sector,share_pct,hue_err_ours,hue_err_auto,abs_hue_err_ours,abs_hue_err_auto,chroma_ratio_ours,"
         "chroma_ratio_auto,dL_ours,dE_ours,dE_auto\n";
  for (int s = 0; s < kNS; ++s) {
    const SectorAcc &o = poolOurs.sec[s], &a = poolAuto.sec[s];
    if (!o.n) continue;
    char line[256];
    std::snprintf(line, sizeof line, "%-9s %6.1f   %+6.2f / %+6.2f            %5.2f       %5.3f / %5.3f               %5.2f\n",
                  sectorName(s), 100.0 * o.n / std::max(1L, totalSec), o.hueSum / o.n, a.hueSum / a.n, o.hueAbs / o.n,
                  o.cOurs / o.cCam, a.cOurs / a.cCam, o.dE / o.n);
    sum << line;
    sec << sectorName(s) << "," << f2(100.0 * o.n / std::max(1L, totalSec), 2) << "," << f2(o.hueSum / o.n, 3) << ","
        << f2(a.hueSum / a.n, 3) << "," << f2(o.hueAbs / o.n, 3) << "," << f2(a.hueAbs / a.n, 3) << ","
        << f2(o.cOurs / o.cCam, 4) << "," << f2(a.cOurs / a.cCam, 4) << "," << f2(o.dL / o.n, 3) << ","
        << f2(o.dE / o.n, 3) << "," << f2(a.dE / a.n, 3) << "\n";
  }
  // Worst images for the untouched render and for auto-vs-untouched.
  std::vector<const Row*> byWorse;
  for (const Row& r : rows) byWorse.push_back(&r);
  std::sort(byWorse.begin(), byWorse.end(), [](const Row* a, const Row* b) {
    return (a->autoV.mean() - a->ours.mean()) > (b->autoV.mean() - b->ours.mean());
  });
  sum << "\nImages where auto hurts most (index, file, dE untouched -> auto, auto changes):\n";
  for (std::size_t i = 0; i < std::min<std::size_t>(5, byWorse.size()); ++i) {
    const Row& r = *byWorse[i];
    sum << "  #" << r.index << " " << r.name << "  " << f2(r.ours.mean()) << " -> " << f2(r.autoV.mean()) << "  "
        << r.notes << "\n";
  }
  std::map<std::string, std::pair<int, double>> byClass;
  for (const Row& r : rows) {
    byClass[r.tone.nearest].first++;
    byClass[r.tone.nearest].second += r.autoV.mean() - r.ours.mean();
  }
  sum << "\nAuto effect by tonal class (images, mean dE change auto-untouched):\n";
  for (const auto& [k, v] : byClass) sum << "  " << k << ": " << v.first << ", " << f2(v.second / v.first, 3) << "\n";

  std::fputs(sum.str().c_str(), stdout);
  std::ofstream(fromU8(out + "/summary.txt")) << sum.str();
  std::printf("\nwrote %s/eval.csv, eval_sectors.csv, summary.txt, sheet_*.jpg\n", out.c_str());
  return 0;
}

}  // namespace dapple::evaltool
