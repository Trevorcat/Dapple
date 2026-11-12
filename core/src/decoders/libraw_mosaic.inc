// decodeRawMosaic / finishMosaicRgb / fileContentKey (dapple/mosaic.h). Included at the end of libraw_decoder.cpp, inside namespace dapple, so
// it shares the helpers of that file (PathDatastream, readMetadata, the DNG opcode glue). The black / white / multiplier arithmetic mirrors
// LibRaw's adjust_bl, scale_colors and convert_to_rgb (output_color 4, use_camera_wb, highlight 0, no auto brightness) so that the Demosaic
// node's scaling reproduces what the RGB path feeds into LibRaw's own demosaic. Docs: pipeline-architecture.md section 32.

void fileContentKey(const std::filesystem::path& path, std::uint64_t out[2]) {
  std::uint64_t a = 1469598103934665603ull, b = 0x9E3779B97F4A7C15ull;
  auto feed = [&](const unsigned char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      a = (a ^ p[i]) * 1099511628211ull;
      b = (b + p[i] + 1) * 0xff51afd7ed558ccdull;
      b ^= b >> 29;
    }
  };
  std::error_code ec;
  const std::uint64_t sz = std::filesystem::file_size(path, ec);
  feed(reinterpret_cast<const unsigned char*>(&sz), sizeof sz);
  std::ifstream f(path, std::ios::binary);
  std::vector<unsigned char> buf(65536);
  if (f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size())) || f.gcount() > 0) feed(buf.data(), std::size_t(f.gcount()));
  if (sz > 2 * buf.size()) {
    f.clear();
    f.seekg(std::streamoff(sz - buf.size()));
    if (f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size())) || f.gcount() > 0) feed(buf.data(), std::size_t(f.gcount()));
  }
  out[0] = a;
  out[1] = b;
}

namespace {
// LibRaw::adjust_bl on the values the decoder left after unpack(): folds the repeat pattern and the common level, returns the common black
// (what scale_colors subtracts from `maximum`), the per-colour levels (which include it) and the reduced repeat pattern.
struct BlackNorm {
  int cb[4] = {0, 0, 0, 0};
  int black = 0, br = 0, bc = 0;
  std::vector<int> pat;
};
BlackNorm normalizeBlack(LibRaw& raw, bool bayerFilters) {
  const auto& C = raw.imgdata.color;
  BlackNorm n;
  n.black = int(C.black);
  for (int c = 0; c < 4; ++c) n.cb[c] = int(C.cblack[c]);
  n.br = int(C.cblack[4]), n.bc = int(C.cblack[5]);
  const bool hasPat = n.br > 0 && n.bc > 0 && n.br * n.bc <= int(LIBRAW_CBLACK_SIZE) - 6;
  if (!hasPat) n.br = n.bc = 0;
  for (int i = 0; i < n.br * n.bc; ++i) n.pat.push_back(int(C.cblack[6 + i]));
  if (bayerFilters && (n.br + 1) / 2 == 1 && (n.bc + 1) / 2 == 1) {
    int clrs[4], lastg = -1, gcnt = 0;
    for (int c = 0; c < 4; ++c) {
      clrs[c] = raw.COLOR(c / 2, c % 2);
      if (clrs[c] == 1) ++gcnt, lastg = c;
    }
    if (gcnt > 1 && lastg >= 0) clrs[lastg] = 3;
    for (int c = 0; c < 4; ++c) n.cb[clrs[c] & 3] += n.pat.empty() ? 0 : n.pat[std::size_t(c / 2 % n.br * n.bc + c % 2 % n.bc)];
    n.br = n.bc = 0;
    n.pat.clear();
  } else if (!bayerFilters && n.br == 1 && n.bc == 1) {  // Fuji RAF dng
    for (int c = 0; c < 4; ++c) n.cb[c] += n.pat[0];
    n.br = n.bc = 0;
    n.pat.clear();
  }
  int i = n.cb[3];
  for (int c = 0; c < 3; ++c) i = std::min(i, n.cb[c]);
  for (int c = 0; c < 4; ++c) n.cb[c] -= i;
  n.black += i;
  if (n.br && n.bc) {
    int m = n.pat[0];
    for (int v : n.pat) m = std::min(m, v);
    bool nonz = false;
    for (int& v : n.pat) nonz |= ((v -= m) != 0);
    n.black += m;
    if (!nonz) n.br = n.bc = 0, n.pat.clear();
  }
  for (int c = 0; c < 4; ++c) n.cb[c] += n.black;
  return n;
}

int lcmInt(int a, int b) { return a / gcdInt(a, b) * b; }
}  // namespace

