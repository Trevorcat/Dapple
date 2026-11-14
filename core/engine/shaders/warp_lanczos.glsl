// Lanczos-3 sample of the source for the reads moved by the portrait reshaping field (section 45). Twin of sampleLanczos3
// (core/src/renderer.cpp): 6 x 6 taps, normalised weights, clamped to the picture (srcAt), >= 0. Included after srcAt.
float lanczos3w(float d) {
  if (abs(d) < 1e-6) return 1.0;
  if (abs(d) >= 3.0) return 0.0;
  const float a = 3.14159265 * d;
  return 3.0 * sin(a) * sin(a * (1.0 / 3.0)) / (a * a);
}
vec3 sampleLanczos3(float x, float y) {
  x -= 0.5;
  y -= 0.5;
  const float fxl = floor(x), fyl = floor(y);
  const int ix = int(fxl), iy = int(fyl);
  const float tx = x - fxl, ty = y - fyl;
  float wx[6], wy[6];
  float sx = 0.0, sy = 0.0;
  for (int k = 0; k < 6; ++k) {
    wx[k] = lanczos3w(float(k - 2) - tx);
    wy[k] = lanczos3w(float(k - 2) - ty);
    sx += wx[k];
    sy += wy[k];
  }
  vec3 o = vec3(0.0);
  for (int j = 0; j < 6; ++j) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 6; ++i) acc += (wx[i] / sx) * srcAt(ix - 2 + i, iy - 2 + j);
    o += (wy[j] / sy) * acc;
  }
  return max(o, vec3(0.0));
}
