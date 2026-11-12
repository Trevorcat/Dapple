// `dapple motion render <video> -o out.mkv [--film stock | --edit params.json] ...`: the desktop software path of an edited Motion Photo clip,
// end to end: decode (core/video, FFmpeg, 10-bit safe) -> VideoFrameSession (the still's edit through the node graph, film included) -> encode
// (FFV1 in Matroska, 8 or 10 bit: a lossless TEST intermediate; the production encoder is the platform one the Kotlin codec ladder picks).
// docs/engine/pipeline-architecture.md section 30.
#include "motion_render.h"

#ifdef DAPPLE_HAVE_VIDEO_HOST

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"
#include "dapple_video.h"
#include "engine/video/video_frame_session.h"
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"
#endif

namespace dapple::motiontool {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using dapple::engine::video::ColourKind;
using dapple::engine::video::FrameInfo;
using dapple::engine::video::InputFrame;
using dapple::engine::video::PackFormat;
using dapple::engine::video::VideoFrameSession;

struct Args {
  std::vector<std::string> pos;
  std::vector<std::pair<std::string, std::string>> opt;
  bool has(const char* k) const {
    for (auto& o : opt)
      if (o.first == k) return true;
    return false;
  }
  std::string get(const char* k, const std::string& def = {}) const {
    for (auto& o : opt)
      if (o.first == k) return o.second;
    return def;
  }
  double num(const char* k, double def) const { return has(k) ? std::atof(get(k).c_str()) : def; }
};

Args parseArgs(const std::vector<std::string>& a) {
  static const char* kFlags[] = {"--gpu", "--no-gpu", "--quiet", "--exact-size"};
  Args r;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].rfind("-", 0) == 0 && a[i].size() > 1 && !std::isdigit(static_cast<unsigned char>(a[i][1]))) {
      bool flag = false;
      for (auto f : kFlags) flag = flag || a[i] == f;
      if (flag || i + 1 >= a.size()) r.opt.push_back({a[i], "1"});
      else r.opt.push_back({a[i], a[++i]});
    } else {
      r.pos.push_back(a[i]);
    }
  }
  return r;
}

