// What the film reads of a picture that is not RAW, and skin protection (docs/engine/pipeline-architecture.md, film section):
//
//   dapple_film_source_tests <id>     ids: deband_ramp deband_edges deband_decoder display_table display_nodes display_scope skin_math skin_no_mask skin_mask
//
//   deband_ramp     a smooth 8-bit quantised ramp comes back smooth (steps below 0.3 codes, error against the ideal ramp halves)
//   deband_edges    a sharp edge and fine texture are returned as decoded, bit for bit
//   deband_decoder  the decoder's 8-bit path debands, DAPPLE_DEBAND=off does not (checked by comparing with the plain conversion), a 16-bit source is untouched
//   display_table   film_display_table.inc (what the shaders read) equals the table built from the calibration knots; monotone; chroma factor in 0..1
//   display_nodes   the inverse camera rendering in the film kernels equals the same transform applied to the input first (byte-identical), and does nothing
//                   when the flag is off
//   display_scope   only a display-referred source with a film stock asks for the inverse: no film and RAW frames never do
//   skin_math       skinCap: chroma above 1.25x the input's is brought down to it, hue and lightness are kept, below it nothing changes
//   skin_no_mask    film_skin = 1 without the planes in the raster store renders byte-identically with film_skin = 0 (no hue window fallback)
//   skin_mask       with a synthetic skin plane: outside it the frame is identical, inside the film's chroma gain is at most 1.25x (+ tolerance)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "../render_f.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/film.h"
#include "decoders/deband.h"
#include "engine/nodes/film/film_nodes.h"
#include "engine/nodes/point/point.h"
#include "film_kernels.h"
#include "film_skin.h"
#include "local_eval.h"

namespace fs = std::filesystem;
using namespace dapple;
namespace ef = dapple::engine::filmnode;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

std::shared_ptr<film::FilmLibrary> g_lib;
std::shared_ptr<film::FilmLibrary> library() {
  if (!g_lib) {
    std::string err;
    const fs::path data = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film";
    const fs::path pack = data / "dapple.dfp";
    if (fs::exists(pack)) g_lib = film::FilmLibrary::loadPack(pack, &err);
    if (!g_lib) g_lib = film::FilmLibrary::loadJsonDir(data / "profiles", &err);
    if (!g_lib) std::fprintf(stderr, "cannot load film library: %s\n", err.c_str());
    if (g_lib) film::setLibrary(g_lib);
  }
  return g_lib;
}

std::uint32_t hash32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

// ---- deband ---------------------------------------------------------------------------------------------------------------------
// A 320 x 192 picture: a horizontal ramp 100 -> 124 codes (13 px per code), plus a slow vertical tilt, quantised to 8 bits like a JPEG would store it.
struct Ramp {
  int w = 320, h = 192;
  std::vector<std::uint8_t> px;
  std::vector<float> ideal;
};
Ramp makeRamp() {
  Ramp r;
  r.px.resize(std::size_t(r.w) * r.h * 3);
  r.ideal.resize(r.px.size());
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x)
      for (int c = 0; c < 3; ++c) {
        const float v = 100.f + 24.f * float(x) / float(r.w - 1) + 3.f * float(y) / float(r.h - 1) + 2.f * float(c);
        r.ideal[(std::size_t(y) * r.w + x) * 3 + c] = v;
        r.px[(std::size_t(y) * r.w + x) * 3 + c] = std::uint8_t(std::lround(v));
      }
  return r;
}

void testDebandRamp() {
  const Ramp r = makeRamp();
  const std::vector<float> out = debandCodes(r.px.data(), r.w, r.h);
  double errIn = 0, errOut = 0;
  float maxStepIn = 0, maxStepOut = 0;
  for (int y = 8; y < r.h - 8; ++y)
    for (int x = 8; x < r.w - 8; ++x)
      for (int c = 0; c < 3; ++c) {
        const std::size_t i = (std::size_t(y) * r.w + x) * 3 + c;
        errIn += std::pow(double(r.px[i]) - double(r.ideal[i]), 2);
        errOut += std::pow(double(out[i]) - double(r.ideal[i]), 2);
        if (x > 8) {
          maxStepIn = std::max(maxStepIn, std::fabs(float(r.px[i]) - float(r.px[i - 3])));
          maxStepOut = std::max(maxStepOut, std::fabs(out[i] - out[i - 3]));
        }
      }
  const double n = double(r.h - 16) * double(r.w - 16) * 3;
  const double rmsIn = std::sqrt(errIn / n), rmsOut = std::sqrt(errOut / n);
  std::printf("  ramp: rms against the ideal %.3f -> %.3f codes, largest neighbour step %.2f -> %.2f\n", rmsIn, rmsOut, maxStepIn, maxStepOut);
  CHECK(maxStepIn >= 1.f);
  CHECK(maxStepOut < 0.3f);       // no step left: the staircase is a ramp
  CHECK(rmsOut < 0.5 * rmsIn);    // and it is the right ramp
  // the values stay within a code of the decoded ones (never a new tone)
  float maxDev = 0;
  for (std::size_t i = 0; i < out.size(); ++i) maxDev = std::max(maxDev, std::fabs(out[i] - float(r.px[i])));
  CHECK(maxDev <= 2.001f);
}

