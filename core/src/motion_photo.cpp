// MP4 stream-copy trimming and Google Motion Photo v1 container writer / validator.
// See dapple/motion_photo.h for the approach.

#include "dapple/motion_photo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>

#include "file_util.h"

namespace dapple {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t cc(const char (&s)[5]) {
  return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16) |
         (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}
std::string ccStr(std::uint32_t t) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[i] = char((t >> (24 - 8 * i)) & 0xFF);
  return s;
}

std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t rd32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint64_t rd64(const std::uint8_t* p) { return (std::uint64_t(rd32(p)) << 32) | rd32(p + 4); }

struct Span {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0;
};
struct BoxView {
  std::uint32_t type = 0;
  Span payload, whole;
};

template <class F>
void children(Span s, F&& f) {
  std::size_t pos = 0;
  while (pos + 8 <= s.n) {
    std::uint64_t sz = rd32(s.p + pos);
    const std::uint32_t type = rd32(s.p + pos + 4);
    std::size_t hdr = 8;
    if (sz == 1) {
      if (pos + 16 > s.n) break;
      sz = rd64(s.p + pos + 8);
      hdr = 16;
    } else if (sz == 0) {
      sz = s.n - pos;
    }
    if (sz < hdr || pos + sz > s.n) break;
    f(BoxView{type, {s.p + pos + hdr, std::size_t(sz - hdr)}, {s.p + pos, std::size_t(sz)}});
    pos += std::size_t(sz);
  }
}

std::optional<BoxView> findChild(Span s, std::uint32_t type) {
  std::optional<BoxView> r;
  children(s, [&](const BoxView& b) {
    if (!r && b.type == type) r = b;
  });
  return r;
}

// ---------------------------------------------------------------------------
// Track model
// ---------------------------------------------------------------------------
struct Track {
  std::uint32_t id = 0, timescale = 0, codec = 0;
  std::uint64_t mediaDuration = 0;
  std::uint16_t lang = 0x55c4;
  std::string handler;
  Bytes hdlrRaw, mediaHdrRaw, dinfRaw, stsdRaw;
  std::uint32_t width16 = 0, height16 = 0;  // 16.16 fixed point from tkhd
  std::uint32_t matrix[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  std::int64_t elstMediaTime = 0;
  bool hasElst = false;
  // samples (decode order)
  std::vector<std::uint32_t> size;
  std::vector<std::uint64_t> dts, off;
  std::vector<std::int32_t> cto;
  std::vector<std::uint8_t> sync;
  std::uint32_t lastDelta = 0;
  bool hasStss = false, hasCtts = false;

  double ptsSec(std::size_t i) const {
    const std::int64_t v = std::int64_t(dts[i]) + (cto.empty() ? 0 : cto[i]) - (hasElst ? elstMediaTime : 0);
    return double(v) / timescale;
  }
};

Bytes toBytes(Span s) { return Bytes(s.p, s.p + s.n); }

Result<void> parseSamples(Span stbl, Track& t) {
  auto stts = findChild(stbl, cc("stts"));
  auto stsz = findChild(stbl, cc("stsz"));
  auto stsc = findChild(stbl, cc("stsc"));
  auto stco = findChild(stbl, cc("stco"));
  auto co64 = findChild(stbl, cc("co64"));
  auto ctts = findChild(stbl, cc("ctts"));
  auto stss = findChild(stbl, cc("stss"));
  if (!stts || !stsz || !stsc || (!stco && !co64)) return fail("MP4: incomplete sample tables");

  // sizes
  {
    const Span p = stsz->payload;
    if (p.n < 12) return fail("MP4: bad stsz");
    const std::uint32_t fixed = rd32(p.p + 4), count = rd32(p.p + 8);
    if (fixed == 0 && p.n < 12 + std::size_t(count) * 4) return fail("MP4: truncated stsz");
    t.size.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) t.size[i] = fixed ? fixed : rd32(p.p + 12 + 4 * std::size_t(i));
  }
  const std::size_t N = t.size.size();
  // decode times
  {
    const Span p = stts->payload;
    if (p.n < 8) return fail("MP4: bad stts");
    const std::uint32_t ne = rd32(p.p + 4);
    if (p.n < 8 + std::size_t(ne) * 8) return fail("MP4: truncated stts");
    t.dts.reserve(N);
    std::uint64_t cur = 0;
    for (std::uint32_t e = 0; e < ne && t.dts.size() < N; ++e) {
      const std::uint32_t cnt = rd32(p.p + 8 + 8 * std::size_t(e)), delta = rd32(p.p + 12 + 8 * std::size_t(e));
      for (std::uint32_t k = 0; k < cnt && t.dts.size() < N; ++k) {
        t.dts.push_back(cur);
        cur += delta;
      }
      t.lastDelta = delta;
    }
    if (t.dts.size() != N) return fail("MP4: stts does not cover all samples");
  }
  // composition offsets
  if (ctts) {
    const Span p = ctts->payload;
    if (p.n < 8) return fail("MP4: bad ctts");
    const std::uint32_t ne = rd32(p.p + 4);
    if (p.n < 8 + std::size_t(ne) * 8) return fail("MP4: truncated ctts");
    t.cto.reserve(N);
    for (std::uint32_t e = 0; e < ne && t.cto.size() < N; ++e) {
      const std::uint32_t cnt = rd32(p.p + 8 + 8 * std::size_t(e));
      const std::int32_t v = std::int32_t(rd32(p.p + 12 + 8 * std::size_t(e)));
      for (std::uint32_t k = 0; k < cnt && t.cto.size() < N; ++k) t.cto.push_back(v);
    }
    if (t.cto.size() != N) t.cto.resize(N, 0);
    t.hasCtts = true;
  }
  // sync samples
  t.sync.assign(N, 1);
  if (stss) {
    const Span p = stss->payload;
    if (p.n < 8) return fail("MP4: bad stss");
    const std::uint32_t ne = rd32(p.p + 4);
    if (p.n < 8 + std::size_t(ne) * 4) return fail("MP4: truncated stss");
    std::fill(t.sync.begin(), t.sync.end(), 0);
    for (std::uint32_t e = 0; e < ne; ++e) {
      const std::uint32_t s = rd32(p.p + 8 + 4 * std::size_t(e));
      if (s >= 1 && s <= N) t.sync[s - 1] = 1;
    }
    t.hasStss = true;
  }
  // offsets
  {
    std::vector<std::uint64_t> chunk;
    if (stco) {
      const Span p = stco->payload;
      const std::uint32_t ne = p.n >= 8 ? rd32(p.p + 4) : 0;
      if (p.n < 8 + std::size_t(ne) * 4) return fail("MP4: truncated stco");
      for (std::uint32_t i = 0; i < ne; ++i) chunk.push_back(rd32(p.p + 8 + 4 * std::size_t(i)));
    } else {
      const Span p = co64->payload;
      const std::uint32_t ne = p.n >= 8 ? rd32(p.p + 4) : 0;
      if (p.n < 8 + std::size_t(ne) * 8) return fail("MP4: truncated co64");
      for (std::uint32_t i = 0; i < ne; ++i) chunk.push_back(rd64(p.p + 8 + 8 * std::size_t(i)));
    }
    const Span p = stsc->payload;
    const std::uint32_t ne = p.n >= 8 ? rd32(p.p + 4) : 0;
    if (ne == 0 || p.n < 8 + std::size_t(ne) * 12) return fail("MP4: bad stsc");
    t.off.resize(N);
    std::size_t sample = 0, e = 0;
    for (std::size_t c = 0; c < chunk.size() && sample < N; ++c) {
      while (e + 1 < ne && rd32(p.p + 8 + 12 * (e + 1)) <= c + 1) ++e;
      const std::uint32_t spc = rd32(p.p + 8 + 12 * e + 4);
      std::uint64_t o = chunk[c];
      for (std::uint32_t k = 0; k < spc && sample < N; ++k) {
        t.off[sample] = o;
        o += t.size[sample];
        ++sample;
      }
    }
    if (sample != N) return fail("MP4: chunk tables do not cover all samples");
  }
  return {};
}

