// dapple film | film-pack | film-bench | film-compare | film-info  (spectral film simulation tools)

#include "film_cmds.h"
#include "film_common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

#include "colour_metrics.h"
#include "dapple/camera.h"
#include "dapple/developer.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/auto_adjust.h"
#include "dapple/renderer.h"
#include "film_internal.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::film;

namespace dapple::filmtool {


std::string cpuName() {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned r[12];
  unsigned a = 0, b = 0, c = 0, d = 0;
  __get_cpuid(0x80000000u, &a, &b, &c, &d);
  if (a >= 0x80000004u) {
    for (unsigned i = 0; i < 3; ++i) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    std::string s(reinterpret_cast<const char*>(r), 48);
    s.erase(s.find_last_not_of(std::string(1, '\0') + " ") + 1);
    return s;
  }
#endif
  return "unknown CPU";
}

// ---- image IO ------------------------------------------------------------------------------------------

bool readPfm(const fs::path& path, ImageF& img, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path.string();
    return false;
  }
  std::string magic;
  int w = 0, h = 0;
  double scale = 0;
  f >> magic >> w >> h >> scale;
  f.get();
  if (magic != "PF" || w <= 0 || h <= 0) {
    err = "not a 3-channel PFM: " + path.string();
    return false;
  }
  img = ImageF(w, h);
  std::vector<float> row(std::size_t(w) * 3);
  for (int y = h - 1; y >= 0; --y) {
    f.read(reinterpret_cast<char*>(row.data()), std::streamsize(row.size() * 4));
    if (!f) {
      err = "truncated PFM";
      return false;
    }
    if (scale > 0) {  // big endian
      for (float& v : row) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        u = (u >> 24) | ((u >> 8) & 0xFF00u) | ((u << 8) & 0xFF0000u) | (u << 24);
        std::memcpy(&v, &u, 4);
      }
    }
    std::memcpy(img.row(y), row.data(), row.size() * 4);
  }
  return true;
}

bool writePfm(const fs::path& path, const ImageF& img) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "PF\n" << img.width << " " << img.height << "\n-1.0\n";
  for (int y = img.height - 1; y >= 0; --y)
    f.write(reinterpret_cast<const char*>(img.row(y)), std::streamsize(std::size_t(img.width) * 3 * 4));
  return bool(f);
}

