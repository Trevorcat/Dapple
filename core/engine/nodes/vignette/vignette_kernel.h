#pragma once
// The vignette formula (inline: the per-pixel loops of the point kernels call it too). Needs core/src (color.h).
#include <algorithm>
#include <cmath>

#include "color.h"
#include "engine/nodes/vignette/vignette.h"

namespace dapple::engine::vignette {

// The formula on one display-encoded pixel; (dx, dy) = offset from the centre in units of the half picture. `p3`: e3 holds Display P3
// encoded values; the vignette is authored on sRGB-encoded values, so it runs there (P3 -> sRGB primaries, extended transfer curve, the
// formula, back): a colour inside sRGB gets the sRGB path's value. GPU twin: point.comp (u.geoC.w) / film_fin.comp.
inline void apply(float e3[3], float dx, float dy, float vig, bool p3 = false) {
  using namespace color;
  const float f = smoothstep(0.35f, 1.05f, std::sqrt((dx * dx + dy * dy) * 0.5f));
  if (!p3) {
    for (int c = 0; c < 3; ++c) e3[c] = vig < 0 ? e3[c] * (1.f + vig * 0.6f * f) : e3[c] + (1.f - e3[c]) * vig * 0.6f * f;
    return;
  }
  const float q[3] = {srgbDecode(e3[0]), srgbDecode(e3[1]), srgbDecode(e3[2])};
  float s[3];
  p3ToSrgb().apply(q, s);
  for (int c = 0; c < 3; ++c) {
    const float e = srgbEncodeExt(s[c]);
    s[c] = srgbDecodeExt(vig < 0 ? e * (1.f + vig * 0.6f * f) : e + (1.f - e) * vig * 0.6f * f);
  }
  float o[3];
  srgbToP3().apply(s, o);
  for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(o[c]);
}

// Output pixel (x, y) of the frame: `p3` given separately (the film streaming path applies the sRGB formula whatever the block says).
inline void applyAt(const Block& b, float e3[3], int x, int y, bool p3) {
  const float px = b.ox0 + (float(x) + 0.5f) * b.stepX - 0.5f, py = b.oy0 + (float(y) + 0.5f) * b.stepY - 0.5f;
  apply(e3, (px - b.cx) / b.cx, (py - b.cy) / b.cy, b.amount, p3);
}

}  // namespace dapple::engine::vignette
