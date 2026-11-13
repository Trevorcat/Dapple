// Batch base swatches (dapple_session_base_previews): equality with the per-base interactive frame, the final-quality render within a stated tolerance, modern and
// unknown ids, latest-wins cancellation, waiting for the interaction flag. `dapple_base_previews_tests <id>`; ids: equal cancel yield ids raw.
// The C API is compiled in (as in export_meta_tests). `raw` skips (exit 77) without build/rawsamples.
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "color.h"
#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"

namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

static fs::path root() {
#ifdef DAPPLE_SOURCE_DIR
  return fs::path(DAPPLE_SOURCE_DIR);
#else
  return fs::current_path();
#endif
}

// A colourful synthetic picture: hue sweep with a light ramp and a grey ramp strip.
static fs::path syntheticJpeg() {
  const int w = 640, h = 480;
  std::vector<unsigned char> px(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      unsigned char* p = &px[(std::size_t(y) * w + x) * 3];
      const double hue = 6.0 * x / w, v = 0.25 + 0.7 * (1.0 - double(y) / h);
      const double r = std::fabs(hue - 3) - 1, g = 2 - std::fabs(hue - 2), b = 2 - std::fabs(hue - 4);
      const auto c = [&](double t) { return (unsigned char)(255.0 * v * std::fmin(1.0, std::fmax(0.15, t))); };
      p[0] = c(r), p[1] = c(g), p[2] = c(b);
      if (y > h - 40) p[0] = p[1] = p[2] = (unsigned char)(255 * x / w);  // grey ramp
    }
  const fs::path dir = root() / "build" / "base-previews";
  fs::create_directories(dir);
  const fs::path f = dir / "synthetic.jpg";
  stbi_write_jpg(f.string().c_str(), w, h, 3, px.data(), 95);
  return f;
}

static bool loadPack() {
  const fs::path pack = root() / "core" / "data" / "film" / "dapple.dfp";
  if (!fs::exists(pack)) return false;
  return dapple_film_load_pack(pack.string().c_str()) == DAPPLE_OK;
}

static std::vector<std::string> filmIds(std::size_t want) {
  std::vector<std::string> ids;
  char* js = dapple_film_list_json();
  if (!js) return ids;
  const std::string j = js;
  dapple_free_string(js);
  std::size_t at = 0;
  while ((at = j.find("{\"id\":\"", at)) != std::string::npos) {
    const std::size_t e = j.find('"', at + 7), t = j.find("\"type\":\"", e);
    const std::string id = j.substr(at + 7, e - at - 7), type = j.substr(t + 8, j.find('"', t + 8) - t - 8);
    at = e;
    if (type == "negative" || type == "slide") ids.push_back(id);
  }
  std::vector<std::string> pick;
  for (std::size_t i = 0; i < ids.size() && pick.size() < want; i += std::max<std::size_t>(1, ids.size() / want)) pick.push_back(ids[i]);
  return pick;
}

struct Img {
  int w = 0, h = 0;
  std::vector<unsigned char> px;
};
static Img take(DappleImage& i) {
  Img r;
  r.w = i.width, r.h = i.height;
  if (i.rgba) r.px.assign(i.rgba, i.rgba + std::size_t(i.width) * i.height * 4);
  dapple_free_image(&i);
  return r;
}
static void diff(const Img& a, const Img& b, double& mean, int& mx) {
  mean = 1e9, mx = 255;
  if (a.w != b.w || a.h != b.h || a.px.empty() || a.px.size() != b.px.size()) return;
  double s = 0;
  mx = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.px.size(); ++i) {
    if (i % 4 == 3) continue;
    const int d = std::abs(int(a.px[i]) - int(b.px[i]));
    s += d, mx = std::max(mx, d), ++n;
  }
  mean = s / double(n);
}

static std::string withFilm(const std::string& base, const std::string& id) {
  return base.substr(0, base.size() - 1) + (base.size() > 2 ? "," : "") + "\"film\":\"" + id + "\"}";
}

// the per-base route at the same tier: one interactive background frame with the coarse-film active parameter, as base_previews renders it
static Img frameOf(DappleSession* s, const std::string& params, int longEdge) {
  DappleFrameRequest rq{};
  rq.size = sizeof rq;
  rq.long_edge = longEdge;
  rq.flags = DAPPLE_FRAME_INTERACTIVE | DAPPLE_FRAME_BACKGROUND;
  rq.bake_size = 33;
  rq.active_param = "film_swatch";
  DappleImage img{};
  if (dapple_session_render_frame(s, params.c_str(), nullptr, &rq, &img, nullptr) != DAPPLE_OK) return {};
  return take(img);
}