std::uint32_t crc32(const unsigned char* d, std::size_t n, std::uint32_t crc = 0) {
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// 16-bit RGB PNG with stored (uncompressed) deflate blocks, tagged sRGB.
bool writePng16(const fs::path& path, const ImageF& img) {
  const int w = img.width, h = img.height;
  std::vector<unsigned char> raw;
  raw.reserve((std::size_t(w) * 6 + 1) * h);
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    const float* r = img.row(y);
    for (int x = 0; x < w * 3; ++x) {
      const float v = std::min(1.f, std::max(0.f, r[x]));
      const unsigned q = unsigned(v * 65535.f + 0.5f);
      raw.push_back(static_cast<unsigned char>(q >> 8));
      raw.push_back(static_cast<unsigned char>(q & 0xFF));
    }
  }
  std::vector<unsigned char> z;
  z.push_back(0x78);
  z.push_back(0x01);
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF);
    z.push_back(n >> 8);
    z.push_back(~n & 0xFF);
    z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  std::uint32_t a = 1, b = 0;
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  const std::uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((ad >> s) & 0xFF);
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  auto be32 = [](std::uint32_t v) {
    return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
  };
  auto chunk = [&](const char* type, const std::vector<unsigned char>& data) {
    f << be32(std::uint32_t(data.size()));
    std::vector<unsigned char> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    f.write(reinterpret_cast<const char*>(td.data()), std::streamsize(td.size()));
    f << be32(crc32(td.data(), td.size()));
  };
  const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  f.write(reinterpret_cast<const char*>(sig), 8);
  std::vector<unsigned char> ihdr;
  for (std::uint32_t v : {std::uint32_t(w), std::uint32_t(h)})
    for (int s = 24; s >= 0; s -= 8) ihdr.push_back((v >> s) & 0xFF);
  ihdr.insert(ihdr.end(), {16, 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("sRGB", {0});
  chunk("IDAT", z);
  chunk("IEND", {});
  return bool(f);
}

// DAPPLE_FILM_PACK, else core/data/film/dapple.dfp relative to the cwd or a few parents; "" when none exists.
std::string findDefaultPack() {
  if (const char* env = std::getenv("DAPPLE_FILM_PACK"))
    if (*env) return env;
  for (const char* c : {"core/data/film/dapple.dfp", "../core/data/film/dapple.dfp", "../../core/data/film/dapple.dfp",
                        "../../../core/data/film/dapple.dfp"})
    if (fs::exists(P(c))) return c;
  return {};
}

std::shared_ptr<FilmLibrary> loadLibrary(const std::string& pack, std::string& err) {
  const std::string path = pack.empty() ? findDefaultPack() : pack;
  if (path.empty()) {
    err = "no film pack: pass --pack <file.dfp|profiles dir> or set DAPPLE_FILM_PACK";
    return nullptr;
  }
  if (fs::is_directory(P(path))) return FilmLibrary::loadJsonDir(P(path), &err);
  return FilmLibrary::loadPack(P(path), &err);
}

bool loadInput(const std::string& path, ImageF& img, int maxEdge, std::string& err, std::string* make, std::string* model) {
  const fs::path p = P(path);
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (ext == ".pfm") {
    if (!readPfm(p, img, err)) return false;
  } else {
    const Developer dev;
    const ImageDecoder* decoder = dev.decoders().find(p);
    if (!decoder) {
      err = "unsupported input file";
      return false;
    }
    auto src = decoder->decode(p, DecodeOptions{DecodeQuality::Full});
    if (!src) {
      err = src.error().message;
      return false;
    }
    img = std::move(src->linear);
    if (make) *make = src->meta.make;
    if (model) *model = src->meta.model;
  }
  if (maxEdge > 0) img = resizeToLongEdge(img, maxEdge);
  return true;
}

bool readText(const std::string& path, std::string& out) {
  std::ifstream f(P(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

FilmParams paramsFromArgs(const Args& a, std::string& err) {
  FilmParams p;
  p.camera.autoExposure = false;  // harness contract: off unless --auto-exposure on
  if (a.has("--film")) p.film = a.get("--film");
  if (a.has("--paper")) p.paper = a.get("--paper");
  if (a.has("--exposure-ev")) p.camera.exposureCompensationEv = std::atof(a.get("--exposure-ev").c_str());
  if (a.has("--auto-exposure")) p.camera.autoExposure = a.get("--auto-exposure") == "on";  // "match" is applied in cmdFilm
  if (a.has("--no-spatial")) p.disableSpatialAndStochastic();
  if (a.has("--seed")) p.settings.seed = std::uint32_t(std::strtoul(a.get("--seed").c_str(), nullptr, 0));
  if (a.has("--json-params")) {
    std::string text;
    if (!readText(a.get("--json-params"), text)) {
      err = "cannot read " + a.get("--json-params");
      return p;
    }
    if (!p.applyJson(text, &err)) return p;
  }
  return p;
}

int cmdFilm(const std::vector<std::string>& args) {
  const Args a = parse(args);
  if (!a.has("--in") && !a.has("--out") && a.has("--dump-params")) {  // contract: dump only, no render
    std::string e;
    FilmParams p = paramsFromArgs(a, e);
    auto lib = e.empty() ? loadLibrary(a.get("--pack"), e) : nullptr;
    FilmSimulator sim(lib);
    if (!lib || !sim.setParams(p, &e)) {
      std::fprintf(stderr, "film: %s\n", e.c_str());
      return 2;
    }
    std::ofstream f(P(a.get("--dump-params")));
    f << sim.effectiveParams().toJson() << "\n";
    return 0;
  }
  if (!a.has("--in") || !a.has("--out")) {
    std::fputs(
        "usage: dapple film --in x.pfm|x.ARW --out y.png [--out-pfm y.pfm] --film NAME --paper NAME|none\n"
        "       [--exposure-ev F] [--auto-exposure on|off] [--no-spatial] [--json-params f.json]\n"
        "       [--pack file.dfp|profiles_dir] [--max-edge N] [--dump-params f.json] [--timings]\n",
        stderr);
    return 2;
  }
  std::string err;
  FilmParams p = paramsFromArgs(a, err);
  if (!err.empty()) {
    std::fprintf(stderr, "film: %s\n", err.c_str());
    return 2;
  }
  auto lib = loadLibrary(a.get("--pack"), err);
  if (!lib) {
    std::fprintf(stderr, "film: %s\n", err.c_str());
    return 1;
  }
  // --neutral-from <film>: decision material, the print filter pack solved on that stock is used unchanged for this one (a single
  // shared pack per paper instead of a neutral solve per stock)
  if (a.has("--neutral-from")) {
    FilmSimulator refSim(lib);
    FilmParams rp = p;
    rp.film = a.get("--neutral-from");
    std::string re;
    if (!refSim.setParams(rp, &re)) {
      std::fprintf(stderr, "film: --neutral-from: %s\n", re.c_str());
      return 2;
    }
    p.settings.neutralPrintFilters = false;
    p.enlarger.cFilterNeutral = refSim.effectiveParams().enlarger.cFilterNeutral;
    p.enlarger.mFilterNeutral = refSim.effectiveParams().enlarger.mFilterNeutral;
    p.enlarger.yFilterNeutral = refSim.effectiveParams().enlarger.yFilterNeutral;
  }
  // the develop pipeline's paper-to-screen mapping (white / black correction) goes with the automatic exposure
  if (a.get("--auto-exposure", "match") == "match") applyFilmPaperMapping(p, std::strtof(a.get("--black", "0").c_str(), nullptr));
  FilmSimulator sim(lib);
  FilmTimings bt;
  if (!sim.setParams(p, &err, &bt)) {
    std::fprintf(stderr, "film: %s\n", err.c_str());
    return 2;
  }
  if (a.has("--dump-params")) {
    std::ofstream f(P(a.get("--dump-params")));
    f << sim.effectiveParams().toJson() << "\n";
    if (!a.has("--in")) return 0;
  }
  ImageF in, out;
  const auto tl = Clock::now();
  std::string metaMake, metaModel;
  if (!loadInput(a.get("--in"), in, std::atoi(a.get("--max-edge", "0").c_str()), err, &metaMake, &metaModel)) {
    std::fprintf(stderr, "film: %s\n", err.c_str());
    return 1;
  }
  const double loadMs = msSince(tl);
  const std::string aeMode = a.get("--auto-exposure", "match");
  float matchEv = 0.f, matchPrintEv = 0.f;
  float matchGain[3] = {1.f, 1.f, 1.f};
  FilmMatchInfo mi;
  if (aeMode == "match") {
    // Film exposure that keeps the photo's key (film_match.h): the develop pipeline's render (automatic correction, as
    // `dapple develop`) of the 256 px proxy sets the target median L*, the film is exposed until its median equals it.
    const Developer dev;
    const CameraProfile& cam = dev.cameras().resolve(metaMake, metaModel);
    SourceImage proxy;
    proxy.sceneReferred = true;
    proxy.linear = resizeToLongEdge(in, 256);
    SourceImage full;
    full.sceneReferred = true;
    full.linear = in.width > 1024 || in.height > 1024 ? resizeToLongEdge(in, 1024) : in;
    EditParams ep;
    ep += computeAutoAdjust(full, cam, AutoOptions{}).correction;
    Renderer ref(ep, cam, nullptr);
    ref.setToneProxy(&proxy);
    const float target = medianLightness(ref.renderFloat(proxy, false));
    FilmParams q = p;
    q.disableSpatialAndStochastic();
    FilmSimulator probe(lib);
    std::string e2;
    if (!probe.setParams(q, &e2)) {
      std::fprintf(stderr, "film: %s\n", e2.c_str());
      return 2;
    }
    ImageF po;
    auto filmL = [&](float ev) {
      // the exposure goes in as an input gain, as the app does: the model's exposureCompensationEv is compensated again by
      // the print exposure (printExposureCompensation) and would leave the print lightness unchanged
      const float g = std::exp2(ev);
      const float gain3[3] = {g, g, g};
      if (!probe.render(proxy.linear, po, nullptr, gain3)) return 0.f;
      return medianLightness(po);
    };
    const FilmMatchOptions mopt = FilmMatchOptions::fromEnv();
    const float unity[3] = {1.f, 1.f, 1.f};  // the film sees the source as it is (gain 1)
    const SceneStats stats = sceneLumaStats(proxy.linear, unity);
    if (mopt.mode == FilmMatchMode::Pull || mopt.mode == FilmMatchMode::Full) {  // v2 / v3: one EV on the film
      solveFilmKeyEv(filmL, target, &mi);
      matchEv = resolveFilmKeyEv(lib->find(p.film), stats, mi.ev, mopt, &mi);
    } else if (mopt.mode == FilmMatchMode::Window) {
      // v4. A: film exposure from the stock's latitude; B: print exposure to the scene's key, limited by the paper
      matchEv = chooseFilmEv(lib->find(p.film), stats, mopt, &mi);
      mi.targetL = target;
      const float gA = std::exp2(matchEv);
      const float gainA[3] = {gA, gA, gA};
      auto printL = [&](float pev) {
        FilmParams qq = q;
        qq.enlarger.printExposure = std::exp2(-double(pev));
        std::string e3;
        if (!probe.setParams(qq, &e3) || !probe.render(proxy.linear, po, nullptr, gainA)) return 0.f;
        return medianLightness(po);
      };
      const FilmPrintRange prange = filmPrintRange(lib, p.film, p.paper);
      matchPrintEv = solvePrintEv(printL, target, prange, &mi);
      if (std::getenv("DAPPLE_MATCH_TRACE"))
        std::fprintf(stderr, "paper range: white L* %.1f black L* %.1f usable %.1f..%.1f maxPrintEv %.2f\n", prange.lWhite, prange.lBlack, prange.loL, prange.hiL, prange.maxPrintEv);
      FilmParams pf = p;
      pf.enlarger.printExposure = std::exp2(-double(matchPrintEv));
      if (!sim.setParams(pf, &err)) {
        std::fprintf(stderr, "film: %s\n", err.c_str());
        return 2;
      }
    } else {
      // v5: one exposure decision per image, shared by all stocks, per-stock usability rescue, print constant (film_match.h)
      EditParams ep0 = ep;
      ep0.exposure = 0.f;
      Renderer ref0(ep0, cam, nullptr);
      ref0.setToneProxy(&proxy);
      MeterInputs mi_in;
      mi_in.lib = lib;
      mi_in.film = p.film;
      mi_in.paper = p.paper;
      mi_in.st0 = mi_in.stFull = stats;
      mi_in.keyL0 = medianLightness(ref0.renderFloat(proxy, false));
      mi_in.shoot = std::atof(a.get("--shoot", "0").c_str());
      mi_in.print = std::atof(a.get("--print", "0").c_str());
      mi_in.latitude = std::atof(a.get("--latitude", "0").c_str());
      mi_in.negativeBiasEv = mopt.negativeBiasEv;
      mi_in.medianL = [&](float fev, float pev) {
        FilmParams qq = q;
        qq.enlarger.printExposure = std::exp2(-double(pev));
        const float g = std::exp2(fev);
        const float gain3[3] = {g, g, g};
        std::string e3;
        if (!probe.setParams(qq, &e3) || !probe.render(proxy.linear, po, nullptr, gain3)) return 0.f;
        return medianLightness(po);
      };
      const FilmExposure fe = solveMeter(mi_in, &mi);
      matchEv = fe.filmEv;
      matchPrintEv = fe.printEv;
      mi.targetL = target;
      if (a.has("--pad-json")) {
        const FilmPadInfo pad = filmPadInfo(mi_in);
        std::ofstream f(P(a.get("--pad-json")));
        f << "{\"shoot_ev\":[" << pad.shootEv[0] << "," << pad.shootEv[1] << "," << pad.shootEv[2] << "],\"print_l\":[" << pad.printL[0] << ","
          << pad.printL[1] << "," << pad.printL[2] << "],\"black_l\":[" << pad.blackL[0] << "," << pad.blackL[1] << "," << pad.blackL[2]
          << "],\"latitude\":[" << pad.latitude[0] << "," << pad.latitude[1] << "," << pad.latitude[2] << "],\"shared_ev\":" << pad.sharedEv
          << ",\"final_ev\":" << pad.finalEv << "}\n";
      }
      FilmParams pf = p;
      pf.enlarger.printExposure = std::exp2(-double(matchPrintEv));
      if (!sim.setParams(pf, &err)) {
        std::fprintf(stderr, "film: %s\n", err.c_str());
        return 2;
      }
    }
    matchGain[0] = matchGain[1] = matchGain[2] = std::exp2(matchEv);
  }
  FilmTimings rt;
  if (!sim.render(in, out, &rt, aeMode == "match" ? matchGain : nullptr)) {
    std::fputs("film: render failed\n", stderr);
    return 1;
  }
  const auto tw = Clock::now();
  if (!writePng16(P(a.get("--out")), out)) {
    std::fputs("film: cannot write output\n", stderr);
    return 1;
  }
  if (a.has("--out-pfm") && !writePfm(P(a.get("--out-pfm")), out)) {
    std::fputs("film: cannot write PFM\n", stderr);
    return 1;
  }
  const double writeMs = msSince(tw);
  const FilmParams& e = sim.effectiveParams();
  std::printf("%s -> %s (%dx%d)  %s + %s  neutral C/M/Y %.2f/%.2f/%.2f  auto-EV %+.2f  kernel %s\n", a.get("--in").c_str(),
              a.get("--out").c_str(), in.width, in.height, p.film.c_str(), p.paper.c_str(), e.enlarger.cFilterNeutral,
              e.enlarger.mFilterNeutral, e.enlarger.yFilterNeutral, rt.autoExposureEv, sim.kernelName());
  if (aeMode == "match") {
    const auto bnd = [](FilmBound b) { return b == FilmBound::Toe ? "toe" : b == FilmBound::Shoulder ? "shoulder" : b == FilmBound::Fallback ? "fallback" : "-"; };
    const auto pbnd = [](PrintBound b) { return b == PrintBound::PaperWhite ? "paper white" : b == PrintBound::PaperBlack ? "paper black" : b == PrintBound::SolverLimit ? "solver limit" : "-"; };
    const FilmMatchMode rmode = FilmMatchOptions::fromEnv().mode;
    if (rmode == FilmMatchMode::Meter)
      std::printf("match: EV_scene %+.2f (key L* %.1f, meter %.4f -> %+.2f)  film EV shared %+.2f final %+.2f  in-range %.0f%% -> %.0f%% (below toe %.0f%%, above shoulder %.0f%%; %.2f / %.2f st)  print %+.2f (const %+.2f)  scene Y p2/p50/hi %.4f/%.3f/%.3f clipped %.1f%%\n",
                  mi.evScene, mi.keyL0, double(mi.meterY), mi.evMeter, mi.sharedEv, matchEv, 100.0 * mi.fracShared, 100.0 * mi.fracFinal,
                  100.0 * mi.fracBelowToe, 100.0 * mi.fracAboveShoulder, mi.stopsBelowToe, mi.stopsAboveShoulder, matchPrintEv, mi.printConst,
                  double(mi.p2), double(mi.p50), double(mi.p995), 100.0 * double(mi.clipFrac));
    else if (rmode == FilmMatchMode::Window)
      std::printf("match: film EV %+.2f (window [%+.2f, %+.2f], bound %s)  print EV %+.2f (key L* %.1f -> %.1f, paper range %.1f..%.1f, bound %s)  scene Y p2/p50/hi %.4f/%.3f/%.3f clipped %.1f%%  develop median L* %.1f  (%d print evals)\n",
                  matchEv, mi.evLo, mi.evHi, bnd(mi.bound), matchPrintEv, mi.targetL, mi.printL, mi.paperLoL, mi.paperHiL, pbnd(mi.printBound),
                  double(mi.p2), double(mi.p50), double(mi.p995), 100.0 * double(mi.clipFrac), mi.targetL, mi.printEvals);
    else
      std::printf("match: key EV %+.2f  (full match %+.2f, pulled %+.2f, window %s[%+.2f, %+.2f] bound %s)  develop median L* %.1f  film at 0 EV %.1f  (%d evals)\n",
                  matchEv, mi.evMatch, mi.evKey, mi.latitudeOk ? "" : "none ", mi.evLo, mi.evHi, bnd(mi.bound), mi.targetL, mi.filmL0, mi.evals);
  }
  if (a.has("--timings"))
    std::printf("timings ms: bake %.1f (raw %.1f curves %.1f neutral %.1f lut %.1f)  auto-exp %.1f  run %.1f  load %.0f  write %.0f\n",
                bt.bakeMs, bt.rawLutMs, bt.curvesMs, bt.neutralMs, bt.lutMs, rt.autoExposureMs, rt.runMs, loadMs, writeMs);
  return 0;
}

int cmdPack(const std::vector<std::string>& args) {
  const Args a = parse(args);
  if (a.pos.size() != 2) {
    std::fputs("usage: dapple film-pack <profiles_dir> <out.dfp> [--check]\n", stderr);
    return 2;
  }
  std::string err;
  const auto t0 = Clock::now();
  auto lib = FilmLibrary::loadJsonDir(P(a.pos[0]), &err);
  if (!lib) {
    std::fprintf(stderr, "film-pack: %s\n", err.c_str());
    return 1;
  }
  const double genMs = msSince(t0);
  if (!lib->writePack(P(a.pos[1]), &err)) {
    std::fprintf(stderr, "film-pack: %s\n", err.c_str());
    return 1;
  }
  std::uintmax_t jsonBytes = 0;
  for (const auto& e : fs::directory_iterator(P(a.pos[0])))
    if (e.path().extension() == ".json") jsonBytes += e.file_size();
  const auto packBytes = fs::file_size(P(a.pos[1]));
  std::printf("%zu stocks, upsample table %dx%d, cmax %dx%d -> %s: %.2f MB (profiles JSON %.2f MB), build %.0f ms\n",
              lib->names().size(), lib->upsample().n, lib->upsample().n, lib->cmax().nJ, lib->cmax().nH, a.pos[1].c_str(),
              double(packBytes) / 1e6, double(jsonBytes) / 1e6, genMs);
  if (a.has("--check")) {
    auto back = FilmLibrary::loadPack(P(a.pos[1]), &err);
    if (!back || back->names() != lib->names()) {
      std::fprintf(stderr, "film-pack: reload check failed: %s\n", err.c_str());
      return 1;
    }
    std::puts("reload check ok");
  }
  return 0;
}

int cmdInfo(const std::vector<std::string>& args) {
  const Args a = parse(args);
  std::string err;
  auto lib = loadLibrary(a.get("--pack"), err);
  if (!lib) {
    std::fprintf(stderr, "film-info: %s\n", err.c_str());
    return 1;
  }
  for (const auto& n : lib->names()) {
    const FilmStock* s = lib->find(n);
    std::printf("%-34s %-8s %-5s %-8s ref %-7s view %s\n", n.c_str(), s->type.c_str(), s->support.c_str(),
                s->stage.c_str(), s->referenceIlluminant.c_str(), s->viewingIlluminant.c_str());
  }
  std::printf("\n%s\n", lib->attribution().c_str());
  return 0;
}

// Area-average downscale by an integer factor in linear light (inputs are sRGB-encoded floats).
ImageF boxDown(const ImageF& s, int f) {
  const int w = s.width / f, h = s.height / f;
  ImageF o(w, h);
  auto dec = [](double v) { v = std::clamp(v, 0.0, 1.0); return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  auto enc = [](double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) {
        double acc = 0;
        for (int j = 0; j < f; ++j)
          for (int i = 0; i < f; ++i) acc += dec(s.data[(std::size_t(y * f + j) * s.width + x * f + i) * 3 + c]);
        o.data[(std::size_t(y) * w + x) * 3 + c] = float(enc(acc / (f * f)));
      }
  return o;
}

int cmdCompare(const std::vector<std::string>& args) {
  const Args a = parse(args);
  if (a.pos.size() != 2) {
    std::fputs("usage: dapple film-compare <a.pfm> <b.pfm>   (sRGB-encoded float, CIEDE2000 on clamped values)\n", stderr);
    return 2;
  }
  ImageF x, y;
  std::string err;
  if (!readPfm(P(a.pos[0]), x, err) || !readPfm(P(a.pos[1]), y, err)) {
    std::fprintf(stderr, "film-compare: %s\n", err.c_str());
    return 1;
  }
  if (x.width != y.width || x.height != y.height) {
    std::fputs("film-compare: size mismatch\n", stderr);
    return 1;
  }
  const int down = std::atoi(a.get("--down", "1").c_str());
  if (down > 1) {
    x = boxDown(x, down);
    y = boxDown(y, down);
  }
  const std::size_t n = std::size_t(x.width) * x.height;
  std::vector<double> de(n);
  double mean = 0, dL = 0, dC = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto la = metrics::srgbToLab(&x.data[i * 3]), lb = metrics::srgbToLab(&y.data[i * 3]);
    de[i] = metrics::deltaE2000(la, lb);
    mean += de[i];
    dL += lb.L - la.L;
    dC += lb.C() - la.C();
  }
  mean /= n;
  std::vector<double> s = de;
  std::sort(s.begin(), s.end());
  auto pct = [&](double q) { return s[std::min(n - 1, std::size_t(q * double(n - 1) + 0.5))]; };
  std::printf("dE2000 mean %.3f  median %.3f  p95 %.3f  p99 %.3f  max %.3f   (mean dL %+.3f, mean dC %+.3f; a=%s b=%s)\n", mean,
              pct(0.5), pct(0.95), pct(0.99), s.back(), dL / n, dC / n, a.pos[0].c_str(), a.pos[1].c_str());
  return 0;
}

// Grain RMS in flat mid-grey patches: std of (full - base) after removing the patch mean, in 8-bit levels (encoded).
// "Flat" is decided on the base (no spatial effects) render of the same pipeline.
int cmdGrainRms(const std::vector<std::string>& args) {
  const Args a = parse(args);
  if (a.pos.size() != 2) {
    std::fputs("usage: dapple film-grain <full.pfm> <base_nospatial.pfm> [--patch 32] [--lo 0.35] [--hi 0.65] [--flat 0.004]\n", stderr);
    return 2;
  }
  ImageF f, b;
  std::string err;
  if (!readPfm(P(a.pos[0]), f, err) || !readPfm(P(a.pos[1]), b, err) || f.width != b.width || f.height != b.height) {
    std::fprintf(stderr, "film-grain: %s\n", err.empty() ? "size mismatch" : err.c_str());
    return 1;
  }
  const int ps = std::atoi(a.get("--patch", "32").c_str());
  const double lo = std::atof(a.get("--lo", "0.35").c_str()), hi = std::atof(a.get("--hi", "0.65").c_str()),
               flat = std::atof(a.get("--flat", "0.004").c_str());
  double var[3] = {0, 0, 0}, varL = 0;
  int count = 0;
  for (int y0 = 0; y0 + ps <= f.height; y0 += ps)
    for (int x0 = 0; x0 + ps <= f.width; x0 += ps) {
      double mb = 0, sb = 0;
      const double n = double(ps) * ps;
      for (int y = 0; y < ps; ++y)
        for (int x = 0; x < ps; ++x) {
          const float* p = &b.data[(std::size_t(y0 + y) * b.width + x0 + x) * 3];
          const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
          mb += l;
          sb += l * l;
        }
      mb /= n;
      const double sd = std::sqrt(std::max(0.0, sb / n - mb * mb));
      if (mb < lo || mb > hi || sd > flat) continue;
      double m[3] = {0, 0, 0}, q[3] = {0, 0, 0}, ml = 0, ql = 0;
      for (int y = 0; y < ps; ++y)
        for (int x = 0; x < ps; ++x) {
          const std::size_t i = (std::size_t(y0 + y) * f.width + x0 + x) * 3;
          double dl = 0;
          const double w[3] = {0.2126, 0.7152, 0.0722};
          for (int c = 0; c < 3; ++c) {
            const double d = double(f.data[i + c]) - b.data[i + c];
            m[c] += d;
            q[c] += d * d;
            dl += w[c] * d;
          }
          ml += dl;
          ql += dl * dl;
        }
      for (int c = 0; c < 3; ++c) var[c] += q[c] / n - (m[c] / n) * (m[c] / n);
      varL += ql / n - (ml / n) * (ml / n);
      ++count;
    }
  if (!count) {
    std::puts("film-grain: no flat mid-grey patches found");
    return 1;
  }
  std::printf("grain RMS (8-bit levels, encoded) over %d flat mid-grey %dx%d patches: R %.3f  G %.3f  B %.3f  luma %.3f   (%s)\n",
              count, ps, ps, 255 * std::sqrt(var[0] / count), 255 * std::sqrt(var[1] / count), 255 * std::sqrt(var[2] / count),
              255 * std::sqrt(varL / count), a.pos[0].c_str());
  return 0;
}

// dapple film-upsm-gen [--n 192] [--out table.bin] [--verify pack.dfp]
// Regenerates the spectral-upsampling table from first principles (core/src/film_upsample.cpp: Jakob & Hanika 2019
// sigmoid-polynomial spectra, minimum-norm fit per chromaticity node, CIE 1931 2-degree observer) and reports a
// determinism hash, the chromaticity accuracy and (optionally) whether a pack contains exactly this table.
int cmdUpsmGen(const std::vector<std::string>& args) {
  const Args a = parse(args);
  const int n = std::atoi(a.get("--n", "192").c_str());
  if (n < 2 || n > 1024) {
    std::fputs("usage: dapple film-upsm-gen [--n 192] [--out table.bin] [--verify pack.dfp]\n", stderr);
    return 2;
  }
  const auto t0 = Clock::now();
  const UpsampleTable t = detail::generateUpsampleTable(n);
  const double ms = msSince(t0);
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a 64 over the float32 bytes
  for (const float f : t.coef) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int b = 0; b < 4; ++b) h = (h ^ ((u >> (8 * b)) & 0xffu)) * 1099511628211ull;
  }
  // spectral locus (CIE 1931, 380..700 nm, closed by the purple line): nodes outside it have no physical spectrum
  std::vector<std::array<double, 2>> locus;
  for (int l = 0; l <= 64; ++l) {
    const double X = detail::cmf(0)[l], Y = detail::cmf(1)[l], Z = detail::cmf(2)[l];
    locus.push_back({X / (X + Y + Z), Y / (X + Y + Z)});
  }
  auto inside = [&](double x, double y) {
    bool in = false;
    for (std::size_t k = 0, m = locus.size() - 1; k < locus.size(); m = k++)
      if ((locus[k][1] > y) != (locus[m][1] > y) &&
          x < (locus[m][0] - locus[k][0]) * (y - locus[k][1]) / (locus[m][1] - locus[k][1]) + locus[k][0])
        in = !in;
    return in;
  };
  double maxErr = 0, sumErr = 0, smin = 1e9, smax = -1e9, maxIn = 0, sumIn = 0;
  int bad = 0, nIn = 0, badIn = 0;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      const float* c = &t.coef[(std::size_t(j) * n + i) * 4];
      const double co[3] = {c[0], c[1], c[2]};
      detail::Spec s;
      detail::spectrumFromCoeffs(co, s);
      double X = 0, Y = 0, Z = 0;
      for (int l = 0; l < detail::kN; ++l) {
        X += s[l] * detail::cmf(0)[l];
        Y += s[l] * detail::cmf(1)[l];
        Z += s[l] * detail::cmf(2)[l];
        smin = std::min<double>(smin, s[l]);
        smax = std::max<double>(smax, s[l]);
      }
      double x, y;
      detail::tcToXy(double(i) / (n - 1), double(j) / (n - 1), x, y);
      const double e = std::hypot(X / (X + Y + Z) - x, Y / (X + Y + Z) - y);
      maxErr = std::max(maxErr, e);
      sumErr += e;
      if (e > 1e-4) ++bad;
      if (inside(x, y)) {
        ++nIn;
        maxIn = std::max(maxIn, e);
        sumIn += e;
        if (e > 1e-4) ++badIn;
      }
    }
  std::printf("upsample table %dx%d: %.0f ms, fnv1a64 %016llx, chromaticity error mean %.2e max %.2e, nodes >1e-4: %d (of %d), "
              "spectrum range %.3g..%.3g\n",
              n, n, ms, (unsigned long long)h, sumErr / (double(n) * n), maxErr, bad, n * n, smin, smax);
  std::printf("  nodes inside the spectral locus: %d, chromaticity error mean %.2e max %.2e, nodes >1e-4: %d; "
              "outside the locus (not reproducible, the engine clamps there): %d\n",
              nIn, nIn ? sumIn / nIn : 0.0, maxIn, badIn, n * n - nIn);
  if (a.has("--out")) {
    std::ofstream f(P(a.get("--out")), std::ios::binary);
    const std::uint32_t nn = std::uint32_t(n);
    f.write(reinterpret_cast<const char*>(&nn), 4);
    f.write(reinterpret_cast<const char*>(t.coef.data()), std::streamsize(t.coef.size() * 4));
    if (!f) {
      std::fputs("film-upsm-gen: cannot write output\n", stderr);
      return 1;
    }
  }
  if (a.has("--verify")) {
    std::string err;
    auto lib = FilmLibrary::loadPack(P(a.get("--verify")), &err);
    if (!lib) {
      std::fprintf(stderr, "film-upsm-gen: %s\n", err.c_str());
      return 1;
    }
    const bool same = lib->upsample().n == n && lib->upsample().coef == t.coef;
    std::printf("pack %s: upsampling table %s\n", a.get("--verify").c_str(), same ? "IDENTICAL to the generated table" : "DIFFERS");
    if (!same) return 1;
  }
  return 0;
}

void installDefaultLibrary() {
  const std::string path = findDefaultPack();
  if (!path.empty()) film::setLibraryPathLazy(P(path));
}

bool handles(const std::string& c) {
  return c == "film-render-bench" || c == "film" || c == "film-pack" || c == "film-upsm-gen" || c == "film-bench" || c == "film-compare" || c == "film-info" || c == "film-grain";
}

int run(const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd == "film") return cmdFilm(args);
  if (cmd == "film-pack") return cmdPack(args);
  if (cmd == "film-upsm-gen") return cmdUpsmGen(args);
  if (cmd == "film-bench") return cmdBench(args);
  if (cmd == "film-render-bench") return cmdRenderBench(args);
  if (cmd == "film-compare") return cmdCompare(args);
  if (cmd == "film-info") return cmdInfo(args);
  if (cmd == "film-grain") return cmdGrainRms(args);
  return 2;
}

}  // namespace dapple::filmtool
