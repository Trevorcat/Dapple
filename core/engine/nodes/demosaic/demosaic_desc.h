#pragma once
// MosaicImage (decoder output) -> demosaic::Desc (what the node reads).
#include <cstdlib>
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
  static const bool hotEnv = [] { const char* e = std::getenv("DAPPLE_DEMOSAIC_HOT"); return e && e[0] == '1'; }();
  d.hot = hotEnv && !m.fixedBadPixels;  // optional isolated-hot-sample filter (off by default; off when the file's opcodes fixed bad pixels)
  static const bool fast = [] { const char* e = std::getenv("DAPPLE_DEMOSAIC_XFAST"); return e && e[0] == '1'; }();
  d.xtMode = fast ? 0 : 1;
  d.blackRows = m.blackRows, d.blackCols = m.blackCols;
  d.black = m.black.data();
  d.invSpan = 1.f / (m.white - m.blackMin);
  std::memcpy(d.wb, m.wb, sizeof d.wb);
  std::memcpy(d.m, m.camToWork, sizeof d.m);
  return d;
}

}  // namespace dapple::demosaic
