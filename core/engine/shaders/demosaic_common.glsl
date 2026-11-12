// Shared by the Demosaic kernels (docs/engine/pipeline-architecture.md section 32). Mirrors core/engine/nodes/demosaic/demosaic_cpu.cpp.
// The mosaic arrives as a window (a tile plus its halo, coordinate-mapped on the host) of packed 16-bit samples, two per uint.
layout(std140, binding = 0) uniform P {
  ivec4 dims;   // window width, window height, ROI width, ROI height
  ivec4 org;    // cfa phase (x, y) of window (0, 0); black table phase (x, y)
  ivec4 io;     // mosaic row stride in samples, output x, output y, 0
  uvec4 cfa;    // colour of phase cell 0..35, 2 bits each (0 R, 1 G, 2 B), 16 cells per word
  vec4 wb;      // multipliers R, G, B, G2
  vec4 sc;      // 1 / span, clamp the output to [0, 1] (1 / 0), 0, 0
  vec4 m0, m1, m2;  // white-balanced camera RGB -> output RGB
} u;

int colAt(int i, int j, int period) {  // colour of window pixel (i, j): 0 R, 1 G, 2 B
  const int ph = ((j + u.org.y) % period) * period + ((i + u.org.x) % period);
  return int((u.cfa[ph >> 4] >> uint((ph & 15) * 2)) & 3u);
}
float clamp01(float x) { return min(max(x, 0.0), 1.0); }
float sq(float x) { return x * x; }
// matrix + clamp, the end of every output pass
vec3 finishRgb(vec3 rgb) {
  rgb = clamp(rgb, vec3(0.0), vec3(1.0));
  vec3 o = vec3(dot(u.m0.xyz, rgb), dot(u.m1.xyz, rgb), dot(u.m2.xyz, rgb));
  return u.sc.y > 0.5 ? clamp(o, vec3(0.0), vec3(1.0)) : o;
}