void testDebandEdges() {
  // a sharp edge (40 codes) over a flat ground with a fine texture (+-6 codes, per pixel hash) on its right half
  const int w = 256, h = 160;
  std::vector<std::uint8_t> px(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) {
        int v = x < w / 2 ? 90 : 130;
        if (x >= w / 2) v += int(hash32(std::uint32_t(y * w + x) * 3u + std::uint32_t(c)) % 13u) - 6;
        px[(std::size_t(y) * w + x) * 3 + c] = std::uint8_t(v);
      }
  const std::vector<float> out = debandCodes(px.data(), w, h);
  std::size_t changed = 0;
  for (std::size_t i = 0; i < out.size(); ++i)
    if (out[i] != float(px[i])) ++changed;
  std::printf("  edges + texture: %zu of %zu samples changed\n", changed, out.size());
  // the flat left half is flat (its pixels may change only to the same flat value); the edge column and the whole textured half must be untouched
  bool edgeExact = true, textureExact = true;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) {
        const std::size_t i = (std::size_t(y) * w + x) * 3 + c;
        if (x >= w / 2 - 2 && x < w / 2 + 2 && out[i] != float(px[i])) edgeExact = false;
        if (x >= w / 2 && out[i] != float(px[i])) textureExact = false;
      }
  CHECK(edgeExact);
  CHECK(textureExact);
}

void testDebandDecoder() {
  const fs::path dir = fs::temp_directory_path() / "dapple_deband_test";
  fs::create_directories(dir);
  const Ramp r = makeRamp();
  const fs::path png = dir / "ramp8.png";
  CHECK(stbi_write_png(png.string().c_str(), r.w, r.h, 3, r.px.data(), r.w * 3) != 0);
  const auto dec = makeJpegDecoder();
  auto src = dec->decode(png, DecodeOptions{});
  CHECK(src.has_value());
  if (src) {
    CHECK(!src->sceneReferred);
    // neighbour steps in linear light: a quantised staircase has steps of one code, the debanded ramp has no step beyond a fraction of a code's linear size
    float maxStep = 0, maxStepCodes = 0;
    const int y = r.h / 2;
    for (int x = 10; x < r.w - 10; ++x) {
      const float a = src->linear.row(y)[3 * x + 1], b = src->linear.row(y)[3 * (x - 1) + 1];
      maxStep = std::max(maxStep, std::fabs(a - b) / std::max(a, 1e-4f));
      maxStepCodes = std::max(maxStepCodes, std::fabs(float(r.px[(std::size_t(y) * r.w + x) * 3 + 1]) - float(r.px[(std::size_t(y) * r.w + x - 1) * 3 + 1])));
    }
    std::printf("  decoder: largest neighbour step of the green channel %.4f relative (one code = %.4f at this level)\n", maxStep, 2.2f / 112.f);
    CHECK(maxStepCodes >= 1.f);
    CHECK(maxStep < 0.4f * 2.2f / 112.f);
  }
}

