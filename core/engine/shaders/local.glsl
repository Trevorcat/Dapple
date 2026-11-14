// Local adjustments, stage L (docs/engine/pipeline-architecture.md section 40): the masks and the scene-linear ops of one pixel. Twin of
// core/src/local_eval.cpp (maskOf / applyLinear): keep the arithmetic identical. The including shader defines the binding numbers
// LB_DESC, LB_PLANES (SSBOs: local::Frame::desc, local::planeStore) and LB_AUX0, LB_AUX1 (the stage-W amount images).
// ---- local adjustments, stage L (docs/engine/pipeline-architecture.md section 40): twin of core/src/local_eval.cpp ----------------
layout(constant_id = 3) const bool LOCALW = false;  // the frame has stage-W ops: write the summed amounts into the aux images
layout(binding = LB_DESC, std430) readonly buffer LDesc { vec4 ld[]; };     // local::Frame::desc
layout(binding = LB_PLANES, std430) readonly buffer LPlanes { float lp[]; };  // local::planeStore
layout(binding = LB_AUX0, rgba16f) uniform writeonly image2D uAux0;      // amount channels 0..3 (legacy: clarity, texture, sharpen, noise)
layout(binding = LB_AUX1, rgba16f) uniform writeonly image2D uAux1;      // amount channels 4..7 (legacy: dehaze); local::Frame::chanOp

