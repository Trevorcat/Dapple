// dapple: desktop command line front end for the Dapple core.
//
//   dapple info <files...>
//   dapple develop <inputs...> [-o out.jpg | --out-dir DIR] [options]
//   dapple watch <dir> --out-dir DIR [options]
//   dapple thumb <raw> -o out.jpg [--long-edge N]     (camera's embedded JPEG)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "dapple/developer.h"
#include "dapple/encode.h"
#include "dapple/ingest.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#include "dapple/xmp_preset.h"
#include "eval.h"
#include "film_cmds.h"
#include "merge_cmds.h"

namespace dapple::benchtool { int editBench(const std::vector<std::string>& args); }
#ifdef DAPPLE_HAVE_GPU
namespace dapple::gpu { int gpuBench(const std::vector<std::string>& args); }
#endif

namespace fs = std::filesystem;
using namespace dapple;

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

void usage() {
  std::puts(
      "dapple - RAW/JPEG develop tool\n"
      "\n"
      "  dapple info <files...>\n"
      "  dapple develop <inputs...> [-o out.jpg | --out-dir DIR] [options]\n"
      "  dapple watch <dir> --out-dir DIR [options] [--existing] [--settle MS] [--max N] [--timeout SEC]\n"
      "  dapple thumb <raw> -o out.jpg [--long-edge N]\n"
      "  dapple hdr <frames...> -o out.jpg [--deghost off|low|mid|high] [--dng f.dng]   HDR bracket merge\n"
      "  dapple pano <frames...> -o out.jpg [--projection sph|cyl|persp]                panorama stitch\n"
      "  dapple motion <video> --cover still.jpg -o x_MP.jpg | --validate f | --info v  Motion Photo\n"
      "  dapple detect <files|dirs...> [-r]                                              find bracket / pano sets\n"
      "  dapple eval --dir <ARW folder> [--step 10] [--out build/eval]   compare with the camera JPEG\n"
      "  dapple fit-profile --dir <ARW folder> --out profile.inc          fit a camera matching profile\n"
      "  dapple film --in x.pfm|x.ARW --out y.png --film NAME --paper NAME|none [...]   spectral film simulation\n"
      "  dapple film-pack <profiles_dir> <out.dfp>                        build the binary film pack\n"
      "  dapple film-upsm-gen [--n 192] [--out t.bin] [--verify pack.dfp]  regenerate / verify the spectral upsampling table\n"
      "  dapple film-render-bench                                         develop pipeline with film on: timings\n"
      "  dapple film-bench | film-compare a.pfm b.pfm | film-info         film benchmark / dE2000 / stock list\n"
      "\n"
      "develop options:\n"
      "  --xmp preset.xmp     Lightroom/Camera Raw preset\n"
      "  --lut look.cube      3D LUT (applied in sRGB at the end)\n"
      "  --lut-amount N       LUT strength 0..100 (default 100)\n"
      "  --auto | --no-auto   per-photo automatic correction (default: --auto)\n"
      "  --strength S         automatic correction strength 0..1 (default 1)\n"
      "  --full               full-resolution demosaic (default: fast half-size)\n"
      "  --long-edge N        output long edge in pixels, 0 = native (default 2560)\n"
      "  --quality N          JPEG quality 1..100 (default 92)\n"
      "  --dump-params        print the applied EditParams JSON\n");
}

struct Options {
  std::vector<std::string> positional;
  std::string out;
  std::string outDir;
  std::string xmp;
  std::string lut;
  float lutAmount = 100.f;
  bool autoAdjust = true;
  float strength = 1.f;
  bool full = false;
  int longEdge = 2560;
  int quality = 92;
  bool dumpParams = false;
  std::string params;
  int repeat = 10;
  // watch
  bool existing = false;
  int settleMs = 1500;
  int maxPhotos = 0;
  int timeoutSec = 0;
};

