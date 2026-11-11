// Bracket / panorama candidate detection from EXIF (+ Sony maker-note sequence
// fields exposed by LibRaw) and, optionally, the embedded previews.

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

#include "dapple/decoder.h"
#include "dapple/merge.h"
#include "file_util.h"
#include "libraw_stream.h"
#include "merge_internal.h"

#ifdef DAPPLE_HAVE_OPENCV
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace dapple {

namespace {

bool isRawExtension(const std::filesystem::path& p) {
  static const char* kExts[] = {".arw", ".srf", ".sr2", ".raf", ".dng", ".cr2", ".cr3", ".nef",
                                ".nrw", ".orf", ".rw2", ".pef", ".srw", ".3fr", ".iiq", ".erf"};
  const std::string ext = lowerExtension(p);
  return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
}

}  // namespace

Result<FrameInfo> probeFrame(const std::filesystem::path& path) {
  FrameInfo fi;
  fi.path = path;
  static const DecoderRegistry registry = DecoderRegistry::withBuiltins();
  const ImageDecoder* dec = registry.find(path);
  if (!dec) return fail("Unsupported file type: " + pathToUtf8(path));
  auto meta = dec->probe(path);
  if (!meta) return std::unexpected(meta.error());
  fi.meta = *meta;
  if (isRawExtension(path)) {
    MetaDatastream stream(path);
    if (stream.valid()) {
      auto raw = std::make_unique<LibRaw>();
      if (raw->open_datastream(&stream) == LIBRAW_SUCCESS) {
        const auto& s = raw->imgdata.makernotes.sony;
        if (fi.meta.make == "Sony" && s.Sony0x9400_version != 0) {
          fi.sequenceImageNumber = s.Sony0x9400_SequenceImageNumber;
          fi.sequenceLength = s.Sony0x9400_SequenceLength1;
          fi.sequenceFileNumber = s.Sony0x9400_SequenceFileNumber;
          fi.releaseMode = s.Sony0x9400_ReleaseMode2;
          // "sequence of 1" is what every single shot reports; only a longer sequence says anything
          fi.hasSequence = fi.sequenceLength > 1;
        }
      }
    }
  }
  if (fi.meta.shutterSeconds > 0 && fi.meta.iso > 0 && fi.meta.aperture > 0) {
    fi.exposureKnown = true;
    fi.exposureStops = std::log2(double(fi.meta.shutterSeconds) * fi.meta.iso / (double(fi.meta.aperture) * fi.meta.aperture));
  }
  fi.orientationKey = std::to_string(fi.meta.sensorWidth) + "x" + std::to_string(fi.meta.sensorHeight);
  return fi;
}

namespace {

bool sameSetup(const FrameInfo& a, const FrameInfo& b) {
  if (a.meta.make != b.meta.make || a.meta.model != b.meta.model) return false;
  if (a.orientationKey != b.orientationKey) return false;
  const float fa = a.meta.focalLengthMm, fb = b.meta.focalLengthMm;
  if (fa > 0 && fb > 0 && std::fabs(fa - fb) > 0.02f * std::max(fa, fb)) return false;
  return true;
}

// -------- preview based verification --------
struct PreviewCache {
  std::map<int, Image8> previews;
  const std::vector<FrameInfo>* frames = nullptr;
  const Image8* get(int idx, int longEdge) {
    auto it = previews.find(idx);
    if (it == previews.end()) {
      Image8 img;
      if (isRawExtension((*frames)[idx].path)) {
        auto p = extractEmbeddedPreview((*frames)[idx].path, longEdge);
        if (p) img = std::move(*p);
      }
      it = previews.emplace(idx, std::move(img)).first;
    }
    return it->second.data.empty() ? nullptr : &it->second;
  }
};

std::vector<float> lumaSmall(const Image8& im, int edge, int& w, int& h) {
  ImageF f(im.width, im.height);
  for (std::size_t i = 0; i < f.data.size(); ++i) f.data[i] = im.data[i] / 255.f;
  ImageF s = resizeToLongEdge(f, edge);
  w = s.width;
  h = s.height;
  std::vector<float> l(std::size_t(w) * h);
  for (std::size_t i = 0; i < l.size(); ++i)
    l[i] = 0.299f * s.data[3 * i] + 0.587f * s.data[3 * i + 1] + 0.114f * s.data[3 * i + 2];
  return l;
}

// Best zero-mean NCC over small shifts: same scene (brackets) scores high even across exposures.
double sceneSimilarity(const Image8& a, const Image8& b) {
  int wa, ha, wb, hb;
  auto la = lumaSmall(a, 96, wa, ha);
  auto lb = lumaSmall(b, 96, wb, hb);
  if (wa != wb || ha != hb) return 0;
  double best = -1;
  const int S = 4;
  for (int dy = -S; dy <= S; ++dy)
    for (int dx = -S; dx <= S; ++dx) {
      double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
      long n = 0;
      for (int y = std::max(0, -dy); y < std::min(ha, ha - dy); ++y)
        for (int x = std::max(0, -dx); x < std::min(wa, wa - dx); ++x) {
          const double u = la[std::size_t(y) * wa + x], v = lb[std::size_t(y + dy) * wb + x + dx];
          sa += u; sb += v; saa += u * u; sbb += v * v; sab += u * v;
          ++n;
        }
      if (n < 100) continue;
      const double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
      if (va > 1e-9 && vb > 1e-9) best = std::max(best, cov / std::sqrt(va * vb));
    }
  return best;
}

#ifdef DAPPLE_HAVE_OPENCV
// Feature overlap between two previews: inlier count and the overlapped fraction of image b.
bool pairOverlap(const Image8& a, const Image8& b, int& inliers, double& overlap) {
  auto gray = [](const Image8& im) {
    cv::Mat m(im.height, im.width, CV_8UC3, const_cast<std::uint8_t*>(im.data.data())), g;
    cv::cvtColor(m, g, cv::COLOR_RGB2GRAY);
    return g;
  };
  cv::Mat ga = gray(a), gb = gray(b);
  auto akaze = cv::AKAZE::create();
  std::vector<cv::KeyPoint> ka, kb;
  cv::Mat da, db;
  akaze->detectAndCompute(ga, cv::noArray(), ka, da);
  akaze->detectAndCompute(gb, cv::noArray(), kb, db);
  inliers = 0;
  overlap = 0;
  if (ka.size() < 20 || kb.size() < 20) return false;
  cv::BFMatcher bf(cv::NORM_HAMMING);
  std::vector<std::vector<cv::DMatch>> knn;
  bf.knnMatch(da, db, knn, 2);
  std::vector<cv::Point2f> pa, pb;
  for (const auto& m : knn)
    if (m.size() == 2 && m[0].distance < 0.8f * m[1].distance) {
      pa.push_back(ka[m[0].queryIdx].pt);
      pb.push_back(kb[m[0].trainIdx].pt);
    }
  if (pa.size() < 12) return false;
  std::vector<std::uint8_t> inl;
  cv::Mat H = cv::findHomography(pb, pa, cv::RANSAC, 3.0, inl);  // b -> a
  if (H.empty()) return false;
  inliers = cv::countNonZero(inl);
  // overlap: intersection of a's frame with b's warped frame, relative to a's area
  std::vector<cv::Point2f> quadB = {{0, 0}, {float(gb.cols), 0}, {float(gb.cols), float(gb.rows)}, {0, float(gb.rows)}}, q;
  cv::perspectiveTransform(quadB, q, H);
  std::vector<cv::Point2f> rectA = {{0, 0}, {float(ga.cols), 0}, {float(ga.cols), float(ga.rows)}, {0, float(ga.rows)}};
  std::vector<cv::Point2f> inter;
  cv::intersectConvexConvex(rectA, q, inter);
  overlap = inter.size() >= 3 ? cv::contourArea(inter) / (double(ga.cols) * ga.rows) : 0;
  return inliers >= 15;
}
#endif

}  // namespace

std::vector<SequenceGroup> detectSequences(const std::vector<FrameInfo>& frames, const DetectOptions& opt,
                                           const MergeControl& ctl) {
  std::vector<SequenceGroup> groups;
  const int n = int(frames.size());
  if (n < 2) return groups;

  std::vector<int> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    if (frames[a].meta.timestamp != frames[b].meta.timestamp) return frames[a].meta.timestamp < frames[b].meta.timestamp;
    if (frames[a].hasSequence && frames[b].hasSequence && frames[a].sequenceFileNumber != frames[b].sequenceFileNumber)
      return frames[a].sequenceFileNumber < frames[b].sequenceFileNumber;
    return pathToUtf8(frames[a].path) < pathToUtf8(frames[b].path);
  });

  PreviewCache cache;
  cache.frames = &frames;
  std::vector<bool> used(n, false);
  auto gap = [&](int a, int b) -> double {
    const auto ta = frames[a].meta.timestamp, tb = frames[b].meta.timestamp;
    if (ta == 0 || tb == 0) return 1e9;
    return double(tb - ta);
  };
  auto bracketMode = [](int rm) { return rm == 2 || rm == 23 || rm == 9; };

  // ---------------- brackets ----------------
  {
    std::vector<int> run;
    auto flush = [&]() {
      // split the run into sets: a set ends when an exposure value repeats
      std::vector<int> set;
      std::vector<double> evs;
      auto closeSet = [&]() {
        if (set.size() >= 2) {
          double lo = 1e9, hi = -1e9;
          for (double e : evs) { lo = std::min(lo, e); hi = std::max(hi, e); }
          bool seqBoost = false, modeBoost = false;
          for (int i : set) {
            seqBoost |= frames[i].hasSequence && frames[i].sequenceLength == int(set.size());
            modeBoost |= frames[i].hasSequence && bracketMode(frames[i].releaseMode);
          }
          // Without camera confirmation (Sony release mode / sequence length) an exposure set must look like
          // a deliberate bracket: large enough spread and regular steps. Auto-exposure drift in P/A mode does not.
          std::vector<double> sorted = evs;
          std::sort(sorted.begin(), sorted.end());
          double minStep = 1e9, maxStep = 0;
          for (std::size_t k = 1; k < sorted.size(); ++k) {
            minStep = std::min(minStep, sorted[k] - sorted[k - 1]);
            maxStep = std::max(maxStep, sorted[k] - sorted[k - 1]);
          }
          const bool regular = maxStep - minStep <= 0.35 * maxStep + 0.1;
          const bool deliberate = (hi - lo >= opt.minEvSpread && minStep >= opt.minEvStep && regular);
          if (((seqBoost || modeBoost) ? hi - lo >= 0.3 : deliberate) && set.size() <= 9) {
            SequenceGroup g;
            g.kind = SequenceGroup::Kind::Bracket;
            g.indices = set;
            double conf = 0.5;
            std::string why = "capture times within " + std::to_string(int(opt.maxGapSeconds)) + " s, same lens setup, exposure spread " +
                              std::to_string(hi - lo).substr(0, 4) + " EV";
            if (seqBoost) { conf += 0.2; why += ", Sony sequence length matches"; }
            if (!seqBoost && !modeBoost) { conf -= 0.1; why += ", regular exposure steps (not confirmed by the camera)"; }
            if (modeBoost) { conf += 0.15; why += ", camera was in bracket/HDR release mode"; }
            bool contentOk = true;
            if (opt.verifyContent) {
              const Image8* first = cache.get(set.front(), 640);
              if (first) {
                double minSim = 1;
                for (std::size_t k = 1; k < set.size(); ++k) {
                  const Image8* other = cache.get(set[k], 640);
                  if (other) minSim = std::min(minSim, sceneSimilarity(*first, *other));
                }
                contentOk = minSim >= 0.55;
                if (contentOk) { conf += 0.15; why += ", previews show the same scene"; }
                else why += ", but previews differ (" + std::to_string(minSim).substr(0, 4) + ")";
              }
            }
            if (contentOk) {
              g.confidence = std::min(1.0, conf);
              g.reason = why;
              for (int i : set) {
                g.paths.push_back(pathToUtf8(frames[i].path));
                g.evSteps.push_back(frames[i].exposureStops - frames[set.front()].exposureStops);
              }
              g.startTime = frames[set.front()].meta.timestamp;
              g.endTime = frames[set.back()].meta.timestamp;
              for (int i : set) used[i] = true;
              groups.push_back(std::move(g));
            }
          }
        }
        set.clear();
        evs.clear();
      };
      for (int i : run) {
        bool repeats = false;
        for (double e : evs) if (std::fabs(e - frames[i].exposureStops) < 0.3) repeats = true;
        // Sony sequence numbers also delimit sets
        if (!set.empty() && frames[i].hasSequence && frames[set.back()].hasSequence &&
            frames[i].sequenceImageNumber != frames[set.back()].sequenceImageNumber &&
            frames[i].sequenceFileNumber <= frames[set.back()].sequenceFileNumber)
          repeats = true;
        if (repeats) closeSet();
        set.push_back(i);
        evs.push_back(frames[i].exposureStops);
      }
      closeSet();
      run.clear();
    };
    for (int idx = 0; idx < n; ++idx) {
      const int i = order[idx];
      if (!frames[i].exposureKnown) { flush(); continue; }
      if (!run.empty()) {
        const int prev = run.back();
        if (gap(prev, i) > opt.maxGapSeconds || !sameSetup(frames[prev], frames[i]) ||
            std::fabs(frames[prev].meta.aperture - frames[i].meta.aperture) > 0.05f * std::max(1.f, frames[i].meta.aperture))
          flush();
      }
      run.push_back(i);
    }
    flush();
  }
  ctl.report(0.5f, "detect");

  // ---------------- panoramas ----------------
  {
    std::vector<int> run;
    auto flush = [&]() {
      if (run.size() >= 2) {
        // verify consecutive pairs (previews) and split where overlap is missing
        std::vector<int> cur{run[0]};
        double confSum = 0;
        int pairs = 0;
        auto closeCur = [&]() {
          if (cur.size() >= 2) {
            SequenceGroup g;
            g.kind = SequenceGroup::Kind::Panorama;
            g.indices = cur;
            g.confidence = std::min(1.0, pairs ? confSum / pairs : 0.4);
            g.reason = "consecutive captures within " + std::to_string(int(opt.panoGapSeconds)) + " s, same focal length";
            if (opt.verifyContent && pairs) g.reason += ", neighbouring previews overlap by feature matching";
            for (int i : cur) {
              g.paths.push_back(pathToUtf8(frames[i].path));
              g.evSteps.push_back(frames[i].exposureKnown && frames[cur.front()].exposureKnown
                                      ? frames[i].exposureStops - frames[cur.front()].exposureStops : 0.0);
              used[i] = true;
            }
            g.startTime = frames[cur.front()].meta.timestamp;
            g.endTime = frames[cur.back()].meta.timestamp;
            groups.push_back(std::move(g));
          }
          cur.clear();
          confSum = 0;
          pairs = 0;
        };
        for (std::size_t k = 1; k < run.size(); ++k) {
          bool linked = true;
          double c = 0.4;
          if (opt.verifyContent) {
#ifdef DAPPLE_HAVE_OPENCV
            const Image8* a = cache.get(run[k - 1], 640);
            const Image8* b = cache.get(run[k], 640);
            if (a && b) {
              int inl = 0;
              double ov = 0;
              const bool ok = pairOverlap(*a, *b, inl, ov);
              // a pano neighbour overlaps partially; ~identical views are a burst, not a pano
              linked = ok && ov >= opt.minPanoOverlap && ov <= opt.maxPanoOverlap;
              c = std::min(1.0, 0.5 + inl / 200.0);
            }
#endif
          }
          if (!linked) { closeCur(); cur.push_back(run[k]); continue; }
          cur.push_back(run[k]);
          confSum += c;
          ++pairs;
          if (cur.size() == 20) closeCur(), cur.push_back(run[k]);
        }
        closeCur();
      }
      run.clear();
    };
    for (int idx = 0; idx < n; ++idx) {
      const int i = order[idx];
      if (used[i]) { flush(); continue; }
      if (!run.empty()) {
        const int prev = run.back();
        const bool ev = !frames[prev].exposureKnown || !frames[i].exposureKnown ||
                        std::fabs(frames[prev].exposureStops - frames[i].exposureStops) <= 1.0;
        if (gap(prev, i) > opt.panoGapSeconds || gap(prev, i) < 0 || !sameSetup(frames[prev], frames[i]) || !ev) flush();
      }
      run.push_back(i);
    }
    flush();
  }
  ctl.report(1.0f, "detect");

  std::sort(groups.begin(), groups.end(), [](const SequenceGroup& a, const SequenceGroup& b) {
    return a.startTime < b.startTime;
  });
  return groups;
}

}  // namespace dapple