const vec3 kLumaPP = vec3(0.2880402, 0.7118741, 0.0000857);
float lss(float e0, float e1, float x) {
  const float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
float lcbrt(float x) { return sign(x) * pow(abs(x), 1.0 / 3.0); }
float lplane(int off, int pw, int ph, int ch, int c, vec2 uv) {
  const float fx = clamp(uv.x * float(pw) - 0.5, 0.0, float(pw - 1)), fy = clamp(uv.y * float(ph) - 0.5, 0.0, float(ph - 1));
  const int x0 = min(int(fx), pw - 1), y0 = min(int(fy), ph - 1);
  const int x1 = min(x0 + 1, pw - 1), y1 = min(y0 + 1, ph - 1);
  const float tx = fx - float(x0), ty = fy - float(y0);
  const float a = lp[off + (y0 * pw + x0) * ch + c], b = lp[off + (y0 * pw + x1) * ch + c];
  const float e = lp[off + (y1 * pw + x0) * ch + c], g = lp[off + (y1 * pw + x1) * ch + c];
  const float t0 = a + (b - a) * tx, t1 = e + (g - e) * tx;
  return t0 + (t1 - t0) * ty;
}
float lguide(float v) { return sqrt(clamp(v / ld[1].x, 0.0, 4.0)); }
// L* (0..1) of the luminance, OKLab hue (degrees) and colourfulness / lightness
vec3 lrange(vec3 lin) {
  const float Y = clamp(dot(lin, kLumaPP), 0.0, 1.0);
  const float L = Y > 0.008856 ? 1.16 * lcbrt(Y) - 0.16 : 9.033 * Y;
  const vec3 r = max(lin, vec3(0.0));
  const float l = lcbrt(dot(ld[2].xyz, r)), m = lcbrt(dot(ld[3].xyz, r)), s = lcbrt(dot(ld[4].xyz, r));
  const float Lo = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  const float A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  const float B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  float hue = atan(B, A) * 57.29577951;
  if (hue < 0.0) hue += 360.0;
  return vec3(L, hue, sqrt(A * A + B * B) / max(Lo, 0.05));
}
float lcomp(int ci, vec2 uv, vec2 xy, vec3 lin, vec3 src, inout bool rangeDone, inout vec3 rng) {
  const int base = 8 + 256 + 4 * ci;
  const vec4 h = ld[base], p0 = ld[base + 1], p1 = ld[base + 2], pd = ld[base + 3];
  const int kind = int(h.x);
  if (kind == 0) {  // linear
    const float t = ((xy.x - p0.x) * p0.z + (xy.y - p0.y) * p0.w) * p1.x;
    return 1.0 - lss(0.0, 1.0, t);
  }
  if (kind == 1) {  // radial
    const vec2 d = xy - p0.xy;
    const float xr = (d.x * p0.z + d.y * p0.w) * p1.x, yr = (-d.x * p0.w + d.y * p0.z) * p1.y;
    const float e = sqrt(xr * xr + yr * yr);
    return p1.z < 1e-3 ? (e <= 1.0 ? 1.0 : 0.0) : 1.0 - lss(1.0 - p1.z, 1.0, e);
  }
  if (kind == 2 || kind == 3) {
    if (!rangeDone) {
      rng = lrange(lin);
      rangeDone = true;
    }
    if (kind == 2) {
      float Lc = rng.x;
      const int pw2 = int(pd.x);
      if (pw2 > 0) {  // spatial coherence (section 40.10): L* of the edge-aware smoothed luminance, the pixel's own gain kept
        const int off2 = int(h.w) * 4096 + int(pd.w), ph2 = int(pd.y);
        const float ys = max(dot(src, kLumaPP), 1e-6), yl = max(dot(lin, kLumaPP), 0.0);
        const float ls = log2(ys);
        const float Y = clamp(yl * exp2(lplane(off2, pw2, ph2, 2, 0, uv) * ls + lplane(off2, pw2, ph2, 2, 1, uv) - ls), 0.0, 1.0);
        Lc = Y > 0.008856 ? 1.16 * lcbrt(Y) - 0.16 : 9.033 * Y;
      }
      return lss(p0.x - p0.z, p0.x, Lc) * (1.0 - lss(p0.y, p0.y + p0.z, Lc));
    }
    float dh = abs(rng.y - p0.x);
    dh = min(dh, 360.0 - dh);
    return (1.0 - lss(p0.y, p0.y + p0.z, dh)) * lss(0.5 * p0.w, p0.w + 0.005, rng.z);
  }
  const int off = int(h.w) * 4096 + int(pd.w), pw = int(pd.x), ph = int(pd.y), ch = int(pd.z);
  if (pw <= 0) return 0.0;  // plane missing (raster not supplied, over the per-edit cap)
  if (kind == 4) return lplane(off, pw, ph, 1, 0, uv);
  if (kind == 5) {
    const float q = lplane(off, pw, ph, 4, 0, uv) * lguide(src.r) + lplane(off, pw, ph, 4, 1, uv) * lguide(src.g) +
                    lplane(off, pw, ph, 4, 2, uv) * lguide(src.b) + lplane(off, pw, ph, 4, 3, uv);
    return clamp(q, 0.0, 1.0);
  }
  return 0.0;
}
float lmask(int a, vec2 uv, vec3 lin, vec3 src) {
  const vec4 A = ld[8 + 16 * a];
  const int nc = int(A.x), c0 = int(A.w);
  const vec2 xy = uv * ld[0].zw;
  bool rangeDone = false;
  vec3 rng = vec3(0.0);
  float m = 0.0;
  for (int j = 0; j < nc; ++j) {
    const int ci = c0 + j;
    float cv = lcomp(ci, uv, xy, lin, src, rangeDone, rng);
    const vec4 h = ld[8 + 256 + 4 * ci];
    if (h.z > 0.5) cv = 1.0 - cv;
    const int op = int(h.y);
    if (j == 0) m = cv;
    else if (op == 0) m = max(m, cv);
    else if (op == 1) m *= 1.0 - cv;
    else m *= cv;
  }
  if (A.y > 0.5) m = 1.0 - m;
  return m * A.z;
}
// stage L of one pixel: masks on the input, ops in list order; K0 / K1 = summed stage-W amounts
void localApply(inout vec3 v, vec2 uv, vec3 src, out vec4 K0, out vec4 K1) {
  const int n = int(ld[0].x);
  float w[16];
  const vec3 lin = v;
  for (int a = 0; a < n; ++a) w[a] = lmask(a, uv, lin, src);
  K0 = vec4(0.0);
  K1 = vec4(0.0);
  for (int a = 0; a < n; ++a) {
    const float wa = w[a];
    if (wa <= 0.0) continue;
    const int b = 8 + 16 * a;
    const vec4 l0 = ld[b + 1], l1 = ld[b + 2], l2 = ld[b + 3], tc = ld[b + 4], w0 = ld[b + 5], w1 = ld[b + 6];
    K0 += wa * w0;
    K1 += wa * w1;
    if (tc.w < 0.5) continue;
    if (l1.z != 0.0 || l1.w != 0.0) {
      vec3 g = vec3(exp2(0.006 * l1.z * wa), exp2(-0.004 * l1.w * wa), exp2(-0.006 * l1.z * wa));
      v *= g / dot(g, kLumaPP);
    }
    if (l0.x != 0.0) v *= exp2(l0.x * wa);
    if (l0.y != 0.0 || l0.z != 0.0 || l0.w != 0.0 || l1.x != 0.0 || l1.y != 0.0) {
      const float Y = dot(v, kLumaPP);
      if (Y > 1e-9) {
        const float Lg = log2(Y / 0.18);
        const float d = l0.y * 0.875 * tanh(Lg / 2.5) + l0.z * 1.25 * lss(-0.5, 2.0, Lg) +
                        l0.w * 1.25 * (1.0 - lss(-4.5, -0.5, Lg)) * lss(-11.0, -8.0, Lg) + l1.x * 0.8 * lss(0.5, 2.5, Lg) +
                        l1.y * 0.8 * (1.0 - lss(-8.0, -4.0, Lg)) * lss(-13.0, -10.0, Lg);
        v *= exp2(wa * d);
      }
    }
    if (l2.x != 0.0 || l2.y != 0.0) {
      const float Y = dot(v, kLumaPP);
      const float mx = max(v.r, max(v.g, v.b)), mn = min(v.r, min(v.g, v.b));
      const float sv = mx > 1e-9 ? (mx - mn) / mx : 0.0;
      const float s = (1.0 + l2.x * wa) * (1.0 + l2.y * wa * (1.0 - sv));
      v = max(vec3(Y) + (v - vec3(Y)) * s, vec3(0.0));
    }
    if (l2.z > 0.0) v *= vec3(1.0) + (tc.xyz - vec3(1.0)) * (l2.z * wa);
    if (l2.w > 0.0) {  // whiten (portrait retouch, docs section 43): warm chroma out, a capped lift
      const float t = min(l2.w * wa, 1.0);
      const float hue = lrange(v).y;
      const float hw = lss(0.0, 20.0, hue) * (1.0 - lss(115.0, 140.0, hue));
      const float Y = dot(v, kLumaPP);
      v = max(vec3(Y) + (v - vec3(Y)) * (1.0 - 0.7 * t * hw), vec3(0.0));
      if (Y > 1e-9) {
        const float Y2 = min(Y * exp2(0.5 * t), max(Y, 0.18 * 2.6390158));
        v *= Y2 / Y;
      }
    }
  }
}
