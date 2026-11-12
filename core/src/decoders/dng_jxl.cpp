#include "decoders/dng_jxl.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>


#ifdef DAPPLE_HAVE_JXL
#include <jxl/decode.h>
#endif

namespace dapple::dng {
namespace {

struct Tiff {
  bool le = true;
  std::vector<std::uint8_t> bytes;
  unsigned u16(std::size_t o) const { return o + 2 <= bytes.size() ? (le ? unsigned(bytes[o] | bytes[o + 1] << 8) : unsigned(bytes[o + 1] | bytes[o] << 8)) : 0u; }
  std::uint32_t u32(std::size_t o) const {
    if (o + 4 > bytes.size()) return 0;
    return le ? (std::uint32_t(bytes[o]) | std::uint32_t(bytes[o + 1]) << 8 | std::uint32_t(bytes[o + 2]) << 16 | std::uint32_t(bytes[o + 3]) << 24)
              : (std::uint32_t(bytes[o + 3]) | std::uint32_t(bytes[o + 2]) << 8 | std::uint32_t(bytes[o + 1]) << 16 | std::uint32_t(bytes[o]) << 24);
  }
  // values of a SHORT / LONG tag of IFD0 (empty when absent)
  std::vector<std::uint32_t> tag(std::uint16_t id) const {
    std::vector<std::uint32_t> v;
    if (bytes.size() < 8) return v;
    const std::size_t ifd = u32(4);
    const unsigned n = u16(ifd);
    for (unsigned i = 0; i < n && i < 1024; ++i) {
      const std::size_t e = ifd + 2 + std::size_t(12) * i;
      if (u16(e) != id) continue;
      const unsigned type = u16(e + 2);
      const std::uint32_t count = u32(e + 4);
      const unsigned sz = type == 3 ? 2u : 4u;
      if ((type != 3 && type != 4) || count == 0 || count > (1u << 22)) return v;
      const std::size_t data = std::size_t(count) * sz <= 4 ? e + 8 : u32(e + 8);
      for (std::uint32_t k = 0; k < count; ++k) v.push_back(type == 3 ? u16(data + std::size_t(k) * 2) : u32(data + std::size_t(k) * 4));
      return v;
    }
    return v;
  }
};

bool loadTiff(const std::filesystem::path& path, Tiff& t) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamsize n = f.tellg();
  if (n < 16 || n > (std::streamsize(1) << 31)) return false;
  t.bytes.resize(std::size_t(n));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(t.bytes.data()), n)) return false;
  if (t.bytes[0] == 'I' && t.bytes[1] == 'I') t.le = true;
  else if (t.bytes[0] == 'M' && t.bytes[1] == 'M') t.le = false;
  else return false;
  return t.u16(2) == 42;
}

}  // namespace

bool isJxlDng(const std::filesystem::path& path) {
  Tiff t;
  if (!loadTiff(path, t)) return false;
  const auto c = t.tag(259);
  return !c.empty() && c[0] == 52546;
}

JxlRaw decodeJxlStream(const std::uint8_t* data, std::size_t size) {
  JxlRaw out;
#ifndef DAPPLE_HAVE_JXL
  (void)data, (void)size;
  out.error = "this build has no JPEG XL decoder (built with DAPPLE_JXL=OFF)";
  return out;
#else
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (!dec) {
    out.error = "JPEG XL decoder allocation failed";
    return out;
  }
  struct Guard {
    JxlDecoder* d;
    ~Guard() { JxlDecoderDestroy(d); }
  } guard{dec};
  if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    out.error = "JPEG XL decoder setup failed";
    return out;
  }
  if (JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS) {
    out.error = "JPEG XL input rejected";
    return out;
  }
  JxlDecoderCloseInput(dec);
  JxlBasicInfo info{};
  JxlPixelFormat fmt{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  for (;;) {
    const JxlDecoderStatus st = JxlDecoderProcessInput(dec);
    if (st == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
        out.error = "JPEG XL basic info unreadable";
        return out;
      }
      out.width = int(info.xsize), out.height = int(info.ysize), out.channels = int(info.num_color_channels);
      if (out.width <= 0 || out.height <= 0 || out.width > 65536 || out.height > 65536 || (out.channels != 1 && out.channels != 3)) {
        out.error = "unsupported JPEG XL image (" + std::to_string(info.xsize) + "x" + std::to_string(info.ysize) + ", " + std::to_string(info.num_color_channels) + " colour channels)";
        return out;
      }
      fmt.num_channels = std::uint32_t(out.channels);
    } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      std::size_t need = 0;
      if (JxlDecoderImageOutBufferSize(dec, &fmt, &need) != JXL_DEC_SUCCESS || need != std::size_t(out.width) * out.height * out.channels * 2) {
        out.error = "JPEG XL output buffer size mismatch";
        return out;
      }
      out.data.assign(need / 2, 0);
      if (JxlDecoderSetImageOutBuffer(dec, &fmt, out.data.data(), need) != JXL_DEC_SUCCESS) {
        out.error = "JPEG XL output buffer rejected";
        return out;
      }
    } else if (st == JXL_DEC_FULL_IMAGE) {
      return out;  // single frame
    } else if (st == JXL_DEC_SUCCESS) {
      if (out.data.empty()) out.error = "JPEG XL stream holds no image";
      return out;
    } else {
      out.error = st == JXL_DEC_NEED_MORE_INPUT ? "JPEG XL stream truncated" : "JPEG XL decode error";
      out.data.clear();
      return out;
    }
  }