bool parseArgs(const std::vector<std::string>& args, Options& o, std::string& err) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) {
        err = "missing value for " + a;
        return false;
      }
      dst = args[++i];
      return true;
    };
    auto number = [&](double& dst) {
      std::string v;
      if (!value(v)) return false;
      char* end = nullptr;
      dst = std::strtod(v.c_str(), &end);
      if (end == v.c_str() || *end) {
        err = "invalid number for " + a + ": " + v;
        return false;
      }
      return true;
    };
    double d = 0;
    if (a == "-o" || a == "--out") { if (!value(o.out)) return false; }
    else if (a == "--out-dir") { if (!value(o.outDir)) return false; }
    else if (a == "--xmp") { if (!value(o.xmp)) return false; }
    else if (a == "--lut") { if (!value(o.lut)) return false; }
    else if (a == "--lut-amount") { if (!number(d)) return false; o.lutAmount = float(d); }
    else if (a == "--auto") o.autoAdjust = true;
    else if (a == "--no-auto") o.autoAdjust = false;
    else if (a == "--strength") { if (!number(d)) return false; o.strength = float(d); }
    else if (a == "--full") o.full = true;
    else if (a == "--long-edge") { if (!number(d)) return false; o.longEdge = int(d); }
    else if (a == "--quality") { if (!number(d)) return false; o.quality = int(d); }
    else if (a == "--dump-params") o.dumpParams = true;
    else if (a == "--params") { if (!value(o.params)) return false; }
    else if (a == "--repeat") { if (!number(d)) return false; o.repeat = int(d); }
    else if (a == "--existing") o.existing = true;
    else if (a == "--settle") { if (!number(d)) return false; o.settleMs = int(d); }
    else if (a == "--max") { if (!number(d)) return false; o.maxPhotos = int(d); }
    else if (a == "--timeout") { if (!number(d)) return false; o.timeoutSec = int(d); }
    else if (a.size() > 1 && a[0] == '-') {
      err = "unknown option " + a;
      return false;
    } else o.positional.push_back(a);
  }
  return true;
}

const char* supportText(Support s) {
  switch (s) {
    case Support::Yes: return "yes";
    case Support::No: return "no";
    default: return "unverified";
  }
}

int cmdInfo(const std::vector<std::string>& files) {
  if (files.empty()) {
    std::fputs("info: no files given\n", stderr);
    return 2;
  }
  const Developer dev;
  int rc = 0;
  for (const auto& f : files) {
    const fs::path p = pathFromUtf8(f);
    const ImageDecoder* decoder = dev.decoders().find(p);
    std::printf("%s\n", f.c_str());
    if (!decoder) {
      std::puts("  unsupported file type");
      rc = 1;
      continue;
    }
    auto meta = decoder->probe(p);
    if (!meta) {
      std::printf("  error: %s\n", meta.error().message.c_str());
      rc = 1;
      continue;
    }
    const ShotMetadata& m = *meta;
    const CameraProfile& cam = dev.cameras().resolve(m.make, m.model);
    std::printf("  decoder     %s\n", decoder->name());
    std::printf("  camera      %s %s\n", m.make.c_str(), m.model.c_str());
    std::printf("  lens        %s\n", m.lens.c_str());
    std::printf("  exposure    ISO %.0f  1/%.0f s  f/%.1f  %.0f mm\n", m.iso,
                m.shutterSeconds > 0 ? 1.0 / m.shutterSeconds : 0.0, m.aperture, m.focalLengthMm);
    std::printf("  size        %d x %d  (%s, %d bit)\n", m.sensorWidth, m.sensorHeight, toString(m.cfa == CfaKind::Unknown ? cam.cfa : m.cfa), m.bitsPerSample);
    if (!m.rawFormat.empty()) std::printf("  raw format  %s\n", m.rawFormat.c_str());
    if (!m.inCameraLook.empty()) std::printf("  in-cam look %s\n", m.inCameraLook.c_str());
    std::printf("  as-shot WB  R %.3f  G 1  B %.3f\n", m.asShotWb[0], m.asShotWb[2]);
    std::printf("  profile     %s (%s)  baseline %+.2f EV, contrast %+.0f\n", cam.id.c_str(), cam.displayName.c_str(),
                cam.baselineExposureEv, cam.baselineContrast);
    std::printf("  transfer    FTP push: %s | USB PTP: %s | vendor app auto-send: %s%s%s\n", supportText(cam.transfer.ftpPush),
                supportText(cam.transfer.usbPtp), supportText(cam.transfer.vendorAppAutoSend),
                cam.transfer.vendorApp.empty() ? "" : " via ", cam.transfer.vendorApp.c_str());
    if (!cam.notes.empty()) std::printf("  notes       %s\n", cam.notes.c_str());
  }
  return rc;
}

