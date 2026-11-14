// Portrait reshaping (docs/engine/pipeline-architecture.md section 45): the backward displacement field, applied to the source position after
// the geometry mapping. Twin of warp::displace (core/src/warp_field.h): keep the arithmetic identical.
// The including shader defines WARP_AT(i) (a float of the field's storage buffer) and the uniform members
//   ivec4 wI;  // gw, gh, float offset of the field in the buffer, 0
//   vec4 wB;   // ax, bx, ay, by: grid coordinate = s * a + b
//   vec4 wC;   // lpx (source pixels per long edge), 0, 0, 0
// Returns false (and leaves the position alone) where the four nodes are exactly zero.
bool warpDisplace(inout float sx, inout float sy) {
  const int gw = u.wI.x, gh = u.wI.y;
  float gx = sx * u.wB.x + u.wB.y, gy = sy * u.wB.z + u.wB.w;
  gx = min(max(gx, 0.0), float(gw - 1));
  gy = min(max(gy, 0.0), float(gh - 1));
  const int i = min(int(gx), gw - 2), j = min(int(gy), gh - 2);
  const float fx = gx - float(i), fy = gy - float(j);
  const int a = u.wI.z + 2 * (j * gw + i), c = a + 2 * gw;
  const float a0 = WARP_AT(a), a1 = WARP_AT(a + 1), a2 = WARP_AT(a + 2), a3 = WARP_AT(a + 3);
  const float c0 = WARP_AT(c), c1 = WARP_AT(c + 1), c2 = WARP_AT(c + 2), c3 = WARP_AT(c + 3);
  if (a0 == 0.0 && a1 == 0.0 && a2 == 0.0 && a3 == 0.0 && c0 == 0.0 && c1 == 0.0 && c2 == 0.0 && c3 == 0.0) return false;
  const float dx = (a0 * (1.0 - fx) + a2 * fx) * (1.0 - fy) + (c0 * (1.0 - fx) + c2 * fx) * fy;
  const float dy = (a1 * (1.0 - fx) + a3 * fx) * (1.0 - fy) + (c1 * (1.0 - fx) + c3 * fx) * fy;
  sx += dx * u.wC.x;
  sy += dy * u.wC.x;
  return true;
}
