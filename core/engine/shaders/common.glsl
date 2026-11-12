// Shared helpers of the Dapple compute kernels (docs/engine/gpu-backend.md). Everything here mirrors the CPU
// reference in core/src/spatial.cpp / renderer.cpp; keep the arithmetic identical when changing either side.
const vec3 kLuma = vec3(0.2126729, 0.7151522, 0.0721750);  // sRGB luma, as spatial.cpp kR/kG/kB
float lumaOf(vec3 c) { return dot(c, kLuma); }

// Epilogue switches shared by every kernel that can end the pipeline: the last pass adds the grain
// (stateless hash of the picture-space pixel, so tiles agree) and writes the final RGBA8 instead of the
// RGBA16F working image.
layout(constant_id = 10) const bool LAST = false;
layout(constant_id = 11) const bool GRAIN = false;

uint hash2(uint x, uint y) {
  uint h = x * 374761393u + y * 668265263u + 0x9E3779B9u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float latticeNoise(int ix, int iy) {  // triangular noise in [-1, 1]
  const uint a = hash2(uint(ix), uint(iy));
  const uint b = hash2(uint(ix) + 7919u, uint(iy) + 104729u);
  return float((a & 0xFFFFu) + (b & 0xFFFFu)) * (1.0 / 65535.0) - 1.0;
}
vec3 grainApply(vec3 c, ivec2 g, float amp, float period) {
  float n;
  if (period <= 1.01) {
    n = latticeNoise(g.x, g.y);
  } else {
    const float inv = 1.0 / period;
    const float fx = float(g.x) * inv, fy = float(g.y) * inv;
    const int ix = int(fx), iy = int(fy);
    const float tx0 = fx - float(ix), ty0 = fy - float(iy);
    const float tx = tx0 * tx0 * (3.0 - 2.0 * tx0), ty = ty0 * ty0 * (3.0 - 2.0 * ty0);
    const float a = latticeNoise(ix, iy) + (latticeNoise(ix + 1, iy) - latticeNoise(ix, iy)) * tx;
    const float b = latticeNoise(ix, iy + 1) + (latticeNoise(ix + 1, iy + 1) - latticeNoise(ix, iy + 1)) * tx;
    n = a + (b - a) * ty;
  }
  const float Yv = lumaOf(c);
  const float wgt = 0.25 + 0.75 * 4.0 * Yv * (1.0 - Yv);
  return clamp(c + vec3(n * amp * wgt), vec3(0.0), vec3(1.0));
}

// bilinear sample of a low-resolution grid (factor f) at full-resolution pixel i of one axis: spatial.cpp makeAxis
void lowAxis(int i, int f, int ln, out int i0, out int i1, out float t) {
  const float p = clamp((float(i) + 0.5) / float(f) - 0.5, 0.0, float(ln - 1));
  i0 = min(int(p), ln - 1);
  i1 = min(i0 + 1, ln - 1);
  t = p - float(i0);
}

// RGBA8 output with the CPU renderer rounding (uint8(v * 255 + 0.5)) applied explicitly; the +0.25 keeps the value away from
// the rounding boundary of the driver float -> UNORM conversion (some drivers truncate), so every driver stores the same byte.
vec4 pack8(vec3 c) { return vec4((floor(clamp(c, vec3(0.0), vec3(1.0)) * 255.0 + 0.5) + 0.25) * (1.0 / 255.0), 1.0); }

// Round a float to the nearest value a RGBA16F store can hold (round to nearest even on the 10 bit mantissa), so the
// store is exact whatever rounding mode the driver uses for float -> half (some truncate, which biases the picture).
float h16(float x) {
  uint u = floatBitsToUint(x);
  u += 0xFFFu + ((u >> 13) & 1u);
  return uintBitsToFloat(u & ~0x1FFFu);
}
vec4 h16v(vec4 v) { return vec4(h16(v.x), h16(v.y), h16(v.z), v.w); }