struct Look {
  EditParams params;
  std::optional<Lut3D> lut;
};

bool loadLook(const Options& o, Look& look) {
  if (!o.xmp.empty()) {
    auto preset = loadXmpPreset(pathFromUtf8(o.xmp));
    if (!preset) {
      std::fprintf(stderr, "xmp: %s\n", preset.error().message.c_str());
      return false;
    }
    look.params = preset->params;
    std::printf("preset: %s\n", preset->name.c_str());
    for (const auto& u : preset->unsupported) std::printf("  not reproduced: %s\n", u.c_str());
  }
  if (!o.params.empty()) {
    std::string json = o.params;
    if (json[0] == '@') {
      std::ifstream f(pathFromUtf8(json.substr(1)), std::ios::binary);
      if (!f) {
        std::fprintf(stderr, "params: cannot read %s\n", json.c_str() + 1);
        return false;
      }
      json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    auto parsed = fromJson(json);
    if (!parsed) {
      std::fprintf(stderr, "params: %s\n", parsed.error().message.c_str());
      return false;
    }
    look.params += *parsed;
  }
  if (!o.lut.empty()) {
    auto lut = Lut3D::loadCube(pathFromUtf8(o.lut));
    if (!lut) {
      std::fprintf(stderr, "lut: %s\n", lut.error().message.c_str());
      return false;
    }
    look.lut = std::move(*lut);
    look.params.lutPath = o.lut;
    look.params.lutAmount = o.lutAmount;
    std::printf("lut: %s (size %d, amount %.0f)\n", look.lut->title().c_str(), look.lut->size(), o.lutAmount);
  }
  return true;
}

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool developOne(const Developer& dev, const Look& look, const Options& o, const fs::path& in, const fs::path& out) {
  const auto t0 = std::chrono::steady_clock::now();
  DevelopRequest req;
  req.input = in;
  req.look = look.params;
  req.lut = look.lut ? &*look.lut : nullptr;
  if (o.autoAdjust) {
    AutoOptions ao;
    ao.strength = o.strength;
    req.autoAdjust = ao;
  }
  req.quality = o.full ? DecodeQuality::Full : DecodeQuality::Preview;
  req.longEdge = o.longEdge;

  auto result = dev.develop(req);
  if (!result) {
    std::fprintf(stderr, "%s: %s\n", utf8(in).c_str(), result.error().message.c_str());
    return false;
  }
  const auto te = std::chrono::steady_clock::now();
  std::error_code ec;
  if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
  auto written = writeJpeg(result->image, out, JpegOptions{o.quality});
  if (!written) {
    std::fprintf(stderr, "%s: %s\n", utf8(out).c_str(), written.error().message.c_str());
    return false;
  }
  const double encodeMs = msSince(te);

  std::printf("%s -> %s\n", utf8(in).c_str(), utf8(out).c_str());
  std::printf("  %s %s  %dx%d  profile %s\n", result->meta.make.c_str(), result->meta.model.c_str(),
              result->image.width, result->image.height, result->camera ? result->camera->id.c_str() : "?");
  if (result->autoResult) {
    const ImageStats& s = result->autoResult->stats;
    std::printf("  auto: median %.3f  p01 %.4f  p99 %.3f  clipped %.2f%%  cast a/b %+.4f/%+.4f\n", s.medianLuma, s.p01Luma,
                s.p99Luma, s.clippedFraction * 100, s.neutralCastA, s.neutralCastB);
    if (result->autoResult->notes.empty()) std::puts("  auto: no correction needed");
    for (const auto& n : result->autoResult->notes) std::printf("  auto: %s\n", n.c_str());
  }
  if (o.dumpParams) std::printf("  applied params:\n%s", toJson(result->applied).c_str());
  const auto& t = result->timings;
  std::printf("  timings: decode %.0f ms, analyze %.0f ms, render %.0f ms, encode %.0f ms, total %.0f ms\n", t.decodeMs,
              t.analyzeMs, t.renderMs, encodeMs, msSince(t0));
  std::fflush(stdout);
  return true;
}

fs::path outputPath(const Options& o, const fs::path& in, bool single) {
  if (single && !o.out.empty()) return pathFromUtf8(o.out);
  fs::path dir = !o.outDir.empty() ? pathFromUtf8(o.outDir) : fs::current_path();
  // Include the source extension so RAW+JPEG pairs (DSC0001.ARW / DSC0001.jpg) do not collide.
  std::u8string ext = in.extension().u8string();
  if (!ext.empty() && ext[0] == u8'.') ext.erase(0, 1);
  for (auto& c : ext) c = static_cast<char8_t>(std::toupper(static_cast<unsigned char>(c)));
  return dir / (in.stem().u8string() + (ext.empty() ? u8"" : u8"_") + ext + u8"_dapple.jpg");
}

int cmdDevelop(const Options& o) {
  if (o.positional.empty()) {
    std::fputs("develop: no inputs given\n", stderr);
    return 2;
  }
  const Developer dev;
  std::vector<fs::path> inputs;
  for (const auto& s : o.positional) {
    const fs::path p = pathFromUtf8(s);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      for (const auto& e : fs::directory_iterator(p, ec))
        if (e.is_regular_file() && dev.decoders().find(e.path())) inputs.push_back(e.path());
    } else {
      inputs.push_back(p);
    }
  }
  if (inputs.empty()) {
    std::fputs("develop: nothing to develop\n", stderr);
    return 2;
  }
  if (inputs.size() > 1 && !o.out.empty()) {
    std::fputs("develop: -o only works with a single input; use --out-dir\n", stderr);
    return 2;
  }
  Look look;
  if (!loadLook(o, look)) return 1;

  int failures = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& in : inputs)
    if (!developOne(dev, look, o, in, outputPath(o, in, inputs.size() == 1))) ++failures;
  if (inputs.size() > 1)
    std::printf("developed %zu photo(s), %d failed, %.1f s\n", inputs.size() - failures, failures, msSince(t0) / 1000);
  return failures ? 1 : 0;
}