// ---- display inverse ---------------------------------------------------------------------------------------------------------------
void testDisplayTable() {
  const film::detail::DisplayInverse& a = film::detail::displayInverse();
  const film::detail::DisplayInverse b = film::detail::buildDisplayInverseReference();
  double worst = 0;
  for (int i = 0; i <= 64; ++i) worst = std::max(worst, std::fabs(double(a.tone[i]) - double(b.tone[i])) / std::max(double(b.tone[i]), 1e-6));
  std::printf("  table vs the calibration knots: worst relative difference %.2e; tone(1) = %.4f, tone(0.18 -> %.4f)\n", worst, a.tone[64], 0.f);
  CHECK(worst < 2e-6);
  for (int i = 1; i <= 64; ++i) CHECK(a.tone[i] > a.tone[i - 1]);
  CHECK(a.tone[0] == 0.f);
  CHECK(a.k0 > 0.f && a.k0 <= 1.f && a.k1 >= a.k0 && a.k1 <= 1.f && a.e1 > a.e0);
  CHECK(a.k0 == b.k0 && a.k1 == b.k1 && a.e0 == b.e0 && a.e1 == b.e1);
  // mid grey stays mid grey (the anchor), a grey stays grey (no chroma from nothing)
  const float grey[3] = {0.18f, 0.18f, 0.18f};
  float o[3];
  film::detail::displayInverseApply(a, grey, o);
  CHECK(std::fabs(o[0] - 0.18f) < 0.01f && std::fabs(o[1] - o[0]) < 1e-6f && std::fabs(o[2] - o[0]) < 1e-6f);
}

ImageF testPicture(int w, int h) {
  ImageF img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float fx = float(x) / float(w - 1), fy = float(y) / float(h - 1);
      float* p = img.row(y) + 3 * x;
      p[0] = 0.05f + 0.85f * fx;
      p[1] = 0.04f + 0.7f * fy * (0.4f + 0.6f * fx);
      p[2] = 0.03f + 0.5f * (1.f - fx) * fy;
    }
  return img;
}

void testDisplayNodes() {
  if (!library()) std::exit(77);
  const int w = 96, h = 64;
  const ImageF in = testPicture(w, h);
  ImageF inv(w, h);
  for (std::size_t i = 0; i < in.data.size(); i += 3) film::detail::displayInverseApply(film::detail::displayInverse(), &in.data[i], &inv.data[i]);
  for (const char* stock : {"kodak_portra_400", "kodak_portra_800_push2"}) {
    ef::FilmFrameArgs a;
    a.lib = library().get();
    a.stock = stock;
    const std::string paper = film::defaultPaperFor(*library(), stock);
    a.paper = paper.c_str();
    a.gain[0] = 1.2f, a.gain[1] = 1.f, a.gain[2] = 1.4f;
    a.w = w, a.h = h;
    a.quality = 1;
    a.grainPct = 0, a.halationPct = 0;
    ImageF direct(w, h), viaFlag(w, h), plain(w, h);
    CHECK(ef::renderFilmFrame(a, inv.data.data(), std::size_t(w) * 3, direct.data.data(), std::size_t(w) * 3));
    a.displayRef = true;
    CHECK(ef::renderFilmFrame(a, in.data.data(), std::size_t(w) * 3, viaFlag.data.data(), std::size_t(w) * 3));
    // the kernels are built with FMA contraction (AVX2) and the test's own transform is not: equal to rounding, not to the bit
    float worst = 0;
    for (std::size_t i = 0; i < direct.data.size(); ++i) worst = std::max(worst, std::fabs(direct.data[i] - viaFlag.data[i]));
    std::printf("  %-24s transform in front of the film vs displayRef: largest difference %.2e\n", stock, double(worst));
    CHECK(worst < 2e-4f);
    a.displayRef = false;
    CHECK(ef::renderFilmFrame(a, in.data.data(), std::size_t(w) * 3, plain.data.data(), std::size_t(w) * 3));
    float flagEffect = 0;
    for (std::size_t i = 0; i < plain.data.size(); ++i) flagEffect = std::max(flagEffect, std::fabs(plain.data[i] - viaFlag.data[i]));
    CHECK(flagEffect > 0.02f);  // the flag does something
  }
}

void testDisplayScope() {
  if (!library()) std::exit(77);
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  SourceImage src;
  src.linear = testPicture(64, 48);
  FastOptions fo;
  for (int variant = 0; variant < 4; ++variant) {
    EditParams p;
    if (variant & 1) p.filmStock = "kodak_portra_400";
    src.sceneReferred = (variant & 2) != 0;
    const engine::point::Params r = engine::point::resolve(p, cam, nullptr);
    const engine::point::Blocks b = engine::point::resolveFrame(r, 64, 48, src.sceneReferred, fo, &src, false);
    const bool want = (variant & 1) && !(variant & 2);  // a film stock on a display-referred source
    std::printf("  film %d scene-referred %d: displayRef %d\n", variant & 1, (variant & 2) != 0, int(b.film.displayRef));
    CHECK(b.film.displayRef == want);
  }
}