Result<Track> parseTrak(Span trak, bool withSamples) {
  Track t;
  auto tkhd = findChild(trak, cc("tkhd"));
  auto mdia = findChild(trak, cc("mdia"));
  if (!tkhd || !mdia) return fail("MP4: track without tkhd/mdia");
  {
    const Span p = tkhd->payload;
    if (p.n < 84) return fail("MP4: bad tkhd");
    const int v = p.p[0];
    const std::size_t idOff = v == 1 ? 20 : 12;
    const std::size_t matOff = v == 1 ? 52 : 40;
    if (p.n < matOff + 44) return fail("MP4: bad tkhd");
    t.id = rd32(p.p + idOff);
    for (int i = 0; i < 9; ++i) t.matrix[i] = rd32(p.p + matOff + 4 * i);
    t.width16 = rd32(p.p + matOff + 36);
    t.height16 = rd32(p.p + matOff + 40);
  }
  if (auto edts = findChild(trak, cc("edts"))) {
    if (auto elst = findChild(edts->payload, cc("elst"))) {
      const Span p = elst->payload;
      if (p.n >= 8 && rd32(p.p + 4) >= 1) {
        const int v = p.p[0];
        if (v == 0 && p.n >= 20) t.elstMediaTime = std::int32_t(rd32(p.p + 12));
        if (v == 1 && p.n >= 28) t.elstMediaTime = std::int64_t(rd64(p.p + 16));
        t.hasElst = t.elstMediaTime >= 0;
        if (!t.hasElst) t.elstMediaTime = 0;  // empty edit (-1): ignore
      }
    }
  }
  if (auto mdhd = findChild(mdia->payload, cc("mdhd"))) {
    const Span p = mdhd->payload;
    if (p.n >= 24 && p.p[0] == 0) {
      t.timescale = rd32(p.p + 12);
      t.mediaDuration = rd32(p.p + 16);
      t.lang = rd16(p.p + 20);
    } else if (p.n >= 36 && p.p[0] == 1) {
      t.timescale = rd32(p.p + 20);
      t.mediaDuration = rd64(p.p + 24);
      t.lang = rd16(p.p + 32);
    }
  }
  if (t.timescale == 0) return fail("MP4: bad mdhd");
  if (auto hdlr = findChild(mdia->payload, cc("hdlr"))) {
    t.hdlrRaw = toBytes(hdlr->whole);
    if (hdlr->payload.n >= 12) t.handler = ccStr(rd32(hdlr->payload.p + 8));
  }
  auto minf = findChild(mdia->payload, cc("minf"));
  if (!minf) return fail("MP4: track without minf");
  children(minf->payload, [&](const BoxView& b) {
    if (b.type == cc("vmhd") || b.type == cc("smhd") || b.type == cc("nmhd") || b.type == cc("hmhd") || b.type == cc("sthd"))
      t.mediaHdrRaw = toBytes(b.whole);
    if (b.type == cc("dinf")) t.dinfRaw = toBytes(b.whole);
  });
  auto stbl = findChild(minf->payload, cc("stbl"));
  if (!stbl) return fail("MP4: track without stbl");
  if (auto stsd = findChild(stbl->payload, cc("stsd"))) {
    t.stsdRaw = toBytes(stsd->whole);
    if (stsd->payload.n >= 16) t.codec = rd32(stsd->payload.p + 12);
  }
  if (withSamples) {
    if (auto r = parseSamples(stbl->payload, t); !r) return std::unexpected(r.error());
  } else if (auto stsz = findChild(stbl->payload, cc("stsz"))) {
    // enough for fps / counts without expanding tables
    (void)stsz;
  }
  return t;
}

struct Movie {
  Bytes moov;  // payload of the moov box
  std::uint32_t timescale = 1000;
  std::uint64_t duration = 0;
  std::vector<Span> traks;
};

Result<Bytes> readMoovPayload(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("Cannot open " + pathToUtf8(path));
  in.seekg(0, std::ios::end);
  const std::uint64_t fileSize = std::uint64_t(in.tellg());
  std::uint64_t pos = 0;
  bool sawFtyp = false;
  while (pos + 8 <= fileSize) {
    std::uint8_t h[16];
    in.seekg(std::streamoff(pos));
    in.read(reinterpret_cast<char*>(h), 16);
    const std::size_t got = std::size_t(in.gcount());
    in.clear();
    if (got < 8) break;
    std::uint64_t sz = rd32(h);
    const std::uint32_t type = rd32(h + 4);
    std::size_t hdr = 8;
    if (sz == 1) {
      if (got < 16) break;
      sz = rd64(h + 8);
      hdr = 16;
    } else if (sz == 0) {
      sz = fileSize - pos;
    }
    if (sz < hdr || pos + sz > fileSize) return fail("MP4: box exceeds file size (truncated file?)");
    if (type == cc("ftyp")) sawFtyp = true;
    if (type == cc("moov")) {
      if (sz > (std::uint64_t(1) << 28)) return fail("MP4: moov too large");
      Bytes b(std::size_t(sz - hdr));
      in.seekg(std::streamoff(pos + hdr));
      in.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
      if (!in) return fail("MP4: cannot read moov");
      return b;
    }
    pos += sz;
  }
  return fail(sawFtyp ? "MP4: no moov box (recording not finalized?)" : "Not an MP4/MOV file");
}

bool isVideoCodec(std::uint32_t c) {
  return c == cc("avc1") || c == cc("avc3") || c == cc("hvc1") || c == cc("hev1") || c == cc("av01");
}

struct Parsed {
  Bytes moov;
  Track video, audio;
  bool hasVideo = false, hasAudio = false;
  std::string audioCodecName;
  std::uint32_t movieTimescale = 1000;
};

Result<Parsed> parseMovie(const std::filesystem::path& path, bool needAudioSamples) {
  auto moov = readMoovPayload(path);
  if (!moov) return std::unexpected(moov.error());
  Parsed p;
  p.moov = std::move(*moov);
  const Span ms{p.moov.data(), p.moov.size()};
  if (auto mvhd = findChild(ms, cc("mvhd"))) {
    const Span q = mvhd->payload;
    if (q.n >= 20) p.movieTimescale = q.p[0] == 1 ? rd32(q.p + 20) : rd32(q.p + 12);
  }
  Result<void> err;
  children(ms, [&](const BoxView& b) {
    if (b.type != cc("trak") || !err) return;
    auto head = parseTrak(b.payload, false);
    if (!head) return;  // ignore unparseable auxiliary tracks
    if (head->handler == "vide" && isVideoCodec(head->codec) && !p.hasVideo) {
      auto full = parseTrak(b.payload, true);
      if (!full) {
        err = std::unexpected(full.error());
        return;
      }
      p.video = std::move(*full);
      p.hasVideo = true;
    } else if (head->handler == "soun" && !p.hasAudio) {
      p.audioCodecName = ccStr(head->codec);
      p.hasAudio = true;
      if (head->codec == cc("mp4a") && needAudioSamples) {
        auto full = parseTrak(b.payload, true);
        if (!full) {
          err = std::unexpected(full.error());
          return;
        }
        p.audio = std::move(*full);
      } else {
        p.audio = std::move(*head);
      }
    }
  });
  if (!err) return std::unexpected(err.error());
  if (!p.hasVideo) return fail("MP4: no H.264/HEVC/AV1 video track (stream copy not possible)");
  if (p.video.size.empty()) return fail("MP4: empty video track");
  return p;
}