int cmdBench(const Options& o) {
  if (o.positional.size() != 1) {
    std::fputs("bench: usage: dapple bench <input> [--params JSON|@file] [--long-edge N] [--repeat N]\n", stderr);
    return 2;
  }
  Look look;
  if (!loadLook(o, look)) return 1;
  const Developer dev;
  const fs::path in = pathFromUtf8(o.positional[0]);
  const ImageDecoder* decoder = dev.decoders().find(in);
  if (!decoder) {
    std::fputs("bench: unsupported file type\n", stderr);
    return 1;
  }
  auto src = decoder->decode(in, DecodeOptions{o.full ? DecodeQuality::Full : DecodeQuality::Preview});
  if (!src) {
    std::fprintf(stderr, "bench: %s\n", src.error().message.c_str());
    return 1;
  }
  if (o.longEdge > 0) src->linear = resizeToLongEdge(src->linear, o.longEdge);
  const CameraProfile& cam = dev.cameras().resolve(src->meta.make, src->meta.model);
  std::vector<double> ms;
  int w = 0, h = 0;
  for (int i = 0; i < std::max(1, o.repeat) + 1; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    Image8 img;
    std::string err;
    if (!engine::renderImage8(look.params, cam, look.lut ? &*look.lut : nullptr, *src, img, &err)) {
      std::fprintf(stderr, "bench: %s\n", err.c_str());
      return 1;
    }
    w = img.width;
    h = img.height;
    if (i > 0) ms.push_back(msSince(t0));  // first run warms caches and the thread pool
  }
  std::sort(ms.begin(), ms.end());
  std::printf("%dx%d (source %dx%d): render min %.1f ms, median %.1f ms, max %.1f ms over %zu runs\n", w, h,
              src->linear.width, src->linear.height, ms.front(), ms[ms.size() / 2], ms.back(), ms.size());
  return 0;
}

