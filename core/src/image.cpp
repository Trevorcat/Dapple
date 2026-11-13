#include "dapple/image.h"

#include <algorithm>
#include <cmath>

#include "parallel.h"
#include "resize_rows.h"

namespace dapple {

ImageF resizeToLongEdge(const ImageF& src, int longEdge) {
  const resize::Geom g = resize::geom(src.width, src.height, longEdge);
  if (g.copy || src.empty()) return src;
  ImageF dst(g.dw, g.dh);
  // Box filter over the exact source footprint with fractional edge weights (resize_rows.h: shared with the streaming export's band reductions).
  parallelRows(g.dh, [&](int y) { resize::row(g, y, [&](int yy) { return src.row(yy); }, dst.row(y)); });
  return dst;
}

}  // namespace dapple
