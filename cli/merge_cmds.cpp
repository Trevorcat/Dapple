// dapple hdr | pano | motion | detect

#include "merge_cmds.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/encode.h"
#include "dapple/merge.h"
#include "dapple/motion_photo.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace dapple::mergetool {
namespace {

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string U(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  std::set<std::string> flags;
  std::string err;
};

Args parse(const std::vector<std::string>& a, const std::set<std::string>& valued) {
  Args r;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].size() > 1 && a[i][0] == '-' && !(a[i][1] >= '0' && a[i][1] <= '9')) {
      if (valued.count(a[i])) {
        if (i + 1 >= a.size()) {
          r.err = "missing value for " + a[i];
          return r;
        }
        const std::string key = a[i];  // C++17 sequences the right operand of '=' first
        r.opt[key] = a[++i];
      } else {
        r.flags.insert(a[i]);
      }
    } else {
      r.pos.push_back(a[i]);
    }
  }
  return r;
}

double num(const Args& a, const char* k, double def) {
  auto it = a.opt.find(k);
  return it == a.opt.end() ? def : std::atof(it->second.c_str());
}
std::string str(const Args& a, const char* k, const std::string& def = {}) {
  auto it = a.opt.find(k);
  return it == a.opt.end() ? def : it->second;
}

MergeControl progressPrinter(bool quiet) {
  MergeControl c;
  if (!quiet)
    c.progress = [](float f, const char* stage) {
      std::fprintf(stderr, "\r  [%3.0f%%] %-10s", f * 100, stage);
      if (f >= 1.f) std::fputc('\n', stderr);
    };
  return c;
}

Result<void> developTo(const SourceImage& src, const fs::path& out, int longEdge, int quality) {
  static const CameraRegistry cameras = CameraRegistry::withBuiltins();
  SourceImage s;
  s.sceneReferred = src.sceneReferred;
  s.meta = src.meta;
  s.linear = longEdge > 0 ? resizeToLongEdge(src.linear, longEdge) : src.linear;
  const CameraProfile cam = effectiveCamera(cameras, s.meta);
  EditParams params;
  AutoOptions ao;
  params += computeAutoAdjust(s, cam, ao).correction;
  Image8 img;
  std::string err;
  if (!engine::renderImage8(params, cam, nullptr, s, img, &err)) return fail("render failed: " + err);
  JpegOptions jo;
  jo.quality = quality;
  return writeJpeg(img, out, jo);
}