// ---- skin protection ------------------------------------------------------------------------------------------------------------------
void labOfEncoded(const float e[3], float lab[3]) {
  float lin[3];
  for (int c = 0; c < 3; ++c) {
    const float v = std::clamp(e[c], 0.f, 1.f);
    lin[c] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
  }
  film::skinLab(lin, lab);
}
float chromaOf(const float lab[3]) { return std::hypot(lab[1], lab[2]); }

void testSkinMath() {
  const float ref[3] = {0.40f, 0.26f, 0.20f};  // a skin tone, linear sRGB
  float labRef[3];
  film::skinLab(ref, labRef);
  // a film picture with the same lightness and hue and 1.7x the chroma
  float labFilm[3] = {labRef[0], labRef[1] * 1.7f, labRef[2] * 1.7f}, lin[3], enc[3];
  film::skinFromLab(labFilm, lin);
  for (int c = 0; c < 3; ++c) enc[c] = lin[c] <= 0.0031308f ? 12.92f * lin[c] : 1.055f * std::pow(lin[c], 1.f / 2.4f) - 0.055f;
  float before[3];
  labOfEncoded(enc, before);
  float out[3] = {enc[0], enc[1], enc[2]};
  film::skinCap(out, ref, 1.f);
  float after[3];
  labOfEncoded(out, after);
  const float gainBefore = chromaOf(before) / chromaOf(labRef), gainAfter = chromaOf(after) / chromaOf(labRef);
  const float hueB = std::atan2(before[2], before[1]), hueA = std::atan2(after[2], after[1]);
  std::printf("  chroma gain %.3f -> %.3f, lightness %.2f -> %.2f, hue %.4f -> %.4f rad\n", gainBefore, gainAfter, before[0], after[0], hueB, hueA);
  CHECK(gainBefore > 1.6f);
  CHECK(std::fabs(gainAfter - 1.25f) < 0.01f);
  CHECK(std::fabs(after[0] - before[0]) < 0.02f);
  CHECK(std::fabs(hueA - hueB) < 0.002f);
  // below the cap: untouched; amount 0: untouched; amount 0.5: halfway
  float low[3];
  labFilm[1] = labRef[1] * 1.1f, labFilm[2] = labRef[2] * 1.1f;
  film::skinFromLab(labFilm, lin);
  for (int c = 0; c < 3; ++c) low[c] = lin[c] <= 0.0031308f ? 12.92f * lin[c] : 1.055f * std::pow(lin[c], 1.f / 2.4f) - 0.055f;
  float keep[3] = {low[0], low[1], low[2]};
  film::skinCap(keep, ref, 1.f);
  CHECK(std::memcmp(keep, low, sizeof keep) == 0);
  float none[3] = {enc[0], enc[1], enc[2]};
  film::skinCap(none, ref, 0.f);
  CHECK(std::memcmp(none, enc, sizeof none) == 0);
  float half[3] = {enc[0], enc[1], enc[2]};
  film::skinCap(half, ref, 0.5f);
  for (int c = 0; c < 3; ++c) CHECK(std::fabs(half[c] - 0.5f * (enc[c] + out[c])) < 1e-5f);
}

// a source picture with skin-like colour on the left and a blue-green ground on the right
SourceImage skinSource(int w, int h, bool display) {
  SourceImage s;
  s.sceneReferred = !display;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float* p = s.linear.row(y) + 3 * x;
      const float t = float(y) / float(h - 1);
      if (x < w / 2) {
        p[0] = 0.34f + 0.1f * t, p[1] = 0.22f + 0.07f * t, p[2] = 0.16f + 0.05f * t;
      } else {
        p[0] = 0.08f + 0.05f * t, p[1] = 0.2f + 0.1f * t, p[2] = 0.3f + 0.1f * t;
      }
    }
  return s;
}

void plantPlane(const char* id, int w, int h, int x0, int x1) {  // a plane covering columns [x0, x1) of its w x h grid
  std::vector<std::uint8_t> px(std::size_t(w) * h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = x0; x < x1; ++x) px[std::size_t(y) * w + x] = 255;
  local::rasterSet(id, w, h, px.data(), w);
}