static void testEqual(DappleSession* s, const char* label) {
  const std::string params = "{\"exposure\":0.3,\"contrast\":10,\"film_grain\":0,\"film_halation\":0}";  // no grain / halation: the final render has them, the swatch tier does not
  std::vector<std::string> ids = {""};
  for (auto& f : filmIds(5)) ids.push_back(f);
  ids.push_back("modern_clean");
  ids.push_back("modern_vivid");
  ids.push_back("modern_soft");
  std::vector<const char*> cid;
  for (auto& i : ids) cid.push_back(i.c_str());
  std::vector<DappleImage> out(ids.size());
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(dapple_session_base_previews(s, params.c_str(), cid.data(), int(ids.size()), 96, out.data()) == DAPPLE_OK);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  [%s] %zu bases in %.0f ms (%.0f ms each, cold)\n", label, ids.size(), ms, ms / double(ids.size()));
  std::vector<Img> got;
  for (auto& o : out) got.push_back(take(o));
  {  // the same batch again (every table is cached now): the result does not depend on how warm the engine was
    std::vector<DappleImage> again(ids.size());
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(dapple_session_base_previews(s, params.c_str(), cid.data(), int(ids.size()), 96, again.data()) == DAPPLE_OK);
    std::printf("  [%s] warm batch %.0f ms\n", label, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
    for (std::size_t i = 0; i < ids.size(); ++i) {
      double m;
      int mx;
      diff(got[i], take(again[i]), m, mx);
      std::printf("    cold vs warm %-24s mean %.3f max %d\n", ids[i].c_str(), m, mx);
      CHECK(mx <= 2);
    }
  }
  frameOf(s, params, 96);  // the first frame of a session also reduces noise: not the one to compare with
  double worstMean = 0, worstFinal = 0;
  int worstMax = 0;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    CHECK(!got[i].px.empty() && std::max(got[i].w, got[i].h) == 96);
    const Img ref = frameOf(s, withFilm(params, ids[i]), 96);
    double m;
    int mx;
    diff(got[i], ref, m, mx);
    worstMean = std::max(worstMean, m), worstMax = std::max(worstMax, mx);
    // the final-quality render (what the old per-base route called): coarse tables and the interactive tier stay close
    DappleImage fin{};
    const std::string pf = withFilm(params, ids[i]);
    CHECK(dapple_session_render(s, pf.c_str(), nullptr, 96, &fin) == DAPPLE_OK);
    double mf;
    int xf;
    diff(got[i], take(fin), mf, xf);
    worstFinal = std::max(worstFinal, mf);
    std::printf("    %-24s vs frame mean %.3f max %d | vs final render mean %.2f max %d\n", ids[i].empty() ? "(standard)" : ids[i].c_str(), m, mx, mf, xf);
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {  // warm cost of one base alone (informational)
    DappleImage one{};
    const auto t2 = std::chrono::steady_clock::now();
    CHECK(dapple_session_base_previews(s, params.c_str(), &cid[i], 1, 96, &one) == DAPPLE_OK);
    std::printf("    warm single %-24s %.1f ms\n", ids[i].c_str(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count());
    dapple_free_image(&one);
  }
  // bases differ from each other
  double dm;
  int dx;
  diff(got[0], got[1], dm, dx);
  CHECK(dm > 1.0);
  diff(got[ids.size() - 3], got[ids.size() - 2], dm, dx);
  CHECK(dm > 0.5);
  CHECK(worstMax <= 2);      // same tier, same frame: identical up to the last rounding
  const double finalLimit = std::strcmp(label, "raw") == 0 ? 8.0 : 3.0;  // a real photo with film: the interactive tier skips the full spatial stage (diffusion, MTF), mean 6 - 7 levels, edges only
  CHECK(worstFinal <= finalLimit);  // against the final-quality render (exact tables, full spatial tier, no grain): mean absolute error in 8-bit levels
  std::printf("  worst: vs frame mean %.3f max %d, vs final mean %.2f (limits: max <= 2, mean <= %.1f)\n", worstMean, worstMax, worstFinal, finalLimit);
}

static void probeTiers(DappleSession* s) {
  const std::string p = "{\"exposure\":0.3,\"contrast\":10,\"film_grain\":0,\"film_halation\":0,\"film\":\"fujifilm_c200\"}";
  auto fr = [&](int flags, const char* act, int bake) {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = 96;
    rq.flags = flags;
    rq.bake_size = bake;
    rq.active_param = act;
    DappleImage img{};
    dapple_session_render_frame(s, p.c_str(), nullptr, &rq, &img, nullptr);
    return take(img);
  };
  fr(DAPPLE_FRAME_INTERACTIVE, nullptr, 0);
  DappleImage fin{};
  dapple_session_render(s, p.c_str(), nullptr, 96, &fin);
  const Img F = take(fin);
  const char* names[] = {"interactive coarse", "interactive exact", "final frame", "final frame bake 33"};
  const Img v[] = {fr(DAPPLE_FRAME_INTERACTIVE, "film_swatch", 33), fr(DAPPLE_FRAME_INTERACTIVE, nullptr, 0), fr(0, nullptr, 0), fr(0, nullptr, 33)};
  for (int i = 0; i < 4; ++i) {
    double m;
    int mx;
    diff(v[i], F, m, mx);
    std::printf("  tier %-22s vs dapple_session_render: mean %.2f max %d\n", names[i], m, mx);
  }
}

static void probeScaling(DappleSession* s) {
  {
    std::vector<std::string> ids = filmIds(100);
    std::vector<const char*> c;
    for (auto& i : ids) c.push_back(i.c_str());
    for (int round = 0; round < 3; ++round) {
      std::vector<DappleImage> o(ids.size());
      dapple_session_base_previews(s, "{\"exposure\":0.3}", c.data(), int(c.size()), 96, o.data());
      for (auto& i : o) dapple_free_image(&i);
    }
  }
  const char* id[] = {"fujifilm_c200"};
  for (int le : {24, 48, 96, 192, 384}) {
    DappleImage o{};
    dapple_session_base_previews(s, "{}", id, 1, le, &o);
    dapple_free_image(&o);
    const auto t = std::chrono::steady_clock::now();
    for (int k = 0; k < 3; ++k) {
      dapple_session_base_previews(s, "{}", id, 1, le, &o);
      dapple_free_image(&o);
    }
    std::printf("  probe long_edge %d: %.1f ms\n", le, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() / 3);
  }
}

static void testIds(DappleSession* s) {
  const std::string params = "{\"exposure\":0.2,\"film\":\"modern_soft\"}";  // a film key in the params is replaced by the id
  const char* ids[] = {"", "modern_vivid", "no_such_stock_xyz", nullptr};
  DappleImage out[4]{};
  CHECK(dapple_session_base_previews(s, params.c_str(), ids, 4, 80, out) == DAPPLE_OK);
  Img std1 = take(out[0]), mod = take(out[1]), unk = take(out[2]), nul = take(out[3]);
  CHECK(!std1.px.empty() && !mod.px.empty() && !unk.px.empty() && !nul.px.empty());
  double m;
  int mx;
  diff(unk, std1, m, mx);
  CHECK(m < 0.01);  // unknown -> standard
  diff(nul, std1, m, mx);
  CHECK(m < 0.01);  // NULL -> standard
  diff(mod, std1, m, mx);
  CHECK(m > 1.0);  // modern differs from standard
  const Img ref = frameOf(s, "{\"exposure\":0.2}", 80);
  diff(std1, ref, m, mx);
  CHECK(mx <= 2);
  // bad arguments
  DappleImage one{};
  CHECK(dapple_session_base_previews(nullptr, "{}", ids, 1, 96, &one) == DAPPLE_ERROR);
  CHECK(dapple_session_base_previews(s, "{bad", ids, 1, 96, &one) == DAPPLE_ERROR);
  CHECK(dapple_session_base_previews(s, "{}", ids, 0, 96, &one) == DAPPLE_ERROR);
}

static void testCancel(DappleSession* s) {
  const char* ids[] = {"modern_clean", "modern_vivid", "modern_soft", ""};
  // explicit cancel: the call waits for the finger, the cancel ends it before any frame
  dapple_set_interaction(1);
  std::vector<DappleImage> a(4);
  std::atomic<int> rcA{-1};
  std::thread ta([&] { rcA = dapple_session_base_previews(s, "{}", ids, 4, 64, a.data()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(rcA.load() == -1);  // still waiting
  dapple_session_base_previews_cancel(s);
  ta.join();
  CHECK(rcA.load() == DAPPLE_OK);
  for (auto& i : a) CHECK(i.rgba == nullptr);
  // latest wins: the second request ends the first; only the second renders
  std::vector<DappleImage> b(4), c(2);
  std::thread tb([&] { dapple_session_base_previews(s, "{}", ids, 4, 64, b.data()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  std::atomic<int> rcC{-1};
  std::thread tc([&] { rcC = dapple_session_base_previews(s, "{}", ids, 2, 64, c.data()); });
  tb.join();
  for (auto& i : b) CHECK(i.rgba == nullptr);
  dapple_set_interaction(0);
  tc.join();
  CHECK(rcC.load() == DAPPLE_OK);
  for (auto& i : c) {
    CHECK(i.rgba != nullptr);
    dapple_free_image(&i);
  }
  // cancel between frames: a long list is stopped part-way
  std::vector<const char*> many;
  for (int i = 0; i < 60; ++i) many.push_back(ids[i % 3]);
  std::vector<DappleImage> d(many.size());
  std::atomic<bool> fin{false};
  std::thread td([&] {
    dapple_session_base_previews(s, "{}", many.data(), int(many.size()), 64, d.data());
    fin = true;
  });
  while (!fin && !reinterpret_cast<std::atomic<unsigned char*>*>(&d[0].rgba)->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  dapple_session_base_previews_cancel(s);
  td.join();
  int have = 0;
  for (auto& i : d)
    if (i.rgba) {
      ++have;
      dapple_free_image(&i);
    }
  std::printf("  cancelled mid-list: %d of %zu rendered\n", have, many.size());
  CHECK(have >= 1 && have < int(many.size()));
  // a later call is unaffected by the earlier cancels
  DappleImage e[1]{};
  CHECK(dapple_session_base_previews(s, "{}", ids, 1, 64, e) == DAPPLE_OK && e[0].rgba);
  dapple_free_image(&e[0]);
}

static void testYield(DappleSession* s) {
  const char* ids[] = {"modern_clean", "modern_soft"};
  DappleImage out[2]{};
  dapple_set_interaction(1);
  std::atomic<bool> fin{false};
  std::thread t([&] {
    dapple_session_base_previews(s, "{}", ids, 2, 64, out);
    fin = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(!fin.load());  // nothing rendered while a pointer is down
  dapple_set_interaction(0);
  t.join();
  CHECK(out[0].rgba && out[1].rgba);
  dapple_free_image(&out[0]);
  dapple_free_image(&out[1]);
}

// `chart <in.pfm> <base id> <out.png>`: the measuring tool of scripts/film/gen_film_map.py for the modern renderings. Reads the 24-patch chart (little-endian
// PFM, ProPhoto linear, scene referred, the same file the stocks are measured on), renders it with the engine's reference path (what the app's final render does,
// no auto correction) with `film` = the base id at strength 100, writes an 8-bit sRGB PNG.
static int chartMain(int argc, char** argv) {
  if (argc < 5) {
    std::printf("usage: chart <in.pfm> <base id> <out.png>\n");
    return 2;
  }
  FILE* f = std::fopen(argv[2], "rb");
  if (!f) return 1;
  int w = 0, h = 0;
  float scale = 0;
  if (std::fscanf(f, "PF %d %d %f", &w, &h, &scale) != 3) return 1;
  std::fgetc(f);
  dapple::SourceImage src;
  src.sceneReferred = true;
  src.linear = dapple::ImageF(w, h);
  std::vector<float> row(std::size_t(w) * 3);
  for (int y = h - 1; y >= 0; --y) {
    if (std::fread(row.data(), sizeof(float), row.size(), f) != row.size()) return 1;
    std::memcpy(src.linear.row(y), row.data(), row.size() * sizeof(float));
  }
  std::fclose(f);
  static const dapple::CameraRegistry registry = dapple::CameraRegistry::withBuiltins();
  dapple::EditParams p;
  p.filmStock = argv[3];
  p.filmStrength = 100.f;
  dapple::ImageF out;
  if (!dapple::engine::renderImageF(p, registry.resolve("", ""), nullptr, src, false, out)) return 1;
  std::vector<unsigned char> px(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w * 3; ++x) px[std::size_t(y) * w * 3 + x] = (unsigned char)std::lround(std::fmin(1.f, std::fmax(0.f, out.row(y)[x])) * 255.f);
  return stbi_write_png(argv[4], w, h, 3, px.data(), w * 3) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "chart") return chartMain(argc, argv);
  const std::string id = argc > 1 ? argv[1] : "equal";
  if (!loadPack()) {
    std::printf("  SKIP no film pack\n");
    return 77;
  }
  fs::path file;
  if (id == "raw" || id == "tiers") {
    file = root() / "build" / "rawsamples" / "Canon/EOS-R6-Mark-II/canon-r6m2-img_0346-raw-normal-full.cr3";
    if (!fs::exists(file)) {
      std::printf("  SKIP no build/rawsamples\n");
      return 77;
    }
  } else {
    file = syntheticJpeg();
  }
  DappleSession* s = dapple_session_open(file.string().c_str(), DAPPLE_QUALITY_PREVIEW, id == "raw" || id == "tiers" ? 1024 : 640);
  if (!s) {
    std::printf("  FAIL open: %s\n", dapple_last_error());
    return 1;
  }
  if (id == "equal") testEqual(s, "synthetic");
  else if (id == "raw") testEqual(s, "raw");
  else if (id == "probe") probeScaling(s);
  else if (id == "tiers") probeTiers(s);
  else if (id == "ids") testIds(s);
  else if (id == "cancel") testCancel(s);
  else if (id == "yield") testYield(s);
  else {
    std::printf("unknown test id %s\n", id.c_str());
    return 2;
  }
  dapple_session_close(s);
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : 0;
}