#endif
}

JxlRaw decodeJxlDng(const std::filesystem::path& path, int threads) {
  JxlRaw out;
#ifndef DAPPLE_HAVE_JXL
  (void)path, (void)threads;
  out.error = "this build has no JPEG XL decoder (built with DAPPLE_JXL=OFF)";
  return out;
#else
  Tiff t;
  if (!loadTiff(path, t)) {
    out.error = "not a TIFF / DNG";
    return out;
  }
  const auto w = t.tag(256), h = t.tag(257), samples = t.tag(277), comp = t.tag(259);
  if (w.empty() || h.empty() || comp.empty() || comp[0] != 52546) {
    out.error = "IFD0 is not a JPEG XL image";
    return out;
  }
  const int W = int(w[0]), H = int(h[0]), spp = samples.empty() ? 1 : int(samples[0]);
  const auto tw = t.tag(322), th = t.tag(323), toff = t.tag(324), tcnt = t.tag(325);
  const auto soff = t.tag(273), scnt = t.tag(279), rps = t.tag(278);
  struct Piece { std::uint32_t off, len; int x0, y0, w, h; };
  std::vector<Piece> pieces;
  if (!tw.empty() && !th.empty() && !toff.empty() && toff.size() == tcnt.size()) {
    const int TW = int(tw[0]), TH = int(th[0]);
    if (TW <= 0 || TH <= 0) {
      out.error = "bad tile size";
      return out;
    }
    const int across = (W + TW - 1) / TW, down = (H + TH - 1) / TH;
    if (std::size_t(across) * down != toff.size()) {
      out.error = "tile count does not match the image size";
      return out;
    }
    for (int ty = 0; ty < down; ++ty)
      for (int tx = 0; tx < across; ++tx) pieces.push_back({toff[std::size_t(ty) * across + tx], tcnt[std::size_t(ty) * across + tx], tx * TW, ty * TH, TW, TH});
  } else if (!soff.empty() && soff.size() == scnt.size()) {
    const int RPS = rps.empty() ? H : int(std::min<std::uint32_t>(rps[0], std::uint32_t(H)));
    for (std::size_t i = 0; i < soff.size(); ++i) pieces.push_back({soff[i], scnt[i], 0, int(i) * RPS, W, std::min(RPS, H - int(i) * RPS)});
  } else {
    out.error = "no strips or tiles in IFD0";
    return out;
  }
  out.width = W, out.height = H, out.channels = spp;
  out.data.assign(std::size_t(W) * H * spp, 0);
  std::atomic<int> failed{0};
  std::string firstError;
  std::atomic<bool> errSet{false};
  const int n = int(pieces.size());
  auto work = [&](int i) {
    if (failed.load()) return;
    const Piece& p = pieces[std::size_t(i)];
    if (std::size_t(p.off) + p.len > t.bytes.size() || p.len == 0) {
      failed = 1;
      if (!errSet.exchange(true)) firstError = "JPEG XL tile outside the file";
      return;
    }
    JxlRaw r = decodeJxlStream(t.bytes.data() + p.off, p.len);
    if (!r.error.empty() || r.channels != spp) {
      failed = 1;
      if (!errSet.exchange(true)) firstError = r.error.empty() ? "JPEG XL tile has " + std::to_string(r.channels) + " channels, the file says " + std::to_string(spp) : r.error;
      return;
    }
    const int cw = std::min(p.w, W - p.x0), ch = std::min(p.h, H - p.y0);
    if (r.width < cw || r.height < ch) {
      failed = 1;
      if (!errSet.exchange(true)) firstError = "JPEG XL tile smaller than its place in the image";
      return;
    }
    for (int y = 0; y < ch; ++y)
      std::memcpy(&out.data[(std::size_t(p.y0 + y) * W + p.x0) * spp], &r.data[std::size_t(y) * r.width * spp], std::size_t(cw) * spp * 2);
  };
  const int nt = std::max(1, std::min(n, threads > 0 ? threads : int(std::thread::hardware_concurrency())));
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  for (int k = 0; k < nt; ++k)
    pool.emplace_back([&] {
      for (int i = next++; i < n; i = next++) work(i);
    });
  for (auto& th2 : pool) th2.join();
  if (failed) {
    out.error = firstError.empty() ? "JPEG XL decode failed" : firstError;
    out.data.clear();
  }
  return out;
#endif
}

}  // namespace dapple::dng