void printHdrReport(const HdrReport& r) {
  std::printf("  base frame %d%s, load %.2fs, align %.2fs, merge %.2fs, max value %.2f\n", r.baseIndex,
              r.exposureFusion ? " (exposure fusion)" : "", r.secLoad, r.secAlign, r.secMerge, r.maxValue);
  for (std::size_t i = 0; i < r.frames.size(); ++i) {
    const auto& f = r.frames[i];
    std::printf("  [%zu] %s\n      EV used %+.2f (EXIF %+.2f%s)  align method %.0f score %.3f shift %.2f px  ghost %.2f%%\n", i,
                f.path.c_str(), f.evUsed, f.evExif, f.evFromData ? ", measured" : "", f.alignMethod, f.alignScore,
                f.shiftPx, f.ghostFraction * 100);
  }
  for (const auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
}

int writeExtras(const Args& a, const SourceImage& img) {
  if (a.opt.count("--dng")) {
    if (auto w = writeLinearDng(img, P(str(a, "--dng"))); !w) {
      std::fprintf(stderr, "dng failed: %s\n", w.error().message.c_str());
      return 1;
    }
    std::printf("  wrote %s\n", str(a, "--dng").c_str());
  }
  if (a.opt.count("--tiff")) {
    if (auto w = writeLinearTiff16(img.linear, P(str(a, "--tiff"))); !w) {
      std::fprintf(stderr, "tiff failed: %s\n", w.error().message.c_str());
      return 1;
    }
    std::printf("  wrote %s\n", str(a, "--tiff").c_str());
  }
  return 0;
}

int cmdHdr(const std::vector<std::string>& argv) {
  Args a = parse(argv, {"-o", "--out", "--base", "--deghost", "--long-edge", "--dng", "--tiff", "--quality"});
  if (!a.err.empty() || a.pos.size() < 2) {
    std::fputs("usage: dapple hdr <2-9 frames> -o out.jpg [--base N] [--deghost off|low|mid|high] [--no-align]\n"
               "                  [--long-edge N] [--preview] [--dng out.dng] [--tiff out.tif] [--quality N]\n"
               "  merges bracketed exposures in linear light; -o is the developed JPEG (default look + auto),\n"
               "  --dng/--tiff keep the linear merge for archival.\n", stderr);
    if (!a.err.empty()) std::fprintf(stderr, "%s\n", a.err.c_str());
    return 2;
  }
  std::vector<fs::path> files;
  for (auto& s : a.pos) files.push_back(P(s));
  HdrOptions o;
  o.baseIndex = a.opt.count("--base") ? int(num(a, "--base", -1)) : -1;
  o.deghost = deghostFromString(str(a, "--deghost", "mid"));
  o.align = !a.flags.count("--no-align");
  o.longEdge = int(num(a, "--long-edge", 0));
  o.quality = a.flags.count("--preview") ? DecodeQuality::Preview : DecodeQuality::Full;
  auto t0 = std::chrono::steady_clock::now();
  auto r = mergeHdr(files, o, progressPrinter(a.flags.count("--quiet")));
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!r) {
    std::fprintf(stderr, "hdr failed: %s\n", r.error().message.c_str());
    return 1;
  }
  std::printf("HDR merge of %zu frames: %dx%d in %.2f s\n", files.size(), r->image.linear.width, r->image.linear.height, sec);
  printHdrReport(r->report);
  const int q = int(num(a, "--quality", 92));
  std::string out = str(a, "-o", str(a, "--out"));
  if (!out.empty()) {
    if (auto w = developTo(r->image, P(out), 0, q); !w) {
      std::fprintf(stderr, "write failed: %s\n", w.error().message.c_str());
      return 1;
    }
    std::printf("  wrote %s\n", out.c_str());
  }
  return writeExtras(a, r->image);
}

int cmdPano(const std::vector<std::string>& argv) {
  Args a = parse(argv, {"-o", "--out", "--projection", "--base", "--long-edge", "--dng", "--tiff", "--quality", "--conf"});
  if (!a.err.empty() || a.pos.size() < 2) {
    std::fputs("usage: dapple pano <2-20 frames> -o out.jpg [--projection sph|cyl|persp] [--no-crop] [--fill] [--base N]\n"
               "                   [--long-edge N] [--no-exposure-comp] [--preview] [--dng out.dng] [--tiff out.tif]\n", stderr);
    if (!a.err.empty()) std::fprintf(stderr, "%s\n", a.err.c_str());
    return 2;
  }
  std::vector<fs::path> files;
  for (auto& s : a.pos) files.push_back(P(s));
  PanoOptions o;
  o.projection = panoProjectionFromString(str(a, "--projection", "cyl"));
  o.crop = !a.flags.count("--no-crop");
  o.fill = a.flags.count("--fill") > 0;
  o.baseIndex = a.opt.count("--base") ? int(num(a, "--base", -1)) : -1;
  o.longEdge = int(num(a, "--long-edge", 0));
  o.exposureCompensation = !a.flags.count("--no-exposure-comp");
  o.quality = a.flags.count("--preview") ? DecodeQuality::Preview : DecodeQuality::Full;
  o.matchConfidence = num(a, "--conf", 0.3);
  auto t0 = std::chrono::steady_clock::now();
  auto r = stitchPanorama(files, o, progressPrinter(a.flags.count("--quiet")));
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!r) {
    std::fprintf(stderr, "pano failed: %s\n", r.error().message.c_str());
    return 1;
  }
  const PanoReport& rep = r->report;
  std::printf("Panorama (%s) of %d/%zu frames: %dx%d (canvas %dx%d) in %.2f s\n", toString(rep.projection), rep.framesUsed,
              files.size(), r->image.linear.width, r->image.linear.height, rep.canvasWidth, rep.canvasHeight, sec);
  std::printf("  load %.2fs features %.2fs estimate %.2fs compose %.2fs, valid %.3f\n", rep.secLoad, rep.secFeatures,
              rep.secEstimate, rep.secCompose, rep.validFraction);
  for (const auto& f : rep.frames)
    std::printf("  %s  f=%.1f yaw %+.2f pitch %+.2f roll %+.2f inliers %d\n", f.path.c_str(), f.focalPx, f.yawDeg, f.pitchDeg,
                f.rollDeg, f.inliers);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  const int q = int(num(a, "--quality", 92));
  std::string out = str(a, "-o", str(a, "--out"));
  if (!out.empty()) {
    if (auto w = developTo(r->image, P(out), 0, q); !w) {
      std::fprintf(stderr, "write failed: %s\n", w.error().message.c_str());
      return 1;
    }
    std::printf("  wrote %s\n", out.c_str());
  }
  return writeExtras(a, r->image);
}