// ---------------------------------------------------------------------------
// Box writer
// ---------------------------------------------------------------------------
struct W {
  Bytes b;
  std::size_t begin(std::uint32_t type) {
    const std::size_t pos = b.size();
    u32(0);
    u32(type);
    return pos;
  }
  void end(std::size_t pos) {
    const std::uint32_t sz = std::uint32_t(b.size() - pos);
    b[pos] = std::uint8_t(sz >> 24);
    b[pos + 1] = std::uint8_t(sz >> 16);
    b[pos + 2] = std::uint8_t(sz >> 8);
    b[pos + 3] = std::uint8_t(sz);
  }
  void u8(std::uint32_t v) { b.push_back(std::uint8_t(v)); }
  void u16(std::uint32_t v) { u8(v >> 8), u8(v); }
  void u32(std::uint32_t v) { u16(v >> 16), u16(v); }
  void raw(const Bytes& r) { b.insert(b.end(), r.begin(), r.end()); }
  void zeros(std::size_t n) { b.insert(b.end(), n, 0); }
  void fullBox(std::size_t& pos, std::uint32_t type, std::uint32_t versionFlags) {
    pos = begin(type);
    u32(versionFlags);
  }
};

struct OutSample {
  std::uint32_t size;
  std::uint32_t delta;  // to next sample
  std::int32_t cto;
  bool sync;
};
struct OutTrack {
  const Track* src = nullptr;
  bool video = false;
  std::vector<OutSample> s;
  std::vector<std::uint64_t> srcOff;
  std::vector<std::uint32_t> chunkOff;  // absolute offset in the output file, per sample
  std::int64_t editMediaTime = 0;       // video: earliest presentation time (media timescale units)
  std::uint64_t mediaDuration = 0;
};

void writeStbl(W& w, const OutTrack& t) {
  const std::size_t stbl = w.begin(cc("stbl"));
  w.raw(t.src->stsdRaw);
  std::size_t p;
  // stts
  {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    for (const auto& s : t.s) {
      if (!runs.empty() && runs.back().second == s.delta) ++runs.back().first;
      else runs.push_back({1, s.delta});
    }
    w.fullBox(p, cc("stts"), 0);
    w.u32(std::uint32_t(runs.size()));
    for (auto& r : runs) w.u32(r.first), w.u32(r.second);
    w.end(p);
  }
  bool anyCto = false, negCto = false;
  for (const auto& s : t.s) {
    anyCto |= s.cto != 0;
    negCto |= s.cto < 0;
  }
  if (t.video && anyCto) {
    std::vector<std::pair<std::uint32_t, std::int32_t>> runs;
    for (const auto& s : t.s) {
      if (!runs.empty() && runs.back().second == s.cto) ++runs.back().first;
      else runs.push_back({1, s.cto});
    }
    w.fullBox(p, cc("ctts"), negCto ? 0x01000000u : 0u);
    w.u32(std::uint32_t(runs.size()));
    for (auto& r : runs) w.u32(r.first), w.u32(std::uint32_t(r.second));
    w.end(p);
  }
  if (t.video) {
    std::vector<std::uint32_t> sync;
    for (std::size_t i = 0; i < t.s.size(); ++i)
      if (t.s[i].sync) sync.push_back(std::uint32_t(i + 1));
    if (sync.size() != t.s.size()) {
      w.fullBox(p, cc("stss"), 0);
      w.u32(std::uint32_t(sync.size()));
      for (auto v : sync) w.u32(v);
      w.end(p);
    }
  }
  w.fullBox(p, cc("stsc"), 0);  // one sample per chunk
  w.u32(1), w.u32(1), w.u32(1), w.u32(1);
  w.end(p);
  w.fullBox(p, cc("stsz"), 0);
  w.u32(0);
  w.u32(std::uint32_t(t.s.size()));
  for (const auto& s : t.s) w.u32(s.size);
  w.end(p);
  w.fullBox(p, cc("stco"), 0);
  w.u32(std::uint32_t(t.chunkOff.size()));
  for (auto o : t.chunkOff) w.u32(o);
  w.end(p);
  w.end(stbl);
}

std::uint64_t toMovie(std::uint64_t v, std::uint32_t ts) { return v * 1000 / ts; }

Bytes buildMoov(const std::vector<OutTrack>& tracks, std::uint32_t nextTrackId) {
  W w;
  const std::size_t moov = w.begin(cc("moov"));
  std::uint64_t movieDur = 0;
  for (const auto& t : tracks) movieDur = std::max(movieDur, toMovie(t.mediaDuration, t.src->timescale));
  std::size_t p;
  w.fullBox(p, cc("mvhd"), 0);
  w.u32(0), w.u32(0), w.u32(1000), w.u32(std::uint32_t(movieDur));
  w.u32(0x00010000), w.u16(0x0100), w.u16(0), w.zeros(8);
  const std::uint32_t mat[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  for (auto m : mat) w.u32(m);
  w.zeros(24);
  w.u32(nextTrackId);
  w.end(p);
  for (const auto& t : tracks) {
    const Track& s = *t.src;
    const std::size_t trak = w.begin(cc("trak"));
    const std::uint64_t dur = toMovie(t.mediaDuration, s.timescale);
    w.fullBox(p, cc("tkhd"), 3);
    w.u32(0), w.u32(0), w.u32(s.id), w.u32(0), w.u32(std::uint32_t(dur));
    w.zeros(8);
    w.u16(0), w.u16(0), w.u16(t.video ? 0 : 0x0100), w.u16(0);
    for (int i = 0; i < 9; ++i) w.u32(t.video ? s.matrix[i] : mat[i]);
    w.u32(t.video ? s.width16 : 0), w.u32(t.video ? s.height16 : 0);
    w.end(p);
    if (t.video && t.editMediaTime > 0) {
      const std::size_t edts = w.begin(cc("edts"));
      w.fullBox(p, cc("elst"), 0);
      w.u32(1);
      const std::uint64_t segMedia = t.mediaDuration > std::uint64_t(t.editMediaTime) ? t.mediaDuration - t.editMediaTime : 0;
      w.u32(std::uint32_t(toMovie(segMedia, s.timescale)));
      w.u32(std::uint32_t(t.editMediaTime));
      w.u32(0x00010000);
      w.end(p);
      w.end(edts);
    }
    const std::size_t mdia = w.begin(cc("mdia"));
    w.fullBox(p, cc("mdhd"), 0);
    w.u32(0), w.u32(0), w.u32(s.timescale), w.u32(std::uint32_t(t.mediaDuration)), w.u16(s.lang), w.u16(0);
    w.end(p);
    w.raw(s.hdlrRaw);
    const std::size_t minf = w.begin(cc("minf"));
    w.raw(s.mediaHdrRaw);
    w.raw(s.dinfRaw);
    writeStbl(w, t);
    w.end(minf);
    w.end(mdia);
    w.end(trak);
  }
  w.end(moov);
  return w.b;
}

}  // namespace