std::string readFile(const std::string& p) {
  std::ifstream f(fs::u8path(p), std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

void usage() {
  std::fputs(
      "usage: dapple motion render <video> -o out.mkv [options]\n"
      "  --film <stock>        film simulation (library id, e.g. kodak_portra_400); --edit params.json for a whole edit (EditParams JSON)\n"
      "  --lut file.cube       look LUT\n"
      "  --start S --duration D  seconds of the clip to process (default: all)\n"
      "  --ref-time T          reference frame (film match, tone analysis) at T seconds into the clip (default: the first frame)\n"
      "  --bits 10|8           output depth (default 10: FFV1 yuv420p10le; 8: yuv420p)\n"
      "  --max-edge N          decode / render at most N px on the long edge\n"
      "  --still-long-edge N   long edge of the still: the film grain is scale-aware against it\n"
      "  --gpu                 8-bit output through the GPU executor when the build has one (the 10-bit output is always the CPU executor)\n"
      "  --hdr off             never write HDR (an HLG / PQ source without film is HDR by default, docs section 30.8)\n"
      "  --max-frames N        stop after N frames\n"
      "  --stats file.csv      per-frame timings\n"
      "  --dump-ppm f.ppm [--dump-index N]  also write frame N (default 0) as a PPM\n",
      stderr);
}

struct Encoder {
  AVFormatContext* oc = nullptr;
  AVCodecContext* cc = nullptr;
  AVStream* st = nullptr;
  AVFrame* fr = nullptr;
  AVPacket* pkt = nullptr;
  bool ten = true;
  int w = 0, h = 0;
  ~Encoder() {
    if (fr) av_frame_free(&fr);
    if (pkt) av_packet_free(&pkt);
    if (cc) avcodec_free_context(&cc);
    if (oc) {
      if (oc->pb) avio_closep(&oc->pb);
      avformat_free_context(oc);
    }
  }
  bool open(const std::string& path, int width, int height, bool tenBit, const int tags[4], std::string& err) {
    w = width;
    h = height;
    ten = tenBit;
    if (avformat_alloc_output_context2(&oc, nullptr, "matroska", path.c_str()) < 0 || !oc) return err = "no matroska muxer", false;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (!codec) return err = "this FFmpeg build has no FFV1 encoder (rebuild with tools/ffmpeg/build-host.sh)", false;
    st = avformat_new_stream(oc, nullptr);
    cc = avcodec_alloc_context3(codec);
    cc->width = w;
    cc->height = h;
    cc->pix_fmt = ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    cc->time_base = AVRational{1, 1000};
    cc->color_primaries = AVColorPrimaries(tags[0]);
    cc->color_trc = AVColorTransferCharacteristic(tags[1]);
    cc->colorspace = AVColorSpace(tags[2]);
    cc->color_range = AVCOL_RANGE_MPEG;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(cc, codec, nullptr);
    if (r < 0) return err = "encoder open failed", false;
    avcodec_parameters_from_context(st->codecpar, cc);
    st->time_base = cc->time_base;
    if ((r = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0) return err = "cannot write " + path, false;
    if ((r = avformat_write_header(oc, nullptr)) < 0) return err = "write header failed", false;
    fr = av_frame_alloc();
    fr->format = cc->pix_fmt;
    fr->width = w;
    fr->height = h;
    av_frame_get_buffer(fr, 0);
    pkt = av_packet_alloc();
    return true;
  }
  bool drain() {
    for (;;) {
      int r = avcodec_receive_packet(cc, pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
      pkt->stream_index = st->index;
      av_interleaved_write_frame(oc, pkt);
    }
  }
  // `p` = the session's pack in the format matching `ten` (P010 words or I420)
  bool put(const std::uint8_t* p, std::int64_t ptsMs) {
    av_frame_make_writable(fr);
    const size_t Y = size_t(w) * h;
    if (ten) {
      const auto* y = reinterpret_cast<const std::uint16_t*>(p);
      const auto* uv = y + Y;
      for (int j = 0; j < h; ++j) {
        auto* d = reinterpret_cast<std::uint16_t*>(fr->data[0] + size_t(j) * fr->linesize[0]);
        for (int i = 0; i < w; ++i) d[i] = std::uint16_t(y[size_t(j) * w + i] >> 6);
      }
      for (int j = 0; j < h / 2; ++j) {
        auto* du = reinterpret_cast<std::uint16_t*>(fr->data[1] + size_t(j) * fr->linesize[1]);
        auto* dv = reinterpret_cast<std::uint16_t*>(fr->data[2] + size_t(j) * fr->linesize[2]);
        for (int i = 0; i < w / 2; ++i) {
          du[i] = std::uint16_t(uv[(size_t(j) * (w / 2) + i) * 2] >> 6);
          dv[i] = std::uint16_t(uv[(size_t(j) * (w / 2) + i) * 2 + 1] >> 6);
        }
      }
    } else {
      for (int j = 0; j < h; ++j) std::memcpy(fr->data[0] + size_t(j) * fr->linesize[0], p + size_t(j) * w, size_t(w));
      const std::uint8_t* u = p + Y;
      const std::uint8_t* v = u + Y / 4;
      for (int j = 0; j < h / 2; ++j) {
        std::memcpy(fr->data[1] + size_t(j) * fr->linesize[1], u + size_t(j) * (w / 2), size_t(w / 2));
        std::memcpy(fr->data[2] + size_t(j) * fr->linesize[2], v + size_t(j) * (w / 2), size_t(w / 2));
      }
    }
    fr->pts = ptsMs;
    if (avcodec_send_frame(cc, fr) < 0) return false;
    return drain();
  }
  bool finish() {
    avcodec_send_frame(cc, nullptr);
    drain();
    return av_write_trailer(oc) >= 0;
  }
};

}  // namespace

int run(const std::vector<std::string>& argv) {
  const Args a = parseArgs(argv);
  if (a.pos.size() != 1 || !a.has("-o")) {
    usage();
    return 2;
  }
  EditParams params;
  if (a.has("--edit")) {
    auto parsed = fromJson(readFile(a.get("--edit")));
    if (!parsed) {
      std::fprintf(stderr, "edit: %s\n", parsed.error().message.c_str());
      return 1;
    }
    params = *parsed;
  }
  if (a.has("--film")) params.filmStock = a.get("--film");
  std::shared_ptr<const Lut3D> lut;
  if (a.has("--lut")) {
    auto l = Lut3D::loadCube(fs::u8path(a.get("--lut")));
    if (!l) {
      std::fprintf(stderr, "lut: %s\n", l.error().message.c_str());
      return 1;
    }
    lut = std::make_shared<const Lut3D>(std::move(*l));
    params.lutPath = a.get("--lut");
  }
  const bool ten = a.get("--bits", "10") != "8";
  char err[256] = {0};
  DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
  DvInfo info{};
  DvVideo* v = dv_open(a.pos[0].c_str(), &oo, &info, err, sizeof err);
  if (!v) {
    std::fprintf(stderr, "open: %s\n", err);
    return 1;
  }
  const double fps = info.fps > 0 ? info.fps : 30.0;
  const double t0 = a.num("--start", 0), dur = a.num("--duration", 0);
  const int64_t startUs = int64_t(t0 * 1e6), endUs = dur > 0 ? int64_t((t0 + dur) * 1e6) : info.durationUs;
  const int maxEdge = int(a.num("--max-edge", 0));
  engine::video::Config cfg;
  cfg.params = params;
  cfg.camera = CameraRegistry::withBuiltins().resolve("", "");
  cfg.lut = lut;
  cfg.stillLongEdge = int(a.num("--still-long-edge", 0));
  cfg.allowGpu = a.has("--gpu") && !ten;
  cfg.allowHdr = a.get("--hdr", "auto") != "off";
#ifdef DAPPLE_HAVE_GPU
  if (cfg.allowGpu) {
    engine::gpu::setRuntimeEnabled(1);
    engine::gpu::installProvider();
  }
#endif
  VideoFrameSession session(cfg);

  auto kindOf = [&] { return static_cast<ColourKind>(info.colorKind); };
  int w = 0, h = 0;
  // reference frame
  {
    const int64_t refUs = int64_t((t0 + a.num("--ref-time", 0)) * 1e6);
    if (dv_begin(v, refUs, refUs, maxEdge, 0, DV_RGBAF16, 0, &w, &h) != 0) {
      std::fprintf(stderr, "decoder seek failed\n");
      dv_close(v);
      return 1;
    }
    std::vector<std::uint16_t> px(size_t(w) * h * 4);
    if (dv_next(v, reinterpret_cast<uint8_t*>(px.data()), int64_t(px.size()) * 2) < 0) {
      std::fprintf(stderr, "no reference frame\n");
      dv_close(v);
      return 1;
    }
    InputFrame f{px.data(), w, h, kindOf(), info.rotation};
    std::string e;
    if (!session.setReference(f, &e)) {
      std::fprintf(stderr, "reference: %s\n", e.c_str());
      dv_close(v);
      return 1;
    }
  }
  if (dv_begin(v, startUs, endUs, maxEdge, 0, DV_RGBAF16, 0, &w, &h) != 0) {
    std::fprintf(stderr, "decoder seek failed\n");
    dv_close(v);
    return 1;
  }
  Encoder enc;
  std::string e;
  int tags[4];
  session.outputTags(tags);
  const PackFormat packFmt = ten ? PackFormat::P010Hdr : PackFormat::I420;  // P010Hdr = P010 in the session's output transfer
  if (!enc.open(a.get("-o"), session.outputWidth(), session.outputHeight(), ten, tags, e)) {
    std::fprintf(stderr, "%s\n", e.c_str());
    dv_close(v);
    return 1;
  }
  std::ofstream stats;
  if (a.has("--stats")) {
    stats.open(fs::u8path(a.get("--stats")));
    stats << "frame,ptsMs,decodeMs,convertMs,renderMs,packMs,encodeMs,executor,tablesHit\n";
  }
  std::vector<std::uint16_t> px(size_t(w) * h * 4);
  std::vector<std::uint8_t> out;
  const int maxFrames = int(a.num("--max-frames", 0));
  int n = 0;
  double sDec = 0, sConv = 0, sRen = 0, sPack = 0, sEnc = 0;
  std::string exec;
  const auto tAll = Clock::now();
  for (;;) {
    const auto td = Clock::now();
    const int64_t us = dv_next(v, reinterpret_cast<uint8_t*>(px.data()), int64_t(px.size()) * 2);
    if (us == -1) break;
    if (us < 0) {
      std::fprintf(stderr, "decode failed at frame %d\n", n);
      dv_close(v);
      return 1;
    }
    const double decMs = std::chrono::duration<double, std::milli>(Clock::now() - td).count();
    InputFrame f{px.data(), w, h, kindOf(), info.rotation};
    FrameInfo fi;
    if (!session.render(f, std::uint32_t(n + 1), packFmt, out, &fi, &e)) {
      std::fprintf(stderr, "frame %d: %s\n", n, e.c_str());
      dv_close(v);
      return 1;
    }
    if (a.has("--dump-ppm") && n == int(a.num("--dump-index", 0))) {  // one frame as a PPM (visual checks; convert with PIL)
      std::vector<std::uint8_t> rgba8;
      if (session.render(f, std::uint32_t(n + 1), PackFormat::Rgba8, rgba8, nullptr, &e)) {
        std::ofstream pp(fs::u8path(a.get("--dump-ppm")), std::ios::binary);
        pp << "P6\n" << session.outputWidth() << ' ' << session.outputHeight() << "\n255\n";
        for (size_t i = 0; i + 3 < rgba8.size(); i += 4) pp.write(reinterpret_cast<const char*>(&rgba8[i]), 3);
      }
    }
    const auto te = Clock::now();
    if (!enc.put(out.data(), us / 1000)) {
      std::fprintf(stderr, "encode failed at frame %d\n", n);
      dv_close(v);
      return 1;
    }
    const double encMs = std::chrono::duration<double, std::milli>(Clock::now() - te).count();
    sDec += decMs, sConv += fi.convertMs, sRen += fi.renderMs, sPack += fi.packMs, sEnc += encMs;
    exec = fi.executor;
    if (stats.is_open())
      stats << n << ',' << us / 1000 << ',' << decMs << ',' << fi.convertMs << ',' << fi.renderMs << ',' << fi.packMs << ',' << encMs << ',' << fi.executor << ','
            << (fi.tablesHit ? 1 : 0) << '\n';
    ++n;
    if (maxFrames > 0 && n >= maxFrames) break;
  }
  const double total = std::chrono::duration<double, std::milli>(Clock::now() - tAll).count();
  const bool ok = enc.finish();
  dv_close(v);
  if (!ok || n == 0) {
    std::fprintf(stderr, "no frames written\n");
    return 1;
  }
  if (!a.has("--quiet"))
    std::printf("motion render: %d frames %dx%d (%s, %d-bit out, %s, executor %s, source %dx%d %d-bit kind %d) in %.2f s = %.1f ms/frame "
                "(decode %.1f, convert %.1f, render %.1f, pack %.1f, encode %.1f ms/frame); fps of the clip %.2f\n",
                n, enc.w, enc.h, params.filmStock.empty() ? "no film" : params.filmStock.c_str(), ten ? 10 : 8, session.outputNote(), exec.c_str(), info.width, info.height, info.bitDepth,
                info.colorKind, total / 1000.0, total / n, sDec / n, sConv / n, sRen / n, sPack / n, sEnc / n, fps);
  return 0;
}

}  // namespace dapple::motiontool

#else  // no desktop FFmpeg

namespace dapple::motiontool {
int run(const std::vector<std::string>&) {
  std::fputs("dapple motion render needs the desktop FFmpeg: run tools/ffmpeg/build-host.sh and reconfigure\n", stderr);
  return 3;
}
}  // namespace dapple::motiontool

#endif