Result<MosaicImage> decodeRawMosaic(const std::filesystem::path& path, const DecodeOptions& options) {
  (void)options;
  PathDatastream stream(path);
  if (!stream.valid()) return fail("Cannot open " + pathToUtf8(path));
  auto raw = std::make_unique<LibRaw>();
  auto& params = raw->imgdata.params;
  params.output_color = 4;
  params.output_bps = 16;
  params.gamm[0] = params.gamm[1] = 1.0;
  params.no_auto_bright = 1;
  params.use_camera_wb = 1;
  params.adjust_maximum_thr = 0.f;
  params.highlight = 0;
  if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS) return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
  MosaicImage m;
  fileContentKey(path, m.key);
  auto none = [&](const std::string& why) -> Result<MosaicImage> {
    m.cfa = CfaKind::None;
    m.reason = why;
    m.data.clear();
    return m;
  };
  const auto& id = raw->imgdata.idata;
  const auto& S = raw->imgdata.sizes;
  if (dng::isJxlDng(path)) return none("JPEG XL DNG (decoded by libjxl, no mosaic)");
  if (id.filters == 0) return none("no colour filter array (sRAW / monochrome / linear data): no demosaic needed");
  if (id.colors != 3) return none("four-colour sensor: LibRaw demosaic");
  const bool xtrans = id.filters == 9, bayer = id.filters > 1000;
  if (!xtrans && !bayer) return none("unsupported filter layout");
  const bool timing = std::getenv("DAPPLE_DECODE_TIMING") != nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  if (int rc = raw->unpack(); rc != LIBRAW_SUCCESS) return fail("LibRaw unpack failed: " + std::string(libraw_strerror(rc)));
  const auto t1 = std::chrono::steady_clock::now();
  if (!raw->imgdata.rawdata.raw_image) return none("the format delivers 3 / 4-channel data, not a 16-bit mosaic");
  if (raw->imgdata.rawdata.ioparams.fuji_width) return none("Fuji rotated sensor layout: LibRaw demosaic");
  if (raw->imgdata.rawdata.ioparams.zero_is_bad) return none("zero pixels are dead pixels in this format (LibRaw removes them): LibRaw demosaic");
  if (raw->imgdata.rawdata.ioparams.raw_color) return none("raw colour output (no usable colour matrix): LibRaw path");
  double wxy[2] = {0, 0};
  m.meta = readMetadata(*raw, readDngWhiteXy(path, wxy) ? wxy : nullptr);
  const bool whiteXy = m.meta.illuminantSource == "dng-whitexy";
  // multipliers as scale_colors computes them for use_camera_wb (+ the DNG AsShotWhiteXY case of the RGB path)
  float pre[4] = {0, 0, 0, 0};
  const auto& C = raw->imgdata.color;
  BlackNorm bn = normalizeBlack(*raw, bayer);
  if (whiteXy) {
    // The RGB path sets params.user_mul from the white point for these DNGs (Pixel 8 Pro), but LibRaw's scale_colors then runs its automatic white
    // balance (cam_mul is empty and use_camera_wb falls back to the picture statistics) and overwrites user_mul: measured on the Pixel 8 Pro sample the
    // RGB path's red / blue are 0.92 / 0.95 of what the multipliers say. Until the owner of that path decides which one is meant, this file keeps
    // going through the RGB path (the oracle) rather than silently changing colour.
    return none("DNG with AsShotWhiteXY only: the RGB path's multipliers are overridden by LibRaw's automatic white balance (see pipeline-architecture.md 32.9)");
  } else {
    if (!(C.cam_mul[0] > 0.00001f)) return none("no as-shot white balance in the file (LibRaw would estimate one from the picture)");
    unsigned sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const int ch = raw->COLOR(r, c) & 3;
        const int v = int(C.white[r][c]) - bn.cb[ch];
        if (v > 0) sum[ch] += unsigned(v);
        sum[ch + 4]++;
      }
    if (sum[0] && sum[1] && sum[2] && sum[3])
      for (int c = 0; c < 4; ++c) pre[c] = float(sum[c + 4]) / float(sum[c]);
    else
      for (int c = 0; c < 4; ++c) pre[c] = C.cam_mul[c];
  }
  if (pre[1] == 0) pre[1] = 1;
  if (pre[3] == 0) pre[3] = pre[1];
  float dmin = pre[0];
  for (int c = 1; c < 4; ++c) dmin = std::min(dmin, pre[c]);
  if (!(dmin > 0.00001f)) return none("unusable white balance multipliers");
  for (int c = 0; c < 4; ++c) m.wb[c] = pre[c] / dmin;
  m.white = float(C.maximum);
  m.blackMin = float(bn.black);
  if (!(m.white - m.blackMin > 16.f)) return none("white level not above black level");
  // colour matrix: LibRaw convert_to_rgb, out_cam = prophoto_rgb * rgb_cam (D65 working space, as the RGB path)
  {
    static const double prophoto[3][3] = {{0.529317, 0.330092, 0.140588}, {0.098368, 0.873465, 0.028169}, {0.016879, 0.117663, 0.865457}};
    double rc = 0;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) rc += std::fabs(double(C.rgb_cam[r][c]));
    if (rc < 1e-6) return none("no camera colour matrix (LibRaw output = camera RGB)");
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        float v = 0.f;
        for (int k = 0; k < 3; ++k) v += float(prophoto[i][k] * C.rgb_cam[k][j]);
        m.camToWork[i * 3 + j] = v;
      }
  }
  // DNG opcode lists that act on the mosaic (as in decode(); OpcodeList3 and the crop are returned for after the demosaic)
  dng::ApplyReport corr;
  const int fileFlip = S.flip;
  if (id.dng_version != 0 && dngOpcodesEnabled()) {
    try {
      const dng::OpcodeList l1 = dngList(*raw, 0), l2 = dngList(*raw, 1), l3 = dngList(*raw, 2);
      for (const dng::OpcodeList* l : {&l1, &l2, &l3})
        if (!l->error.empty()) corr.skipped.push_back("malformed list (" + l->error + ")");
      auto view = [&] {
        dng::MosaicView v;
        v.px = raw->imgdata.rawdata.raw_image;
        v.pitch = std::size_t(S.raw_pitch) / sizeof(std::uint16_t);
        v.top = S.top_margin, v.left = S.left_margin, v.width = S.width, v.height = S.height;
        return v;
      };
      if (!l1.ops.empty()) {
        dng::MosaicView v = view();
        dng::applyMosaic(l1, v, corr);
      }
      if (!l2.ops.empty()) {
        dng::MosaicView v = view();
        fillBlackPattern(*raw, v);
        dng::applyMosaic(l2, v, corr);
      }
      const auto& o3 = raw->imgdata.color.dng_levels.rawopcodes[2];
      if (!l3.ops.empty() && o3.data && o3.len) m.opcodeList3.assign(static_cast<const std::uint8_t*>(o3.data), static_cast<const std::uint8_t*>(o3.data) + o3.len);
      int cx, cy, cw, ch;
      if (dngDefaultCrop(*raw, cx, cy, cw, ch)) m.hasCrop = true, m.cropX = cx, m.cropY = cy, m.cropW = cw, m.cropH = ch;
    } catch (...) {
      m.opcodeList3.clear();
      m.hasCrop = false;
      corr.skipped.push_back("exception");
    }
  }
  m.dngCorrections = corr.summary();
  // the mosaic itself
  m.width = S.width, m.height = S.height, m.flip = fileFlip;
  m.cfa = xtrans ? CfaKind::XTrans : CfaKind::Bayer;
  m.period = xtrans ? 6 : 2;
  for (int y = 0; y < m.period; ++y)
    for (int x = 0; x < m.period; ++x) m.cfaIdx[y * m.period + x] = std::uint8_t(raw->COLOR(y, x) & 3);
  if (bayer)  // adjacent samples of one colour: not a Bayer layout the node handles
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x)
        if (m.cfaIdx[y * 2 + x] == m.cfaIdx[((y + 1) & 1) * 2 + x] || m.cfaIdx[y * 2 + x] == m.cfaIdx[y * 2 + ((x + 1) & 1)]) return none("unusual Bayer layout");
  {
    const int br = bn.br ? bn.br : 1, bc = bn.bc ? bn.bc : 1;
    m.blackRows = lcmInt(m.period, br), m.blackCols = lcmInt(m.period, bc);
    if (m.blackRows * m.blackCols > 4096) return none("black level pattern too large");
    m.black.assign(std::size_t(m.blackRows) * m.blackCols, 0.f);
    for (int y = 0; y < m.blackRows; ++y)
      for (int x = 0; x < m.blackCols; ++x) {
        const int ch = m.cfaIdx[(y % m.period) * m.period + (x % m.period)];
        m.black[std::size_t(y) * m.blackCols + x] = float(bn.cb[ch] + (bn.br ? bn.pat[std::size_t((y % br) * bc + (x % bc))] : 0));
      }
  }
  m.data.resize(std::size_t(m.width) * m.height);
  const std::uint16_t* src = raw->imgdata.rawdata.raw_image;
  const std::size_t pitch = std::size_t(S.raw_pitch) / sizeof(std::uint16_t);
  const int copyH = std::min(int(S.height), int(S.raw_height) - int(S.top_margin)), copyW = std::min(int(S.width), int(S.raw_width) - int(S.left_margin));
  if (copyH != m.height || copyW != m.width) return none("raw buffer smaller than the active area");
  parallelRows(m.height, [&](int y) { std::memcpy(&m.data[std::size_t(y) * m.width], src + (std::size_t(y) + S.top_margin) * pitch + S.left_margin, std::size_t(m.width) * 2); });
  raw->recycle();
  if (timing) {
    const auto t2 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "mosaic-timing %s unpack %.0f ms, copy %.0f ms\n", path.filename().string().c_str(), std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
  return m;
}

ImageF finishMosaicRgb(const MosaicImage& m, ImageF rgb, std::string* corrections) {
  dng::ApplyReport corr;
  if (!m.opcodeList3.empty()) {
    try {
      const dng::OpcodeList l3 = dng::parseOpcodeList(m.opcodeList3.data(), m.opcodeList3.size());
      dng::applyImage(l3, rgb, corr);
    } catch (...) {
      corr.skipped.push_back("exception in geometry");
    }
  }
  if (m.hasCrop) {
    try {
      rgb = dng::cropImage(rgb, m.cropX, m.cropY, m.cropW, m.cropH);
      corr.applied.push_back("DefaultCrop");
    } catch (...) {
      corr.skipped.push_back("exception in geometry");
    }
  }
  rgb = rotateFlip(rgb, m.flip);
  if (corrections) *corrections = corr.summary();
  return rgb;
}