// ---------------------------------------------------------------------------
// probe / trim
// ---------------------------------------------------------------------------
Result<VideoInfo> probeVideo(const std::filesystem::path& mp4) {
  try {
    auto m = parseMovie(mp4, false);
    if (!m) return std::unexpected(m.error());
    VideoInfo v;
    const Track& t = m->video;
    v.durationSec = double(t.mediaDuration) / t.timescale;
    v.width = int(t.width16 >> 16);
    v.height = int(t.height16 >> 16);
    const double a = double(std::int32_t(t.matrix[0])) / 65536.0, b = double(std::int32_t(t.matrix[1])) / 65536.0;
    int rot = int(std::lround(std::atan2(b, a) * 180.0 / 3.14159265358979));
    v.rotationDeg = ((rot % 360) + 360) % 360;
    v.fps = t.size.size() > 1 && t.dts.back() > 0 ? double(t.size.size() - 1) * t.timescale / double(t.dts.back()) : 0;
    v.videoCodec = ccStr(t.codec);
    v.audioCodec = m->hasAudio ? m->audioCodecName : "";
    for (std::size_t i = 0; i < t.size.size(); ++i)
      if (t.sync[i]) v.keyframesSec.push_back(std::max(0.0, t.ptsSec(i)));
    std::sort(v.keyframesSec.begin(), v.keyframesSec.end());
    v.streamCopyOk = true;
    return v;
  } catch (const std::exception& e) {
    return fail(std::string("probeVideo failed: ") + e.what());
  }
}