void printInfo(const MotionPhotoInfo& i) {
  std::printf("  %s: file %llu bytes, jpeg %llu, video %llu at offset %llu (%s %dx%d %.2fs, video %s, audio %s), cover pts %lld us\n",
              i.valid ? "VALID" : "INVALID", (unsigned long long)i.fileSize, (unsigned long long)i.jpegLength,
              (unsigned long long)i.videoLength, (unsigned long long)i.videoOffset, i.videoMime.c_str(), i.videoWidth,
              i.videoHeight, i.videoDurationSec, i.videoCodec.c_str(), i.audioCodec.empty() ? "none" : i.audioCodec.c_str(),
              (long long)i.presentationTimestampUs);
  for (auto& p : i.problems) std::printf("  problem: %s\n", p.c_str());
  for (auto& p : i.notes) std::printf("  note: %s\n", p.c_str());
}

int cmdMotion(const std::vector<std::string>& argv) {
  Args a = parse(argv, {"-o", "--out", "--cover", "--start", "--duration", "--cover-time", "--jpeg", "--mp4", "--validate", "--info"});
  if (a.opt.count("--validate")) {
    auto r = validateMotionPhotoFile(P(str(a, "--validate")));
    if (!r) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      return 1;
    }
    printInfo(*r);
    return r->valid ? 0 : 1;
  }
  if (a.opt.count("--info")) {
    auto v = probeVideo(P(str(a, "--info")));
    if (!v) {
      std::fprintf(stderr, "%s\n", v.error().message.c_str());
      return 1;
    }
    std::printf("%s: %dx%d rot %d  %.2f s  %.2f fps  video %s  audio %s  keyframes %zu:", str(a, "--info").c_str(), v->width,
                v->height, v->rotationDeg, v->durationSec, v->fps, v->videoCodec.c_str(),
                v->audioCodec.empty() ? "none" : v->audioCodec.c_str(), v->keyframesSec.size());
    for (std::size_t i = 0; i < v->keyframesSec.size() && i < 12; ++i) std::printf(" %.2f", v->keyframesSec[i]);
    std::puts(v->keyframesSec.size() > 12 ? " ..." : "");
    return 0;
  }
  const std::string out = str(a, "-o", str(a, "--out"));
  if (a.opt.count("--jpeg") && a.opt.count("--mp4")) {  // container only (clip trimmed elsewhere)
    MotionPhotoOptions mo;
    mo.presentationTimestampUs = std::int64_t(num(a, "--cover-time", -1) * 1e6);
    mo.legacyMicroVideo = a.flags.count("--legacy") > 0;
    auto r = writeMotionPhoto(P(str(a, "--jpeg")), P(str(a, "--mp4")), P(out), mo);
    if (!r) {
      std::fprintf(stderr, "motion failed: %s\n", r.error().message.c_str());
      return 1;
    }
    printInfo(*r);
    return 0;
  }
  if (a.pos.size() != 1 || out.empty() || !a.opt.count("--cover")) {
    std::fputs("usage: dapple motion <video.mp4> --cover still.jpg -o name_MP.jpg [--start S] [--duration 3] [--cover-time T] [--no-audio] [--legacy]\n"
               "       dapple motion --jpeg still.jpg --mp4 clip.mp4 -o name_MP.jpg [--cover-time SEC_IN_CLIP]\n"
               "       dapple motion --validate file_MP.jpg | --info video.mp4\n", stderr);
    return 2;
  }
  MotionFromVideoRequest rq;
  rq.video = P(a.pos[0]);
  rq.coverJpeg = P(str(a, "--cover"));
  rq.output = P(out);
  rq.startSec = num(a, "--start", 0);
  rq.durationSec = num(a, "--duration", 3.0);
  rq.coverSec = num(a, "--cover-time", -1);
  rq.keepAudio = !a.flags.count("--no-audio");
  rq.legacyMicroVideo = a.flags.count("--legacy") > 0;
  auto t0 = std::chrono::steady_clock::now();
  auto r = createMotionPhotoFromVideo(rq);
  if (!r) {
    std::fprintf(stderr, "motion failed: %s\n", r.error().message.c_str());
    return 1;
  }
  std::printf("Motion Photo written in %.2f s: clip starts at %.3f s (source), %.3f s long, %d frames, cover at %.3f s in clip%s%s\n",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), r->trim.actualStartSec,
              r->trim.actualDurationSec, r->trim.videoFrames, r->coverInClipSec, r->trim.note.empty() ? "" : " - ",
              r->trim.note.c_str());
  printInfo(r->info);
  return 0;
}