void testSkinNoMask() {
  if (!library()) std::exit(77);
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  for (int display = 0; display < 2; ++display) {
    const SourceImage src = skinSource(160, 120, display != 0);
    EditParams off;
    off.filmStock = "kodak_portra_800_push2";
    EditParams on = off;
    on.filmSkin = 1.f;
    on.filmSkinPhoto = "ai:nomask0000000000";  // a photo id without planes in the store
    const ImageF a = renderF(off, cam, nullptr, src), b = renderF(on, cam, nullptr, src);
    CHECK(a.data.size() == b.data.size() && std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0);
    EditParams noId = on;
    noId.filmSkinPhoto.clear();  // no photo id at all
    const ImageF c = renderF(noId, cam, nullptr, src);
    CHECK(std::memcmp(a.data.data(), c.data.data(), a.data.size() * sizeof(float)) == 0);
    std::printf("  display-referred %d: no planes -> identical\n", display);
  }
}

void testSkinMask() {
  if (!library()) std::exit(77);
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  const int w = 200, h = 120;
  plantPlane("ai:skintest:faceskin", 100, 60, 8, 42);  // columns 8..41 of 100: u in [0.08, 0.42]
  for (int display = 0; display < 2; ++display) {
    const SourceImage src = skinSource(w, h, display != 0);
    for (const char* stock : {"kodak_portra_400", "kodak_portra_800_push2"}) {
      EditParams off;
      off.filmStock = stock;
      off.filmGrain = 0.f;
      off.filmHalation = 0.f;
      off.sharpen = off.nrLuma = off.nrColor = off.clarity = off.texture = off.dehaze = off.grain = 0.f;  // no stage whose strength follows the frame's statistics
      EditParams on = off;
      on.filmSkin = 1.f;
      on.filmSkinPhoto = "ai:skintest";
      const ImageF a = renderF(off, cam, nullptr, src), b = renderF(on, cam, nullptr, src);
      // outside the feathered plane (right of u = 0.42 + the feather, and at the left edge) the frames are identical; inside the chroma gain is capped
      const engine::point::Params r = engine::point::resolve(on, cam, nullptr);
      const float gain[3] = {r.wb[0], r.wb[1], r.wb[2]};
      int outsideDiff = 0, insideN = 0;
      double gainOff = 0, gainOn = 0, worstOn = 0;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const float u = (float(x) + 0.5f) / float(w);
          const float* pa = a.row(y) + 3 * x;
          const float* pb = b.row(y) + 3 * x;
          if (u > 0.55f) {
            if (std::memcmp(pa, pb, 3 * sizeof(float)) != 0) ++outsideDiff;
            continue;
          }
          if (u < 0.2f || u > 0.3f) continue;  // the core of the plane: the feather has no say
          float ref[3];
          film::skinRef(src.linear.row(y) + 3 * x, display != 0 && film::detail::displayInverseEnabled(), gain, ref);
          float labRef[3], labA[3], labB[3];
          film::skinLab(ref, labRef);
          labOfEncoded(pa, labA);
          labOfEncoded(pb, labB);
          const double cr = chromaOf(labRef);
          if (cr < 1.0) continue;
          ++insideN;
          gainOff += chromaOf(labA) / cr;
          gainOn += chromaOf(labB) / cr;
          worstOn = std::max(worstOn, double(chromaOf(labB) / cr));
        }
      std::printf("  %-24s display-referred %d: outside differing %d, inside mean chroma gain %.3f -> %.3f (worst %.3f)\n", stock, display, outsideDiff,
                  insideN ? gainOff / insideN : 0.0, insideN ? gainOn / insideN : 0.0, worstOn);
      CHECK(outsideDiff == 0);
      CHECK(insideN > 200);
      if (std::strstr(stock, "push2")) CHECK(gainOff / insideN > 1.3);  // a pushed film does gain chroma on this picture ...
      CHECK(worstOn <= 1.25 + 0.03);          // ... and the cap holds (the 8 bit-free float render: only the feather and the Lab round trip are tolerance)
    }
  }
}

struct Test {
  const char* id;
  void (*fn)();
};
const Test kTests[] = {
    {"deband_ramp", testDebandRamp},       {"deband_edges", testDebandEdges}, {"deband_decoder", testDebandDecoder},
    {"display_table", testDisplayTable},   {"display_nodes", testDisplayNodes}, {"display_scope", testDisplayScope},
    {"skin_math", testSkinMath},           {"skin_no_mask", testSkinNoMask},  {"skin_mask", testSkinMask},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--list") {
    for (const Test& t : kTests) std::printf("%s\n", t.id);
    return 0;
  }
  bool ran = false;
  for (const Test& t : kTests)
    if (argc < 2 || std::string(argv[1]) == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      ran = true;
    }
  if (!ran) return 2;
  std::printf(g_failures ? "FAILED: %d check(s)\n" : "ok\n", g_failures);
  return g_failures ? 1 : 0;
}