Result<TrimResult> trimMp4(const std::filesystem::path& input, const TrimOptions& opt) {
  try {
    if (opt.durationSec <= 0) return fail("Trim duration must be positive");
    auto pm = parseMovie(input, opt.keepAudio);
    if (!pm) return std::unexpected(pm.error());
    Track& v = pm->video;
    const std::size_t N = v.size.size();

    // sync sample indices with presentation times
    std::vector<std::size_t> keys;
    for (std::size_t i = 0; i < N; ++i)
      if (v.sync[i]) keys.push_back(i);
    if (keys.empty()) return fail("MP4: video has no sync samples");
    auto keyPts = [&](std::size_t k) { return v.ptsSec(keys[k]); };

    // start key: nearest to the requested start (prefer the one at or before it when equally close)
    const double total = double(v.mediaDuration) / v.timescale;
    double want = std::clamp(opt.startSec, 0.0, std::max(0.0, total - 0.1));
    std::size_t ks = 0;
    for (std::size_t k = 0; k < keys.size(); ++k)
      if (std::fabs(keyPts(k) - want) < std::fabs(keyPts(ks) - want) - 1e-9) ks = k;
    // end key: nearest key after ks to start + duration
    std::size_t ke = keys.size();  // sentinel = end of stream
    {
      const double target = keyPts(ks) + opt.durationSec;
      double bestD = std::fabs(total - target);
      for (std::size_t k = ks + 1; k < keys.size(); ++k) {
        const double d = std::fabs(keyPts(k) - target);
        if (d < bestD) {
          bestD = d;
          ke = k;
        }
      }
    }
    const std::size_t s0 = keys[ks], s1 = ke == keys.size() ? N : keys[ke];
    if (s1 <= s0) return fail("MP4: empty window");

    OutTrack ov;
    ov.src = &v;
    ov.video = true;
    std::int64_t minPts = INT64_MAX;
    const std::uint64_t dts0 = v.dts[s0];
    for (std::size_t i = s0; i < s1; ++i) {
      OutSample s;
      s.size = v.size[i];
      const std::uint64_t next = i + 1 < N ? v.dts[i + 1] : v.dts[i] + v.lastDelta;
      s.delta = std::uint32_t(next - v.dts[i]);
      s.cto = v.cto.empty() ? 0 : v.cto[i];
      s.sync = v.sync[i] != 0;
      ov.s.push_back(s);
      ov.srcOff.push_back(v.off[i]);
      minPts = std::min<std::int64_t>(minPts, std::int64_t(v.dts[i] - dts0) + s.cto);
    }
    ov.mediaDuration = (s1 < N ? v.dts[s1] : v.dts[N - 1] + v.lastDelta) - dts0;
    ov.editMediaTime = std::max<std::int64_t>(0, minPts);
    const double clipStartPts = v.ptsSec(s0) + double(ov.editMediaTime) / v.timescale;  // earliest pts in the window
    (void)clipStartPts;
    // earliest presentation time among kept frames, in source time
    double firstPts = 1e18;
    for (std::size_t i = s0; i < s1; ++i) firstPts = std::min(firstPts, v.ptsSec(i));

    TrimResult out;
    out.actualStartSec = firstPts;
    out.actualDurationSec = double(ov.mediaDuration - ov.editMediaTime) / v.timescale;
    out.videoFrames = int(s1 - s0);

    std::vector<OutTrack> tracks;
    tracks.push_back(std::move(ov));
    OutTrack oa;
    if (pm->hasAudio) {
      if (!opt.keepAudio) {
        out.note = "audio removed on request";
      } else if (pm->audio.codec != cc("mp4a") || pm->audio.size.empty()) {
        out.note = "audio (" + pm->audioCodecName + ") dropped: Motion Photo needs AAC";
      } else {
        Track& a = pm->audio;
        const double p0 = firstPts, p1 = firstPts + out.actualDurationSec;
        std::size_t a0 = a.size.size(), a1 = a.size.size();
        for (std::size_t i = 0; i < a.size.size(); ++i) {
          const double t = a.ptsSec(i);
          if (a0 == a.size.size() && t >= p0 - 1e-6) a0 = i;
          if (t >= p1 - 1e-6) {
            a1 = i;
            break;
          }
        }
        if (a0 < a1) {
          oa.src = &a;
          oa.video = false;
          const std::uint64_t d0 = a.dts[a0];
          for (std::size_t i = a0; i < a1; ++i) {
            OutSample s;
            s.size = a.size[i];
            const std::uint64_t next = i + 1 < a.size.size() ? a.dts[i + 1] : a.dts[i] + a.lastDelta;
            s.delta = std::uint32_t(next - a.dts[i]);
            s.cto = 0;
            s.sync = true;
            oa.s.push_back(s);
            oa.srcOff.push_back(a.off[i]);
          }
          oa.mediaDuration = (a1 < a.size.size() ? a.dts[a1] : a.dts.back() + a.lastDelta) - d0;
          out.audioKept = true;
        }
      }
    }
    if (out.audioKept) tracks.push_back(std::move(oa));
    for (auto& t : tracks)
      if (t.src == &pm->audio) t.src = &pm->audio;

    // interleave samples by time
    struct Ref { double t; int track; std::size_t idx; };
    std::vector<Ref> order;
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
      double acc = 0;
      for (std::size_t i = 0; i < tracks[ti].s.size(); ++i) {
        order.push_back({acc, ti, i});
        acc += double(tracks[ti].s[i].delta) / tracks[ti].src->timescale;
      }
    }
    std::stable_sort(order.begin(), order.end(), [](const Ref& a, const Ref& b) { return a.t < b.t; });
    for (auto& t : tracks) t.chunkOff.assign(t.s.size(), 0);

    Bytes ftyp;
    {
      W w;
      const std::size_t p = w.begin(cc("ftyp"));
      w.u32(cc("isom")), w.u32(0x200), w.u32(cc("isom")), w.u32(cc("iso2")), w.u32(cc("mp41")), w.u32(cc("mp42"));
      w.end(p);
      ftyp = w.b;
    }
    const std::uint32_t nextId = 1 + std::max(pm->video.id, out.audioKept ? pm->audio.id : 0u);
    Bytes moov = buildMoov(tracks, nextId);
    const std::uint64_t dataStart = ftyp.size() + moov.size() + 8;
    std::uint64_t cur = dataStart;
    std::uint64_t dataBytes = 0;
    for (const auto& r : order) {
      tracks[r.track].chunkOff[r.idx] = std::uint32_t(cur);
      cur += tracks[r.track].s[r.idx].size;
      dataBytes += tracks[r.track].s[r.idx].size;
    }
    if (cur > 0xFFFFFFF0ull) return fail("Clip too large for 32-bit offsets");
    moov = buildMoov(tracks, nextId);
    if (ftyp.size() + moov.size() + 8 != dataStart) return fail("internal error: moov size changed");

    std::ifstream in(input, std::ios::binary);
    if (!in) return fail("Cannot open " + pathToUtf8(input));
    out.mp4.reserve(std::size_t(dataStart + dataBytes));
    out.mp4.insert(out.mp4.end(), ftyp.begin(), ftyp.end());
    out.mp4.insert(out.mp4.end(), moov.begin(), moov.end());
    {
      W w;
      w.u32(std::uint32_t(8 + dataBytes));
      w.u32(cc("mdat"));
      out.mp4.insert(out.mp4.end(), w.b.begin(), w.b.end());
    }
    for (const auto& r : order) {
      const OutTrack& t = tracks[r.track];
      const std::size_t at = out.mp4.size();
      out.mp4.resize(at + t.s[r.idx].size);
      in.seekg(std::streamoff(t.srcOff[r.idx]));
      in.read(reinterpret_cast<char*>(out.mp4.data() + at), std::streamsize(t.s[r.idx].size));
      if (!in) return fail("MP4: sample data beyond end of file (truncated recording?)");
    }
    return out;
  } catch (const std::exception& e) {
    return fail(std::string("Trim failed: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// JPEG helpers + container
// ---------------------------------------------------------------------------
namespace {

constexpr std::size_t npos = std::size_t(-1);

// Offset just after the EOI that terminates the first image in the buffer.
std::size_t jpegEnd(const std::uint8_t* d, std::size_t n) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return npos;
  std::size_t pos = 2;
  while (pos + 1 < n) {
    if (d[pos] != 0xFF) {
      ++pos;
      continue;
    }
    const std::uint8_t m = d[pos + 1];
    if (m == 0xFF) {
      ++pos;
      continue;
    }
    if (m == 0x00 || (m >= 0xD0 && m <= 0xD8) || m == 0x01) {
      pos += 2;
      continue;
    }
    if (m == 0xD9) return pos + 2;
    if (pos + 4 > n) return npos;
    pos += 2 + ((std::size_t(d[pos + 2]) << 8) | d[pos + 3]);
  }
  return npos;
}

struct JSeg {
  std::uint8_t marker;
  std::size_t start, end;  // [start,end) including FF xx and length
};

// Segments from SOI up to (not including) SOS.
std::vector<JSeg> headerSegments(const std::uint8_t* d, std::size_t n, std::size_t& sosAt) {
  std::vector<JSeg> segs;
  std::size_t pos = 2;
  sosAt = npos;
  while (pos + 4 <= n && d[pos] == 0xFF) {
    const std::uint8_t m = d[pos + 1];
    if (m == 0xDA) {
      sosAt = pos;
      break;
    }
    const std::size_t len = (std::size_t(d[pos + 2]) << 8) | d[pos + 3];
    if (pos + 2 + len > n) break;
    segs.push_back({m, pos, pos + 2 + len});
    pos += 2 + len;
  }
  return segs;
}

const char kXmpHeader[] = "http://ns.adobe.com/xap/1.0/";  // followed by NUL

bool isXmpSegment(const std::uint8_t* d, const JSeg& s) {
  return s.marker == 0xE1 && s.end - s.start >= 4 + sizeof(kXmpHeader) &&
         std::memcmp(d + s.start + 4, kXmpHeader, sizeof(kXmpHeader)) == 0;  // includes the NUL
}
bool isExifSegment(const std::uint8_t* d, const JSeg& s) {
  return s.marker == 0xE1 && s.end - s.start >= 10 && std::memcmp(d + s.start + 4, "Exif\0\0", 6) == 0;
}

std::string makeXmp(std::uint64_t videoLength, const std::string& videoMime, std::int64_t ptsUs, bool legacy) {
  std::string x;
  x += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
  x += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Adobe XMP Core 5.1.0-jc003\">\n";
  x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  x += "  <rdf:Description rdf:about=\"\"\n";
  x += "    xmlns:GCamera=\"http://ns.google.com/photos/1.0/camera/\"\n";
  x += "    xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n";
  x += "    xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n";
  x += "    GCamera:MotionPhoto=\"1\"\n";
  x += "    GCamera:MotionPhotoVersion=\"1\"\n";
  if (legacy) {
    x += "    GCamera:MicroVideo=\"1\"\n    GCamera:MicroVideoVersion=\"1\"\n";
    x += "    GCamera:MicroVideoOffset=\"" + std::to_string(videoLength) + "\"\n";
    if (ptsUs >= 0) x += "    GCamera:MicroVideoPresentationTimestampUs=\"" + std::to_string(ptsUs) + "\"\n";
  }
  x += "    GCamera:MotionPhotoPresentationTimestampUs=\"" + std::to_string(ptsUs) + "\">\n";
  x += "   <Container:Directory>\n    <rdf:Seq>\n";
  x += "     <rdf:li rdf:parseType=\"Resource\">\n      <Container:Item\n        Item:Mime=\"image/jpeg\"\n";
  x += "        Item:Semantic=\"Primary\"\n        Item:Length=\"0\"\n        Item:Padding=\"0\"/>\n     </rdf:li>\n";
  x += "     <rdf:li rdf:parseType=\"Resource\">\n      <Container:Item\n        Item:Mime=\"" + videoMime + "\"\n";
  x += "        Item:Semantic=\"MotionPhoto\"\n        Item:Length=\"" + std::to_string(videoLength) +
       "\"\n        Item:Padding=\"0\"/>\n     </rdf:li>\n";
  x += "    </rdf:Seq>\n   </Container:Directory>\n  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
  return x;
}

std::string attr(const std::string& s, const std::string& name, std::size_t from = 0, std::size_t to = std::string::npos) {
  const std::string key = name + "=\"";
  const std::size_t a = s.find(key, from);
  if (a == std::string::npos || (to != std::string::npos && a >= to)) return {};
  const std::size_t b = s.find('"', a + key.size());
  return b == std::string::npos ? std::string() : s.substr(a + key.size(), b - a - key.size());
}

// The MP4 box layout of an in-memory clip: validates top-level structure and extracts track facts.
void inspectMp4(const Bytes& mp4, MotionPhotoInfo& info) {
  std::size_t pos = 0;
  bool ftyp = false, moov = false, mdat = false;
  std::uint64_t mdatStart = 0, mdatEnd = 0;
  const Span all{mp4.data(), mp4.size()};
  std::size_t consumed = 0;
  children(all, [&](const BoxView& b) {
    consumed += b.whole.n;
    if (b.type == cc("ftyp")) ftyp = true;
    if (b.type == cc("mdat")) {
      mdat = true;
      mdatStart = std::uint64_t(b.payload.p - mp4.data());
      mdatEnd = mdatStart + b.payload.n;
    }
    if (b.type == cc("moov")) moov = true;
  });
  (void)pos;
  if (!ftyp) info.problems.push_back("video: missing ftyp box");
  if (!moov) info.problems.push_back("video: missing moov box");
  if (!mdat) info.problems.push_back("video: missing mdat box");
  if (consumed != mp4.size()) info.problems.push_back("video: top-level boxes do not cover the appended bytes exactly");
  if (!moov) return;
  Span moovPayload;
  children(all, [&](const BoxView& b) {
    if (b.type == cc("moov")) moovPayload = b.payload;
  });
  bool haveVideo = false;
  children(moovPayload, [&](const BoxView& b) {
    if (b.type != cc("trak")) return;
    auto head = parseTrak(b.payload, false);
    if (!head) {
      info.problems.push_back("video: unreadable track: " + head.error().message);
      return;
    }
    if (head->handler == "vide" && isVideoCodec(head->codec) && !haveVideo) {
      haveVideo = true;
      info.videoCodec = ccStr(head->codec);
      info.videoWidth = int(head->width16 >> 16);
      info.videoHeight = int(head->height16 >> 16);
      info.videoDurationSec = double(head->mediaDuration) / head->timescale;
      auto full = parseTrak(b.payload, true);
      if (!full) {
        info.problems.push_back("video: " + full.error().message);
        return;
      }
      for (std::size_t i = 0; i < full->size.size(); ++i)
        if (full->off[i] < mdatStart || full->off[i] + full->size[i] > mdatEnd) {
          info.problems.push_back("video: sample " + std::to_string(i) + " lies outside mdat (offsets must be relative to the start of the appended MP4)");
          break;
        }
      if (full->size.empty()) info.problems.push_back("video: no samples");
      else if (!full->sync[0]) info.problems.push_back("video: first sample is not a sync sample");
    } else if (head->handler == "soun" && info.audioCodec.empty()) {
      info.audioCodec = ccStr(head->codec);
    }
  });
  if (!haveVideo) info.problems.push_back("video: no H.264/HEVC/AV1 video track");
  if (!info.audioCodec.empty() && info.audioCodec != "mp4a")
    info.notes.push_back("audio track is " + info.audioCodec + " (spec expects AAC)");
}

}  // namespace

namespace {

// ---- Ultra HDR (JPEG_R) cover: primary JPEG + MPF + gain-map JPEG, with a Container directory in the primary's XMP -------
// The Motion Photo directory then lists Primary, GainMap and MotionPhoto (video last), as Pixel's Ultra HDR motion
// photos do. The primary grows by the new XMP, so the MPF entries (primary size, gain-map offset) are rewritten.

// Adds the Motion Photo attributes and the video item to an existing XMP packet that has a Container directory.
// Returns an empty string when the packet has no Container:Directory sequence to extend.
std::string mergeMotionIntoXmp(const std::string& xmp, std::uint64_t videoLength, const std::string& videoMime, std::int64_t ptsUs,
                               bool legacy) {
  const std::size_t desc = xmp.find("<rdf:Description");
  if (desc == std::string::npos || xmp.find("Container:Directory") == std::string::npos) return {};
  std::size_t seqEnd = xmp.find("</rdf:Seq>", desc);
  const std::size_t descEnd = xmp.find('>', desc);
  if (seqEnd == std::string::npos || descEnd == std::string::npos || descEnd > seqEnd) return {};
  std::string add;
  if (xmp.find("xmlns:GCamera=") == std::string::npos) add += " xmlns:GCamera=\"http://ns.google.com/photos/1.0/camera/\"";
  add += " GCamera:MotionPhoto=\"1\" GCamera:MotionPhotoVersion=\"1\"";
  if (legacy) {
    add += " GCamera:MicroVideo=\"1\" GCamera:MicroVideoVersion=\"1\" GCamera:MicroVideoOffset=\"" + std::to_string(videoLength) + "\"";
    if (ptsUs >= 0) add += " GCamera:MicroVideoPresentationTimestampUs=\"" + std::to_string(ptsUs) + "\"";
  }
  add += " GCamera:MotionPhotoPresentationTimestampUs=\"" + std::to_string(ptsUs) + "\"";
  const std::size_t at = xmp[descEnd - 1] == '/' ? descEnd - 1 : descEnd;
  std::string out = xmp.substr(0, at) + add + xmp.substr(at);
  seqEnd += add.size();
  const std::string li = "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Mime=\"" + videoMime +
                         "\" Item:Semantic=\"MotionPhoto\" Item:Length=\"" + std::to_string(videoLength) +
                         "\" Item:Padding=\"0\"/></rdf:li>";
  out.insert(seqEnd, li);
  return out;
}

// Rewrites the MPF index of a primary JPEG whose total length changed. primaryLen = SOI..EOI of the new primary.
bool patchMpf(Bytes& primary, std::size_t primaryLen) {
  std::size_t sosAt = npos;
  const auto segs = headerSegments(primary.data(), primary.size(), sosAt);
  for (const auto& sg : segs) {
    if (sg.marker != 0xE2 || sg.end - sg.start < 4 + 4 + 8) continue;
    if (std::memcmp(primary.data() + sg.start + 4, "MPF\0", 4) != 0) continue;
    const std::size_t T = sg.start + 8;  // start of the TIFF header (endian marker): offsets are relative to it
    const bool le = primary[T] == 'I';
    auto r16 = [&](std::size_t o) { return le ? std::uint16_t(primary[o] | (primary[o + 1] << 8)) : rd16(&primary[o]); };
    auto r32 = [&](std::size_t o) {
      return le ? std::uint32_t(primary[o] | (primary[o + 1] << 8) | (primary[o + 2] << 16) | (std::uint32_t(primary[o + 3]) << 24))
                : rd32(&primary[o]);
    };
    auto w32 = [&](std::size_t o, std::uint32_t v) {
      if (le) {
        primary[o] = v & 0xff; primary[o + 1] = (v >> 8) & 0xff; primary[o + 2] = (v >> 16) & 0xff; primary[o + 3] = std::uint8_t(v >> 24);
      } else {
        primary[o] = std::uint8_t(v >> 24); primary[o + 1] = (v >> 16) & 0xff; primary[o + 2] = (v >> 8) & 0xff; primary[o + 3] = v & 0xff;
      }
    };
    const std::size_t ifd = T + r32(T + 4);
    if (ifd + 2 > sg.end) return false;
    const unsigned n = r16(ifd);
    for (unsigned i = 0; i < n; i++) {
      const std::size_t e = ifd + 2 + i * 12;
      if (e + 12 > sg.end) return false;
      if (r16(e) != 0xB002) continue;  // MP Entry
      const std::size_t arr = T + r32(e + 8);
      const unsigned cnt = r32(e + 4) / 16;
      if (cnt < 2 || arr + 32 > sg.end) return false;
      w32(arr + 4, std::uint32_t(primaryLen));              // entry 0: size of the primary
      w32(arr + 16 + 8, std::uint32_t(primaryLen - T));     // entry 1: gain map offset, relative to the MPF header
      return true;
    }
  }
  return false;
}

}  // namespace

Result<Bytes> buildMotionPhoto(const Bytes& jpeg, const Bytes& mp4, const MotionPhotoOptions& opt) {
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return fail("Cover is not a JPEG");
  if (mp4.size() < 16 || rd32(mp4.data() + 4) != cc("ftyp")) return fail("Video does not look like an MP4/MOV (no ftyp)");
  const std::size_t end = jpegEnd(jpeg.data(), jpeg.size());
  if (end == npos) return fail("Cover JPEG is truncated (no EOI marker)");

  // an Ultra HDR cover carries its gain map as a second JPEG right after the primary's EOI
  Bytes gainMap;
  if (jpeg.size() > end + 4 && jpeg[end] == 0xFF && jpeg[end + 1] == 0xD8) {
    const std::size_t gEnd = jpegEnd(jpeg.data() + end, jpeg.size() - end);
    if (gEnd != npos) gainMap.assign(jpeg.begin() + std::ptrdiff_t(end), jpeg.begin() + std::ptrdiff_t(end + gEnd));
  }

  std::size_t sos = npos;
  const auto segs = headerSegments(jpeg.data(), end, sos);
  if (sos == npos) return fail("Cover JPEG has no scan data");

  // leading APP0 / Exif segments stay in front of the XMP packet
  std::size_t insertAt = 2;
  for (std::size_t k = 0; k < segs.size(); ++k) {
    if (segs[k].marker == 0xE0 || isExifSegment(jpeg.data(), segs[k])) insertAt = segs[k].end;
    else break;
  }

  const bool isMov = rd32(mp4.data() + 8) == cc("qt  ");
  const std::string mime = isMov ? "video/quicktime" : "video/mp4";

  // Ultra HDR: extend the XMP the cover already has (its Container directory lists the gain map)
  std::string xmp;
  const JSeg* oldXmp = nullptr;
  if (!gainMap.empty()) {
    for (const auto& sg : segs)
      if (isXmpSegment(jpeg.data(), sg)) { oldXmp = &sg; break; }
    if (oldXmp) {
      const std::string cur(reinterpret_cast<const char*>(jpeg.data() + oldXmp->start + 4 + sizeof(kXmpHeader)),
                            oldXmp->end - oldXmp->start - 4 - sizeof(kXmpHeader));
      xmp = mergeMotionIntoXmp(cur, mp4.size(), mime, opt.presentationTimestampUs, opt.legacyMicroVideo);
    }
    if (xmp.empty()) gainMap.clear();  // cannot describe it: keep the primary image alone (a plain Motion Photo)
  }
  if (gainMap.empty()) {
    xmp = makeXmp(mp4.size(), mime, opt.presentationTimestampUs, opt.legacyMicroVideo);
    oldXmp = nullptr;
  }
  const std::size_t payload = sizeof(kXmpHeader) + xmp.size();  // header incl. NUL + packet
  if (payload + 2 > 0xFFFF) return fail("XMP packet too large");

  Bytes out;
  out.reserve(end + payload + 8 + gainMap.size() + mp4.size());
  if (!gainMap.empty() && oldXmp) {
    // swap the cover's own XMP segment for the merged one, in place
    out.insert(out.end(), jpeg.begin(), jpeg.begin() + std::ptrdiff_t(oldXmp->start));
    out.push_back(0xFF);
    out.push_back(0xE1);
    out.push_back(std::uint8_t((payload + 2) >> 8));
    out.push_back(std::uint8_t(payload + 2));
    out.insert(out.end(), kXmpHeader, kXmpHeader + sizeof(kXmpHeader));
    out.insert(out.end(), xmp.begin(), xmp.end());
    out.insert(out.end(), jpeg.begin() + std::ptrdiff_t(oldXmp->end), jpeg.begin() + std::ptrdiff_t(end));
    if (!patchMpf(out, out.size())) return fail("Ultra HDR cover: MPF index not found or malformed");
    out.insert(out.end(), gainMap.begin(), gainMap.end());
    out.insert(out.end(), mp4.begin(), mp4.end());
    return out;
  }
  out.insert(out.end(), jpeg.begin(), jpeg.begin() + std::ptrdiff_t(insertAt));
  out.push_back(0xFF);
  out.push_back(0xE1);
  out.push_back(std::uint8_t((payload + 2) >> 8));
  out.push_back(std::uint8_t(payload + 2));
  out.insert(out.end(), kXmpHeader, kXmpHeader + sizeof(kXmpHeader));  // with NUL
  out.insert(out.end(), xmp.begin(), xmp.end());
  // the rest of the header segments except old XMP packets, then scan data up to EOI
  std::size_t pos = insertAt;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (segs[i].start < insertAt) continue;
    if (isXmpSegment(jpeg.data(), segs[i])) {
      out.insert(out.end(), jpeg.begin() + std::ptrdiff_t(pos), jpeg.begin() + std::ptrdiff_t(segs[i].start));
      pos = segs[i].end;
    }
  }
  out.insert(out.end(), jpeg.begin() + std::ptrdiff_t(pos), jpeg.begin() + std::ptrdiff_t(end));
  out.insert(out.end(), mp4.begin(), mp4.end());
  return out;
}

MotionPhotoInfo validateMotionPhoto(const Bytes& f) {
  MotionPhotoInfo info;
  info.fileSize = f.size();
  auto bad = [&](const std::string& s) { info.problems.push_back(s); };
  try {
    if (f.size() < 64 || f[0] != 0xFF || f[1] != 0xD8) {
      bad("not a JPEG (no SOI)");
      return info;
    }
    const std::size_t end = jpegEnd(f.data(), f.size());
    if (end == npos) {
      bad("JPEG has no terminating EOI marker");
      return info;
    }
    info.jpegLength = end;
    std::size_t sos = npos;
    const auto segs = headerSegments(f.data(), f.size(), sos);
    std::string xmp;
    int xmpCount = 0;
    for (const auto& s : segs)
      if (isXmpSegment(f.data(), s)) {
        ++xmpCount;
        if (xmp.empty()) xmp.assign(reinterpret_cast<const char*>(f.data() + s.start + 4 + sizeof(kXmpHeader)),
                                    s.end - s.start - 4 - sizeof(kXmpHeader));
      }
    if (xmpCount == 0) {
      bad("no XMP packet (APP1 http://ns.adobe.com/xap/1.0/)");
      return info;
    }
    if (xmpCount > 1) info.notes.push_back("more than one XMP APP1 segment; readers use the first");
    if (xmp.find("http://ns.google.com/photos/1.0/camera/") == std::string::npos) bad("XMP lacks the Camera namespace URI");
    if (xmp.find("http://ns.google.com/photos/1.0/container/") == std::string::npos) bad("XMP lacks the Container namespace URI");
    // prefix is conventionally Camera or GCamera; both are accepted
    std::string mp = attr(xmp, "GCamera:MotionPhoto");
    if (mp.empty()) mp = attr(xmp, "Camera:MotionPhoto");
    if (mp != "1") bad("Camera:MotionPhoto is not 1");
    std::string ver = attr(xmp, "GCamera:MotionPhotoVersion");
    if (ver.empty()) ver = attr(xmp, "Camera:MotionPhotoVersion");
    info.version = ver.empty() ? 0 : std::atoi(ver.c_str());
    if (info.version != 1) bad("Camera:MotionPhotoVersion is not 1");
    std::string ts = attr(xmp, "GCamera:MotionPhotoPresentationTimestampUs");
    if (ts.empty()) ts = attr(xmp, "Camera:MotionPhotoPresentationTimestampUs");
    info.presentationTimestampUs = ts.empty() ? -1 : std::atoll(ts.c_str());

    // container directory items, in order
    struct Item { std::string mime, semantic; std::uint64_t length = 0, padding = 0; bool hasLength = false; };
    std::vector<Item> items;
    std::size_t at = 0;
    while ((at = xmp.find("<Container:Item", at)) != std::string::npos) {
      const std::size_t stop = xmp.find("/>", at);
      if (stop == std::string::npos) break;
      Item it;
      it.mime = attr(xmp, "Item:Mime", at, stop);
      it.semantic = attr(xmp, "Item:Semantic", at, stop);
      const std::string len = attr(xmp, "Item:Length", at, stop), pad = attr(xmp, "Item:Padding", at, stop);
      it.hasLength = !len.empty();
      it.length = len.empty() ? 0 : std::strtoull(len.c_str(), nullptr, 10);
      it.padding = pad.empty() ? 0 : std::strtoull(pad.c_str(), nullptr, 10);
      items.push_back(it);
      at = stop;
    }
    if (items.size() < 2) {
      bad("Container:Directory needs at least a Primary and a MotionPhoto item");
      return info;
    }
    if (items.front().semantic != "Primary") bad("first directory item must have Semantic=Primary");
    if (items.front().mime != "image/jpeg") bad("primary item Mime is not image/jpeg");
    int nPrimary = 0, nVideo = 0;
    for (const auto& it : items) {
      nPrimary += it.semantic == "Primary";
      nVideo += it.semantic == "MotionPhoto";
      if (it.mime.empty()) bad("directory item without Mime");
    }
    if (nPrimary != 1) bad("exactly one Primary item is required");
    if (nVideo != 1) bad("exactly one MotionPhoto item is required");
    const Item& last = items.back();
    if (last.semantic != "MotionPhoto") bad("the MotionPhoto item must be last");
    if (last.mime != "video/mp4" && last.mime != "video/quicktime") bad("video item Mime must be video/mp4 or video/quicktime");
    info.videoMime = last.mime;
    if (!last.hasLength || last.length == 0) bad("video item lacks a positive Length");
    std::uint64_t secondaries = 0;
    for (std::size_t i = 1; i < items.size(); ++i) secondaries += items[i].length;
    if (secondaries + info.jpegLength + items.front().padding != f.size())
      bad("item lengths do not add up: jpeg(" + std::to_string(info.jpegLength) + ") + padding + items(" +
          std::to_string(secondaries) + ") != file size " + std::to_string(f.size()) + " (trailing or missing bytes)");
    if (last.length == 0 || last.length > f.size()) return info.valid = false, info;
    info.videoLength = last.length;
    info.videoOffset = f.size() - last.length;
    if (info.videoOffset < info.jpegLength + items.front().padding) bad("video overlaps the still image");

    if (info.videoOffset + 8 <= f.size() && info.videoOffset >= info.jpegLength) {
      if (rd32(f.data() + info.videoOffset + 4) != cc("ftyp")) {
        bad("bytes at the video offset are not an MP4 (no ftyp)");
      } else {
        Bytes vid(f.begin() + std::ptrdiff_t(info.videoOffset), f.end());
        inspectMp4(vid, info);
      }
    }
    if (info.presentationTimestampUs >= 0 && info.videoDurationSec > 0 &&
        info.presentationTimestampUs > std::int64_t(info.videoDurationSec * 1e6) + 1000)
      bad("MotionPhotoPresentationTimestampUs lies beyond the video duration");
    info.valid = info.problems.empty();
  } catch (const std::exception& e) {
    bad(std::string("validator exception: ") + e.what());
  }
  return info;
}

Result<MotionPhotoInfo> validateMotionPhotoFile(const std::filesystem::path& file) {
  auto bytes = readFile(file);
  if (!bytes) return std::unexpected(bytes.error());
  return validateMotionPhoto(*bytes);
}

namespace {
Result<void> writeAll(const std::filesystem::path& p, const Bytes& b) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return fail("Cannot write " + pathToUtf8(p));
  out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  out.flush();
  if (!out) return fail("Write failed: " + pathToUtf8(p));
  return {};
}
}  // namespace