void collect(const fs::path& p, bool recursive, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    if (recursive) {
      for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) out.push_back(it->path());
    } else {
      for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) out.push_back(it->path());
    }
  } else {
    out.push_back(p);
  }
}

int cmdDetect(const std::vector<std::string>& argv) {
  Args a = parse(argv, {"--gap", "--pano-gap", "--min-ev"});
  if (a.pos.empty()) {
    std::fputs("usage: dapple detect <files or folders...> [-r] [--no-verify] [--gap SEC] [--pano-gap SEC] [--min-ev EV] [--list]\n", stderr);
    return 2;
  }
  std::vector<fs::path> paths;
  for (auto& s : a.pos) collect(P(s), a.flags.count("-r") > 0, paths);
  std::sort(paths.begin(), paths.end());
  std::vector<FrameInfo> frames;
  int unreadable = 0;
  for (auto& p : paths) {
    std::string ext = U(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".arw" && ext != ".jpg" && ext != ".jpeg" && ext != ".dng" && ext != ".raf" && ext != ".cr3" && ext != ".nef") continue;
    auto fi = probeFrame(p);
    if (fi) frames.push_back(std::move(*fi));
    else ++unreadable;
  }
  std::printf("%zu frames probed (%d unreadable)\n", frames.size(), unreadable);
  if (a.flags.count("--list"))
    for (auto& f : frames)
      std::printf("  %s  t=%lld  %.4fs ISO %.0f f/%.1f %.0fmm  EV %.2f  seq %s %lld/%d file %lld mode %d\n",
                  U(f.path.filename()).c_str(), f.meta.timestamp, f.meta.shutterSeconds, f.meta.iso, f.meta.aperture,
                  f.meta.focalLengthMm, f.exposureStops, f.hasSequence ? "yes" : "no", f.sequenceImageNumber,
                  f.sequenceLength, f.sequenceFileNumber, f.releaseMode);
  DetectOptions d;
  d.verifyContent = !a.flags.count("--no-verify");
  d.maxGapSeconds = num(a, "--gap", d.maxGapSeconds);
  d.panoGapSeconds = num(a, "--pano-gap", d.panoGapSeconds);
  d.minEvStep = num(a, "--min-ev", d.minEvStep);
  auto t0 = std::chrono::steady_clock::now();
  auto groups = detectSequences(frames, d);
  std::printf("%zu groups (%.1f s)\n", groups.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  for (auto& g : groups) {
    std::printf("%s  %zu frames  conf %.2f  %s\n", g.kind == SequenceGroup::Kind::Bracket ? "BRACKET" : "PANORAMA",
                g.paths.size(), g.confidence, g.reason.c_str());
    for (std::size_t i = 0; i < g.paths.size(); ++i) std::printf("    %s  (%+.2f EV)\n", g.paths[i].c_str(), g.evSteps[i]);
  }
  return 0;
}

}  // namespace

bool handles(const std::string& cmd) { return cmd == "hdr" || cmd == "pano" || cmd == "motion" || cmd == "detect"; }

int run(const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd == "hdr") return cmdHdr(args);
  if (cmd == "pano") return cmdPano(args);
  if (cmd == "motion") return cmdMotion(args);
  return cmdDetect(args);
}

}  // namespace dapple::mergetool
