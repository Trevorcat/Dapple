#pragma once
// MosaicImage (decoder output) -> demosaic::Desc (what the node reads).
#include <cstring>

#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic.h"

namespace dapple::demosaic {

inline Desc makeDesc(const MosaicImage& m) {
  Desc d;
  d.cfa = m.cfa == CfaKind::XTrans ? Cfa::XTrans : Cfa::Bayer;
  d.period = m.period;
  for (int i = 0; i < m.period * m.period; ++i) {
    d.cfa4[i] = m.cfaIdx[i];
    d.cfa3[i] = m.cfaIdx[i] == 3 ? 1 : m.cfaIdx[i];
  }
  d.width = m.width, d.height = m.height;
  d.data = m.data.data();
  d.blackRows = m.blackRows, d.blackCols = m.blackCols;
  d.black = m.black.data();
  d.invSpan = 1.f / (m.white - m.blackMin);
  std::memcpy(d.wb, m.wb, sizeof d.wb);
  std::memcpy(d.m, m.camToWork, sizeof d.m);
  return d;
}

}  // namespace dapple::demosaic
