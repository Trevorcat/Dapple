// Film swatch bands for the UI: three fixed test patches (skin, sky, deep shadow; linear sRGB scene reflectances) rendered
// through every camera stock with its default paper. Separate from c_api.cpp so the swatch design can change without
// touching the main C API file; the 18 % grey patch of dapple_film_swatches_json shows nearly nothing of a stock's character
// (every stock is calibrated to put grey at about the same level), the shadow patch shows its toe, cast and black level.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "color.h"
#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/renderer.h"

namespace {
std::string jsonEsc(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out;
}
char* dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}
}  // namespace

// {"stock_id":["skin","sky","shadow"],...} with rrggbb hex; a pure function of the loaded pack (callers cache it).
extern "C" DAPPLE_API char* dapple_film_swatch_bands_json(void) {
  try {
    using namespace dapple;
    const auto lib = film::defaultLibrary();
    std::string j = "{";
    if (lib) {
      static const float kPatch[3][3] = {{0.50f, 0.30f, 0.22f}, {0.15f, 0.28f, 0.55f}, {0.03f, 0.03f, 0.03f}};
      constexpr int kCell = 16;
      SourceImage src;
      src.sceneReferred = true;
      src.linear = ImageF(3 * kCell, kCell);
      for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < 3 * kCell; ++x) color::srgbToProPhoto().apply(kPatch[x / kCell], src.linear.row(y) + 3 * x);
      static const CameraRegistry registry = CameraRegistry::withBuiltins();
      const CameraProfile& cam = registry.resolve("", "");
      bool first = true;
      for (const film::StockInfo& si : film::listStocks(*lib)) {
        if (si.kind == "paper") continue;
        EditParams p;
        p.filmStock = si.id;
        p.filmStrength = 100.f;
        const ImageF out = Renderer(p, cam, nullptr).renderFloat(src, false);
        if (out.width != 3 * kCell) continue;
        j += std::string(first ? "" : ",") + "\"" + jsonEsc(si.id) + "\":[";
        for (int k = 0; k < 3; ++k) {
          const float* px = out.row(kCell / 2) + 3 * (k * kCell + kCell / 2);
          char hex[16];
          std::snprintf(hex, sizeof hex, "%s\"%02x%02x%02x\"", k ? "," : "", int(std::lround(std::clamp(px[0], 0.f, 1.f) * 255.f)),
                        int(std::lround(std::clamp(px[1], 0.f, 1.f) * 255.f)), int(std::lround(std::clamp(px[2], 0.f, 1.f) * 255.f)));
          j += hex;
        }
        j += "]";
        first = false;
      }
    }
    return dup(j + "}");
  } catch (...) {
    return nullptr;
  }
}