Result<MotionPhotoInfo> writeMotionPhoto(const std::filesystem::path& jpeg, const std::filesystem::path& mp4,
                                         const std::filesystem::path& output, const MotionPhotoOptions& options) {
  try {
    auto j = readFile(jpeg);
    if (!j) return std::unexpected(j.error());
    auto m = readFile(mp4);
    if (!m) return std::unexpected(m.error());
    auto built = buildMotionPhoto(*j, *m, options);
    if (!built) return std::unexpected(built.error());
    MotionPhotoInfo info = validateMotionPhoto(*built);
    if (!info.valid) return fail("Generated file failed validation: " + (info.problems.empty() ? std::string("?") : info.problems[0]));
    if (auto w = writeAll(output, *built); !w) return std::unexpected(w.error());
    return info;
  } catch (const std::exception& e) {
    return fail(std::string("Motion Photo failed: ") + e.what());
  }
}

Result<MotionFromVideoResult> createMotionPhotoFromVideo(const MotionFromVideoRequest& rq) {
  try {
    TrimOptions to;
    to.startSec = rq.startSec;
    to.durationSec = rq.durationSec;
    to.keepAudio = rq.keepAudio;
    auto trim = trimMp4(rq.video, to);
    if (!trim) return std::unexpected(trim.error());
    auto jpeg = readFile(rq.coverJpeg);
    if (!jpeg) return std::unexpected(jpeg.error());
    MotionFromVideoResult res;
    const double cover = rq.coverSec >= 0 ? rq.coverSec : trim->actualStartSec + trim->actualDurationSec * 0.5;
    res.coverInClipSec = std::clamp(cover - trim->actualStartSec, 0.0, trim->actualDurationSec);
    MotionPhotoOptions mo;
    mo.presentationTimestampUs = std::int64_t(std::llround(res.coverInClipSec * 1e6));
    mo.legacyMicroVideo = rq.legacyMicroVideo;
    auto built = buildMotionPhoto(*jpeg, trim->mp4, mo);
    if (!built) return std::unexpected(built.error());
    res.info = validateMotionPhoto(*built);
    if (!res.info.valid) return fail("Generated file failed validation: " + (res.info.problems.empty() ? std::string("?") : res.info.problems[0]));
    if (auto w = writeAll(rq.output, *built); !w) return std::unexpected(w.error());
    res.trim = std::move(*trim);
    res.trim.mp4.clear();
    res.trim.mp4.shrink_to_fit();
    return res;
  } catch (const std::exception& e) {
    return fail(std::string("Motion Photo failed: ") + e.what());
  }
}

}  // namespace dapple