int cmdWatch(const Options& o) {
  if (o.positional.size() != 1 || o.outDir.empty()) {
    std::fputs("watch: usage: dapple watch <dir> --out-dir DIR [options]\n", stderr);
    return 2;
  }
  const fs::path dir = pathFromUtf8(o.positional[0]);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    std::fprintf(stderr, "watch: not a directory: %s\n", o.positional[0].c_str());
    return 2;
  }
  Look look;
  if (!loadLook(o, look)) return 1;
  const Developer dev;

  FolderWatchSource::Options wo;
  wo.includeExisting = o.existing;
  wo.settleTime = std::chrono::milliseconds(o.settleMs);
  FolderWatchSource source(dir, [&](const fs::path& p) { return dev.decoders().find(p) != nullptr; }, wo);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("watching %s (Ctrl-C to stop)\n", o.positional[0].c_str());
  std::fflush(stdout);

  int done = 0;
  const auto start = std::chrono::steady_clock::now();
  while (!g_stop) {
    source.poll([&](const IncomingPhoto& photo) {
      std::printf("[%s] new photo %s\n", photo.sourceName.c_str(), utf8(photo.path.filename()).c_str());
      developOne(dev, look, o, photo.path, outputPath(o, photo.path, false));
      ++done;
    });
    if (o.maxPhotos > 0 && done >= o.maxPhotos) break;
    if (o.timeoutSec > 0 && msSince(start) > o.timeoutSec * 1000.0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  std::printf("watch finished, %d photo(s) developed\n", done);
  return 0;
}

int cmdThumb(const Options& o) {
  if (o.positional.size() != 1 || o.out.empty()) {
    std::fputs("thumb: usage: dapple thumb <raw> -o out.jpg [--long-edge N]\n", stderr);
    return 2;
  }
  auto img = extractEmbeddedPreview(pathFromUtf8(o.positional[0]), o.longEdge);
  if (!img) {
    std::fprintf(stderr, "thumb: %s\n", img.error().message.c_str());
    return 1;
  }
  auto w = writeJpeg(*img, pathFromUtf8(o.out), JpegOptions{o.quality});
  if (!w) {
    std::fprintf(stderr, "thumb: %s\n", w.error().message.c_str());
    return 1;
  }
  std::printf("%s -> %s  (%dx%d)\n", o.positional[0].c_str(), o.out.c_str(), img->width, img->height);
  return 0;
}

// Calibration helper: dumps the decoded linear ProPhoto image (as-shot WB,
// before any renderer stage) as int32 w, int32 h, then float32 RGB.
int cmdDumpLinear(const Options& o) {
  if (o.positional.size() != 1 || o.out.empty()) {
    std::fputs("dump-linear: usage: dapple dump-linear <file> -o out.f32 [--long-edge N] [--full]\n", stderr);
    return 2;
  }
  const Developer dev;
  const fs::path in = pathFromUtf8(o.positional[0]);
  const ImageDecoder* decoder = dev.decoders().find(in);
  if (!decoder) {
    std::fputs("dump-linear: unsupported file\n", stderr);
    return 1;
  }
  auto src = decoder->decode(in, DecodeOptions{o.full ? DecodeQuality::Full : DecodeQuality::Preview});
  if (!src) {
    std::fprintf(stderr, "dump-linear: %s\n", src.error().message.c_str());
    return 1;
  }
  const ImageF img = resizeToLongEdge(src->linear, o.longEdge);
  std::ofstream f(pathFromUtf8(o.out), std::ios::binary);
  const std::int32_t hdr[2] = {img.width, img.height};
  f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
  f.write(reinterpret_cast<const char*>(img.data.data()), std::streamsize(img.data.size() * sizeof(float)));
  std::printf("%s -> %s (%dx%d)\n", o.positional[0].c_str(), o.out.c_str(), img.width, img.height);
  return f ? 0 : 1;
}

// Reference-comparison exporter: decodes a RAW (as-shot WB, camera profile ->
// linear ProPhoto, no tone/colour/look), optionally box-downsizes, writes a
// little-endian float32 PFM (3ch, rows bottom-to-top).
//   dapple linear <in> <out.pfm> [--max-edge N]
int cmdLinear(const std::vector<std::string>& a) {
  std::vector<std::string> pos;
  int maxEdge = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--max-edge" && i + 1 < a.size()) maxEdge = std::atoi(a[++i].c_str());
    else pos.push_back(a[i]);
  }
  if (pos.size() != 2) {
    std::fputs("linear: usage: dapple linear <in.ARW> <out.pfm> [--max-edge N]\n", stderr);
    return 2;
  }
  const Developer dev;
  const fs::path in = pathFromUtf8(pos[0]);
  const ImageDecoder* decoder = dev.decoders().find(in);
  if (!decoder) {
    std::fputs("linear: unsupported file\n", stderr);
    return 1;
  }
  auto src = decoder->decode(in, DecodeOptions{DecodeQuality::Full});
  if (!src) {
    std::fprintf(stderr, "linear: %s\n", src.error().message.c_str());
    return 1;
  }
  const ImageF img = resizeToLongEdge(src->linear, maxEdge);
  std::ofstream f(pathFromUtf8(pos[1]), std::ios::binary);
  if (!f) {
    std::fputs("linear: cannot open output\n", stderr);
    return 1;
  }
  f << "PF\n" << img.width << " " << img.height << "\n-1.0\n";
  for (int y = img.height - 1; y >= 0; --y)
    f.write(reinterpret_cast<const char*>(img.row(y)), std::streamsize(std::size_t(img.width) * 3 * sizeof(float)));
  std::printf("%s -> %s (%dx%d)\n", pos[0].c_str(), pos[1].c_str(), img.width, img.height);
  return f ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  for (int i = 1; wargv && i < wargc; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? std::size_t(len - 1) : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
    args.push_back(std::move(s));
  }
  if (wargv) LocalFree(wargv);
#else
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);  // already UTF-8
#endif
  if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    usage();
    return args.empty() ? 2 : 0;
  }
  const std::string cmd = args[0];
  filmtool::installDefaultLibrary();
  if (mergetool::handles(cmd)) return mergetool::run(cmd, {args.begin() + 1, args.end()});
  if (filmtool::handles(cmd)) return filmtool::run(cmd, {args.begin() + 1, args.end()});
  if (cmd == "eval") return evaltool::cmdEval({args.begin() + 1, args.end()});
  if (cmd == "fit-profile") return evaltool::cmdFitProfile({args.begin() + 1, args.end()});
  if (cmd == "edit-bench") return dapple::benchtool::editBench({args.begin() + 1, args.end()});
#ifdef DAPPLE_HAVE_GPU
  if (cmd == "gpu-bench") return dapple::gpu::gpuBench({args.begin() + 1, args.end()});
#endif
  if (cmd == "linear") return cmdLinear({args.begin() + 1, args.end()});
  Options o;
  std::string err;
  if (!parseArgs({args.begin() + 1, args.end()}, o, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  if (cmd == "info") return cmdInfo(o.positional);
  if (cmd == "develop") return cmdDevelop(o);
  if (cmd == "watch") return cmdWatch(o);
  if (cmd == "bench") return cmdBench(o);
  if (cmd == "thumb") return cmdThumb(o);
  if (cmd == "dump-linear") return cmdDumpLinear(o);
  std::fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
  usage();
  return 2;
}
