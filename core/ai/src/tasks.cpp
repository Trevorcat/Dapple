// Task wrappers. Pre/post-processing mirrors scripts/ai-eval/ai_ref.py (which mirrors the research scripts) line by line.
#include "ai/tasks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "imageops.h"

namespace dapple::ai {
namespace {

// ---- model handles (one lazy Model per logical id; the registry in ai.cpp frees their sessions when idle) ----------------------------
Model& modelFor(const char* id) {
  static std::mutex mu;
  static std::map<std::string, std::shared_ptr<Model>>* models = new std::map<std::string, std::shared_ptr<Model>>;
  std::lock_guard<std::mutex> l(mu);
  auto& p = (*models)[id];
  if (!p) p = Model::open(id);
  return *p;
}

RunOptions ro(const TaskOptions& o) {
  RunOptions r;
  r.threads = o.threads;
  r.cancel = o.cancel;
  return r;
}

Status checkImage(const Image& im) {
  if (!im.rgb || im.w < 2 || im.h < 2 || im.stride < im.w * 3) return {Code::BadInput, "bad image"};
  return {};
}

TensorView view(DType t, std::vector<int64_t> dims, const void* data) {
  TensorView v;
  v.type = t;
  v.dims = std::move(dims);
  v.data = data;
  return v;
}

// (px/scale - mean)/std in float32, NHWC (what prep(..., nchw=False) does)
std::vector<float> toNhwc(const img::Rgb& r, float scale, float mean, float stdv, bool norm) {
  std::vector<float> f(r.px.size());
  for (size_t i = 0; i < f.size(); ++i) {
    float v = float(r.px[i]) / scale;
    if (norm) v = (v - mean) / stdv;
    f[i] = v;
  }
  return f;
}

float sigmoidf(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// ---- faces ------------------------------------------------------------------------------------------------------------------------
struct Anchors {
  std::vector<float> x, y;
  Anchors() {
    for (auto [fm, k] : {std::pair<int, int>{16, 2}, std::pair<int, int>{8, 6}})
      for (int yy = 0; yy < fm; ++yy)
        for (int xx = 0; xx < fm; ++xx)
          for (int i = 0; i < k; ++i) {
            x.push_back((xx + 0.5f) / fm);
            y.push_back((yy + 0.5f) / fm);
          }
  }
};
const Anchors& anchors() {
  static const Anchors a;
  return a;
}

double iouBox(const float* a, const float* b) {
  const double ax0 = a[0] - a[2] / 2.0, ay0 = a[1] - a[3] / 2.0, ax1 = a[0] + a[2] / 2.0, ay1 = a[1] + a[3] / 2.0;
  const double bx0 = b[0] - b[2] / 2.0, by0 = b[1] - b[3] / 2.0, bx1 = b[0] + b[2] / 2.0, by1 = b[1] + b[3] / 2.0;
  const double iw = std::max(0.0, std::min(ax1, bx1) - std::max(ax0, bx0)), ih = std::max(0.0, std::min(ay1, by1) - std::max(ay0, by0));
  const double inter = iw * ih, u = double(a[2]) * a[3] + double(b[2]) * b[3] - inter;
  return u > 0 ? inter / u : 0.0;
}

}  // namespace

namespace {
struct Cand {
  float score, box[4], kp[6][2];
};

// One BlazeFace pass over `im` (letterboxed to 128): every anchor above minScore, decoded into the pixel frame of `im` shifted by (offX, offY).
Status detectCands(const Image& im, float minScore, const TaskOptions& o, float offX, float offY, std::vector<Cand>& cand) {
  int ox = 0, oy = 0;
  const img::Rgb sq = img::letterboxSquare(im, ox, oy);
  const int s = sq.w;
  const img::Rgb small = img::resizeBilinear(sq.px.data(), s, s, s * 3, 128, 128);
  std::vector<float> in = toNhwc(small, 1.0f, 127.5f, 127.5f, true);
  std::vector<Tensor> res;
  Status st = run(modelFor("face_detector"), {view(DType::F32, {1, 128, 128, 3}, in.data())}, res, ro(o));
  if (!st) return st;
  if (res.size() < 2) return {Code::RunFailed, "face_detector: unexpected outputs"};
  const Tensor* raw = &res[0];
  const Tensor* sc = &res[1];
  if (raw->count() != 896 * 16) std::swap(raw, sc);
  if (raw->count() != 896 * 16 || sc->count() != 896) return {Code::RunFailed, "face_detector: unexpected output shapes"};
  const Anchors& an = anchors();
  for (int i = 0; i < 896; ++i) {
    const float logit = std::min(100.0f, std::max(-100.0f, sc->f32()[i]));
    const float p = sigmoidf(logit);
    if (p < minScore) continue;
    const float* r = raw->f32() + size_t(i) * 16;
    Cand c;
    c.score = p;
    c.box[0] = (r[0] / 128 + an.x[size_t(i)]) * float(s) - float(ox) + offX;
    c.box[1] = (r[1] / 128 + an.y[size_t(i)]) * float(s) - float(oy) + offY;
    c.box[2] = r[2] / 128 * float(s);
    c.box[3] = r[3] / 128 * float(s);
    for (int k = 0; k < 6; ++k) {
      c.kp[k][0] = (r[4 + 2 * k] / 128 + an.x[size_t(i)]) * float(s) - float(ox) + offX;
      c.kp[k][1] = (r[5 + 2 * k] / 128 + an.y[size_t(i)]) * float(s) - float(oy) + offY;
    }
    cand.push_back(c);
  }
  return {};
}

// weighted NMS (MediaPipe): every candidate overlapping the best by IoU > 0.3 is merged into it, weighted by score
void mergeCands(std::vector<Cand> cand, std::vector<Face>& out) {
  std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
  while (!cand.empty()) {
    const Cand best = cand[0];
    std::vector<Cand> grp, rest;
    for (const Cand& c : cand) (iouBox(best.box, c.box) > 0.3 ? grp : rest).push_back(c);
    double wsum = 0;
    for (auto& c : grp) wsum += c.score;
    Face f;
    f.score = best.score;
    double b[4] = {0, 0, 0, 0}, kp[6][2] = {};
    for (auto& c : grp) {
      for (int j = 0; j < 4; ++j) b[j] += double(c.box[j]) * c.score;
      for (int k = 0; k < 6; ++k) for (int j = 0; j < 2; ++j) kp[k][j] += double(c.kp[k][j]) * c.score;
    }
    f.cx = float(b[0] / wsum);
    f.cy = float(b[1] / wsum);
    f.w = float(b[2] / wsum);
    f.h = float(b[3] / wsum);
    for (int k = 0; k < 6; ++k) for (int j = 0; j < 2; ++j) f.kp[k][j] = float(kp[k][j] / wsum);
    out.push_back(f);
    cand = std::move(rest);
  }
}

struct Blob {
  float x0, y0, x1, y1;  // image pixels
  int area;              // grid cells
};
bool insideBox(const Face& f, float x0, float y0, float x1, float y1) { return f.cx >= x0 && f.cx <= x1 && f.cy >= y0 && f.cy <= y1; }
bool centreInFace(const Face& a, const Face& b) { return std::fabs(a.cx - b.cx) <= b.w / 2 && std::fabs(a.cy - b.cy) <= b.h / 2; }

// 4-connected components of the person mask on a 128 grid; regions of at least 0.4% of the frame, largest first
std::vector<Blob> personBlobs(const uint8_t* mask, int mw, int mh, int W, int H) {
  constexpr int G = 128;
  std::vector<uint8_t> g(size_t(G) * G), seen(size_t(G) * G, 0);
  for (int y = 0; y < G; ++y)
    for (int x = 0; x < G; ++x)
      g[size_t(y) * G + x] = mask[size_t(std::min(mh - 1, (y * 2 + 1) * mh / (2 * G))) * size_t(mw) + size_t(std::min(mw - 1, (x * 2 + 1) * mw / (2 * G)))] > 127;
  std::vector<Blob> out;
  std::vector<int> stack;
  for (int i = 0; i < G * G; ++i) {
    if (!g[size_t(i)] || seen[size_t(i)]) continue;
    int x0 = G, y0 = G, x1 = -1, y1 = -1, area = 0;
    stack.assign(1, i);
    seen[size_t(i)] = 1;
    while (!stack.empty()) {
      const int c = stack.back();
      stack.pop_back();
      const int cx = c % G, cy = c / G;
      ++area, x0 = std::min(x0, cx), x1 = std::max(x1, cx), y0 = std::min(y0, cy), y1 = std::max(y1, cy);
      const int nb[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
      for (auto& n : nb) {
        if (n[0] < 0 || n[1] < 0 || n[0] >= G || n[1] >= G) continue;
        const int k = n[1] * G + n[0];
        if (g[size_t(k)] && !seen[size_t(k)]) seen[size_t(k)] = 1, stack.push_back(k);
      }
    }
    if (area * 250 < G * G) continue;
    out.push_back({float(x0) * float(W) / G, float(y0) * float(H) / G, float(x1 + 1) * float(W) / G, float(y1 + 1) * float(H) / G, area});
  }
  std::stable_sort(out.begin(), out.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });
  return out;
}
}  // namespace

Status detectFaces(const Image& im, std::vector<Face>& out, float minScore, const TaskOptions& o) {
  out.clear();
  if (auto s = checkImage(im); !s) return s;
  std::vector<Cand> cand;
  if (auto s = detectCands(im, minScore, o, 0, 0, cand); !s) return s;
  mergeCands(std::move(cand), out);
  return {};
}

Status findFaces(const Image& im, const uint8_t* personMask, int mw, int mh, std::vector<FoundFace>& out, const TaskOptions& o, FaceFindStats* stats) {
  out.clear();
  if (stats) *stats = FaceFindStats{};
  if (auto s = checkImage(im); !s) return s;
  auto cancelled = [&] { return o.cancel && o.cancel->load(); };
  std::vector<Face> f0;
  if (auto s = detectFaces(im, f0, 0.5f, o); !s) return s;
  if (stats) stats->passes = 1;
  for (const Face& f : f0) {
    if (cancelled()) return {Code::Cancelled, "cancelled"};
    FoundFace ff;
    ff.face = f;
    if (auto s = faceLandmarks(im, f, ff.lm, o); !s) return s;
    if (ff.lm.valid) out.push_back(std::move(ff));  // the rest: too small for a good crop, profiles, false hits
  }
  if (!personMask || mw <= 0 || mh <= 0) return {};
  // a person region is explained when it holds a mesh-validated face and is not much wider than one person (about 4 face widths)
  std::vector<Blob> todo;
  for (const Blob& b : personBlobs(personMask, mw, mh, im.w, im.h)) {
    bool explained = false;
    for (const FoundFace& ff : out)
      if (insideBox(ff.face, b.x0, b.y0, b.x1, b.y1) && b.x1 - b.x0 <= 4.0f * ff.face.w) explained = true;
    if (!explained) todo.push_back(b);
  }
  if (todo.empty()) return {};
  // square windows over the upper part of each unexplained region (side 0.4 of its height, 50% overlap), at most 10 in all
  const int side = std::min(im.w, im.h);
  std::vector<Cand> cand;
  int tiles = 0;
  for (const Blob& b : todo) {
    const float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    const float S = std::min(float(side), std::max(0.4f * bh, 160.0f));
    const float y0 = std::min(std::max(0.0f, b.y0 - 0.1f * S), float(im.h) - S);
    std::vector<float> xs;
    if (bw <= S) xs.push_back(b.x0 + bw / 2 - S / 2);
    else
      for (float x = b.x0; xs.empty() || x < b.x1 - S / 2; x += S / 2) xs.push_back(x);
    for (float xf : xs) {
      if (tiles >= 10) break;
      if (cancelled()) return {Code::Cancelled, "cancelled"};
      const int ix = int(std::min(std::max(0.0f, xf), float(im.w) - S)), iy = int(y0), is = int(S);
      const Image w{im.rgb + size_t(iy) * size_t(im.stride) + size_t(ix) * 3, is, is, im.stride};
      if (auto s = detectCands(w, 0.5f, o, float(ix), float(iy), cand); !s) return s;
      ++tiles;
    }
  }
  if (stats) stats->passes += tiles;
  std::vector<Face> ft;
  mergeCands(std::move(cand), ft);  // sorted by score; a window-clipped duplicate is dropped when its centre lies in a better face's box
  std::vector<Face> kept;
  for (const Face& f : ft) {
    bool dup = false;
    for (const FoundFace& ff : out) dup |= centreInFace(f, ff.face);
    for (const Face& k : kept) dup |= centreInFace(f, k) || centreInFace(k, f);
    if (!dup) kept.push_back(f);
  }
  for (const Face& f : kept) {
    if (cancelled()) return {Code::Cancelled, "cancelled"};
    FoundFace ff;
    ff.face = f;
    if (auto s = faceLandmarks(im, f, ff.lm, o); !s) return s;
    if (stats) ++stats->crops;
    if (ff.lm.valid) out.push_back(std::move(ff));
  }
  return {};
}

Status faceLandmarks(const Image& im, const Face& face, Landmarks& out, const TaskOptions& o) {
  out = Landmarks{};
  if (auto s = checkImage(im); !s) return s;
  const double ang = std::atan2(double(face.kp[1][1]) - face.kp[0][1], double(face.kp[1][0]) - face.kp[0][0]);
  const double size = std::max(face.w, face.h) * 1.5;
  const double c = std::cos(ang), sn = std::sin(ang), k = size / 256;
  const double a[6] = {c * k, -sn * k, face.cx - 128 * k * c + 128 * k * sn, sn * k, c * k, face.cy - 128 * k * sn - 128 * k * c};
  const img::Rgb crop = img::affineBilinear(im, a, 256, 256);
  std::vector<float> in = toNhwc(crop, 255.0f, 0, 1, false);
  std::vector<Tensor> res;
  Status st = run(modelFor("face_landmarks"), {view(DType::F32, {1, 256, 256, 3}, in.data())}, res, ro(o));
  if (!st) return st;
  if (res.size() < 2 || res[0].count() != 478 * 3 || res[1].count() != 1) return {Code::RunFailed, "face_landmarks: unexpected outputs"};
  const float* lm = res[0].f32();
  out.faceFlag = res[1].f32()[0];
  out.valid = out.faceFlag > 0;
  for (int i = 0; i < 478; ++i) {
    const double u = lm[i * 3], v = lm[i * 3 + 1];
    out.xy[i][0] = float(a[0] * u + a[1] * v + a[2]);
    out.xy[i][1] = float(a[3] * u + a[4] * v + a[5]);
    out.z[i] = float(lm[i * 3 + 2] * k);
  }
  for (int i = 0; i < 6; ++i) out.crop[i] = float(a[i]);
  return {};
}

// ---- poses ----------------------------------------------------------------------------------------------------------------------------
namespace {
// SSD anchors of the BlazePose detector (224 input): strides 8, 16, 32 x3 -> 28x28x2, 14x14x2, 7x7x6 = 2254, centres (x + .5) / fm
struct PoseAnchors {
  std::vector<float> x, y;
  PoseAnchors() {
    for (auto [fm, k] : {std::pair<int, int>{28, 2}, std::pair<int, int>{14, 2}, std::pair<int, int>{7, 6}})
      for (int yy = 0; yy < fm; ++yy)
        for (int xx = 0; xx < fm; ++xx)
          for (int i = 0; i < k; ++i) {
            x.push_back((xx + 0.5f) / fm);
            y.push_back((yy + 0.5f) / fm);
          }
  }
};
struct PoseCand {
  float score, box[4], kp[2][2];  // box cx cy w h; kp0 = mid hip, kp1 = the point on the full-body circle
};
}  // namespace

Status findPoses(const Image& im, std::vector<Pose>& out, int maxPoses, const TaskOptions& o) {
  out.clear();
  if (auto s = checkImage(im); !s) return s;
  static const PoseAnchors an;
  int ox = 0, oy = 0;
  const img::Rgb sq = img::letterboxSquare(im, ox, oy);
  const int s = sq.w;
  const img::Rgb small = img::resizeBilinear(sq.px.data(), s, s, s * 3, 224, 224);
  std::vector<float> in = toNhwc(small, 1.0f, 127.5f, 127.5f, true);
  std::vector<Tensor> res;
  if (auto st = run(modelFor("pose_detector"), {view(DType::F32, {1, 224, 224, 3}, in.data())}, res, ro(o)); !st) return st;
  if (res.size() < 2) return {Code::RunFailed, "pose_detector: unexpected outputs"};
  const Tensor* raw = &res[0];
  const Tensor* sc = &res[1];
  if (raw->count() != 2254 * 12) std::swap(raw, sc);
  if (raw->count() != 2254 * 12 || sc->count() != 2254) return {Code::RunFailed, "pose_detector: unexpected output shapes"};
  std::vector<PoseCand> cand;
  for (int i = 0; i < 2254; ++i) {
    const float p = sigmoidf(std::min(100.0f, std::max(-100.0f, sc->f32()[i])));
    if (p < 0.5f) continue;
    const float* r = raw->f32() + size_t(i) * 12;
    PoseCand c;
    c.score = p;
    c.box[0] = (r[0] / 224 + an.x[size_t(i)]) * float(s) - float(ox);
    c.box[1] = (r[1] / 224 + an.y[size_t(i)]) * float(s) - float(oy);
    c.box[2] = r[2] / 224 * float(s);
    c.box[3] = r[3] / 224 * float(s);
    for (int k = 0; k < 2; ++k) {
      c.kp[k][0] = (r[4 + 2 * k] / 224 + an.x[size_t(i)]) * float(s) - float(ox);
      c.kp[k][1] = (r[5 + 2 * k] / 224 + an.y[size_t(i)]) * float(s) - float(oy);
    }
    cand.push_back(c);
  }
  // weighted NMS (IoU 0.3)
  std::stable_sort(cand.begin(), cand.end(), [](const PoseCand& a, const PoseCand& b) { return a.score > b.score; });
  std::vector<PoseCand> dets;
  while (!cand.empty() && int(dets.size()) < 2 * std::max(1, maxPoses)) {
    const PoseCand best = cand[0];
    std::vector<PoseCand> grp, rest;
    for (const PoseCand& c : cand) (iouBox(best.box, c.box) > 0.3 ? grp : rest).push_back(c);
    double wsum = 0, kp[2][2] = {};
    for (auto& c : grp) wsum += c.score;
    for (auto& c : grp)
      for (int k = 0; k < 2; ++k) for (int j = 0; j < 2; ++j) kp[k][j] += double(c.kp[k][j]) * c.score;
    PoseCand m = best;
    for (int k = 0; k < 2; ++k) for (int j = 0; j < 2; ++j) m.kp[k][j] = float(kp[k][j] / wsum);
    dets.push_back(m);
    cand = std::move(rest);
  }
  const float cxImg = 0.5f * float(im.w), cyImg = 0.5f * float(im.h);
  struct Ranked {
    Pose p;
    float rank;
  };
  std::vector<Ranked> ranked;
  for (const PoseCand& d : dets) {
    if (o.cancel && o.cancel->load()) return {Code::Cancelled, "cancelled"};
    const double x0 = d.kp[0][0], y0 = d.kp[0][1], dx = double(d.kp[1][0]) - x0, dy = double(d.kp[1][1]) - y0;
    const double L = std::sqrt(dx * dx + dy * dy);
    if (L < 8) continue;  // a detection too small to crop usefully
    const double ux = dx / L, uy = dy / L, size = 2 * L * 1.25, th = std::atan2(ux, -uy);
    const double c = std::cos(th), sn = std::sin(th), k = size / 256;
    const double a[6] = {c * k, -sn * k, x0 - 128 * k * c + 128 * k * sn, sn * k, c * k, y0 - 128 * k * sn - 128 * k * c};
    const img::Rgb crop = img::affineBilinear(im, a, 256, 256);
    std::vector<float> cin = toNhwc(crop, 255.0f, 0, 1, false);
    std::vector<Tensor> lr;
    if (auto st = run(modelFor("pose_landmarks"), {view(DType::F32, {1, 256, 256, 3}, cin.data())}, lr, ro(o)); !st) return st;
    const Tensor* lm = nullptr;
    const Tensor* fl = nullptr;
    for (const Tensor& t : lr) {
      if (t.count() == 195 && !lm) lm = &t;
      else if (t.count() == 1 && !fl) fl = &t;
    }
    if (!lm || !fl) return {Code::RunFailed, "pose_landmarks: unexpected outputs"};
    Pose p;
    p.score = d.score;
    p.flag = fl->f32()[0];
    if (!(p.flag > 0.5f)) continue;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < 33; ++i) {
      const double u = lm->f32()[i * 5], v = lm->f32()[i * 5 + 1];
      p.xy[i][0] = float(a[0] * u + a[1] * v + a[2]);
      p.xy[i][1] = float(a[3] * u + a[4] * v + a[5]);
      p.vis[i] = sigmoidf(std::min(100.0f, std::max(-100.0f, lm->f32()[i * 5 + 3]))) * sigmoidf(std::min(100.0f, std::max(-100.0f, lm->f32()[i * 5 + 4])));
      if (p.vis[i] >= 0.5f) {
        bx0 = std::min(bx0, p.xy[i][0]), bx1 = std::max(bx1, p.xy[i][0]);
        by0 = std::min(by0, p.xy[i][1]), by1 = std::max(by1, p.xy[i][1]);
      }
    }
    if (bx1 < bx0) continue;  // nothing visible
    p.box[0] = bx0, p.box[1] = by0, p.box[2] = bx1, p.box[3] = by1;
    // main person: the largest extent, discounted by how far its centre is from the picture's centre (0 at the centre .. -50 % at a corner)
    const float area = (bx1 - bx0) * (by1 - by0);
    const float dxc = (0.5f * (bx0 + bx1) - cxImg) / float(im.w), dyc = (0.5f * (by0 + by1) - cyImg) / float(im.h);
    const float offc = std::min(1.0f, std::sqrt(dxc * dxc + dyc * dyc) / 0.7071f);
    ranked.push_back({p, area * (1.0f - 0.5f * offc)});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.rank > b.rank; });
  for (const Ranked& r : ranked) {
    if (int(out.size()) >= maxPoses) break;
    out.push_back(r.p);
  }
  return {};
}

// ---- person parts --------------------------------------------------------------------------------------------------------------------
namespace {
struct GateFeatures {
  float blob = 0, face = 0, hair = 0, body = 0, person = 0;
  bool ok = false;
};
// 4-connected largest blob of person classes (hair, body skin, face skin, clothes) and the gate thresholds (same numbers as ai_ref.GATE)
GateFeatures gateRegion(const uint8_t* cls, int n) {
  constexpr int W = PersonParts::kSize;
  std::vector<int32_t> lab(size_t(n), 0);
  std::vector<int> stack;
  int best = 0, bestLabel = 0, next = 0, personPx = 0;
  for (int i = 0; i < n; ++i) personPx += (cls[i] >= 1 && cls[i] <= 4);
  for (int i = 0; i < n; ++i) {
    if (lab[size_t(i)] || cls[i] < 1 || cls[i] > 4) continue;
    ++next;
    int area = 0;
    stack.push_back(i);
    lab[size_t(i)] = next;
    while (!stack.empty()) {
      const int p = stack.back();
      stack.pop_back();
      ++area;
      const int x = p % W, y = p / W;
      const int nb[4] = {x > 0 ? p - 1 : -1, x < W - 1 ? p + 1 : -1, y > 0 ? p - W : -1, y < W - 1 ? p + W : -1};
      for (int q : nb)
        if (q >= 0 && !lab[size_t(q)] && cls[q] >= 1 && cls[q] <= 4) { lab[size_t(q)] = next; stack.push_back(q); }
    }
    if (area > best) { best = area; bestLabel = next; }
  }
  GateFeatures f;
  f.person = float(personPx) / float(n);
  f.blob = float(best) / float(n);
  int face = 0, hair = 0, body = 0;
  if (bestLabel)
    for (int i = 0; i < n; ++i)
      if (lab[size_t(i)] == bestLabel) {
        face += cls[i] == kFaceSkin;
        hair += cls[i] == kHair;
        body += (cls[i] == kBodySkin || cls[i] == kClothes);
      }
  f.face = float(face) / float(n);
  f.hair = float(hair) / float(n);
  f.body = float(body) / float(n);
  f.ok = f.blob >= 0.03f && f.hair >= 0.004f && (f.face >= 0.004f || f.body >= 0.01f);
  return f;
}
}  // namespace

Status segmentPerson(const Image& im, PersonParts& out, const TaskOptions& o) {
  out = PersonParts{};
  if (auto s = checkImage(im); !s) return s;
  constexpr int S = PersonParts::kSize, N = S * S;
  const img::Rgb small = img::resizeBilinear(im, S, S);
  std::vector<float> in = toNhwc(small, 255.0f, 0, 1, false);
  std::vector<Tensor> res;
  Status st = run(modelFor("person_parts"), {view(DType::F32, {1, S, S, 3}, in.data())}, res, ro(o));
  if (!st) return st;
  if (res.empty() || res[0].count() != int64_t(N) * 6) return {Code::RunFailed, "person_parts: unexpected output"};
  const float* p = res[0].f32();
  out.prob.assign(size_t(N) * 6, 0);
  std::vector<uint8_t> cls;
  cls.resize(size_t(N));
  for (int i = 0; i < N; ++i) {
    int bc = 0;
    for (int c = 0; c < 6; ++c) {
      if (p[i * 6 + c] > p[i * 6 + bc]) bc = c;
      out.prob[size_t(c) * N + size_t(i)] = uint8_t(std::min(255.0f, std::max(0.0f, std::nearbyint(p[i * 6 + c] * 255.0f))));
    }
    cls[size_t(i)] = uint8_t(bc);
  }
  const GateFeatures g = gateRegion(cls.data(), N);
  out.personArea = g.person;
  out.blobArea = g.blob;
  // validated faces: detector + mesh face flag (a fruit, a profile or a statue does not pass the mesh)
  std::vector<Face> faces;
  if (auto s = detectFaces(im, faces, 0.5f, o); !s) return s;
  for (const Face& f : faces) {
    Landmarks lm;
    if (auto s = faceLandmarks(im, f, lm, o); !s) return s;
    out.faces += lm.valid;
  }
  if (out.faces > 0) { out.trusted = true; out.gateReason = "face"; }
  else if (g.ok) { out.trusted = true; out.gateReason = "region"; }
  else { out.trusted = false; out.gateReason = "none"; }
  out.classMap = out.trusted ? cls : std::vector<uint8_t>(size_t(N), 0);
  return {};
}

// ---- tap select -------------------------------------------------------------------------------------------------------------------------
namespace {
class MagicTouchSession final : public TapSession {
 public:
  Status init(const Image& im, const TaskOptions& o) {
    w_ = im.w;
    h_ = im.h;
    const img::Rgb r = img::resizeBilinear(im, 768, 768);
    std::vector<float> in = toNhwc(r, 255.0f, 0, 1, false);
    return run(modelFor("tap_encoder"), {view(DType::F32, {1, 768, 768, 3}, in.data())}, feats_, ro(o));
  }
  TapEngine engine() const override { return TapEngine::MagicTouch; }
  Status tap(float x, float y, Result& out, bool positive, const TaskOptions& o) override {
    (void)positive;  // the shipped decoder was only verified with a positive click (channel 0); negative clicks need MobileSam
    std::vector<float> v(size_t(768) * 768 * 3, 0.0f);
    const double tx = double(x) / w_ * 768, ty = double(y) / h_ * 768;
    for (int yy = 0; yy < 768; ++yy)
      for (int xx = 0; xx < 768; ++xx) {
        const double dx = xx - tx, dy = yy - ty;
        if (dx * dx + dy * dy < 12.0 * 12.0) v[(size_t(yy) * 768 + size_t(xx)) * 3] = 1.0f;
      }
    std::vector<TensorView> in;
    in.push_back(view(DType::F32, {1, 768, 768, 3}, v.data()));
    for (const Tensor& t : feats_) in.push_back(view(DType::F32, t.dims, t.bytes.data()));
    std::vector<Tensor> res;
    Status st = run(modelFor("tap_decoder"), in, res, ro(o));
    if (!st) return st;
    if (res.empty() || res[0].count() != 768 * 768) return {Code::RunFailed, "tap_decoder: unexpected output"};
    out = Result{};
    out.w = out.h = 768;
    out.validW = out.validH = 768;
    out.mask.assign(res[0].f32(), res[0].f32() + 768 * 768);
    return {};
  }

 private:
  int w_ = 0, h_ = 0;
  std::vector<Tensor> feats_;
};

class MobileSamSession final : public TapSession {
 public:
  Status init(const Image& im, const TaskOptions& o) {
    w_ = im.w;
    h_ = im.h;
    const double s = 1024.0 / std::max(im.w, im.h);
    lw_ = int(std::nearbyint(im.w * s));  // python round(): half to even, like nearbyint in the default rounding mode
    lh_ = int(std::nearbyint(im.h * s));
    const img::Rgb r = img::resizeBilinear(im, lw_, lh_);
    std::vector<float> in(r.px.begin(), r.px.end());
    std::vector<Tensor> res;
    Status st = run(modelFor("sam_encoder"), {view(DType::F32, {lh_, lw_, 3}, in.data())}, res, ro(o));
    if (!st) return st;
    if (res.empty()) return {Code::RunFailed, "sam_encoder: no output"};
    emb_ = std::move(res[0]);
    return {};
  }
  TapEngine engine() const override { return TapEngine::MobileSam; }
  Status tap(float x, float y, Result& out, bool positive, const TaskOptions& o) override {
    Model& dec = modelFor("sam_decoder");
    std::vector<TensorSpec> names;
    if (auto s = dec.inputs(names); !s) return s;
    const float pc[4] = {float(double(x) / w_ * lw_), float(double(y) / h_ * lh_), 0.0f, 0.0f};
    const float pl[2] = {positive ? 1.0f : 0.0f, -1.0f};
    std::vector<float> maskIn(256 * 256, 0.0f);
    const float has = 0.0f;
    const float orig[2] = {lh_ / 4.0f, lw_ / 4.0f};
    std::vector<TensorView> in;
    for (const TensorSpec& s : names) {
      if (s.name == "image_embeddings") in.push_back(view(DType::F32, emb_.dims, emb_.bytes.data()));
      else if (s.name == "point_coords") in.push_back(view(DType::F32, {1, 2, 2}, pc));
      else if (s.name == "point_labels") in.push_back(view(DType::F32, {1, 2}, pl));
      else if (s.name == "mask_input") in.push_back(view(DType::F32, {1, 1, 256, 256}, maskIn.data()));
      else if (s.name == "has_mask_input") in.push_back(view(DType::F32, {1}, &has));
      else if (s.name == "orig_im_size") in.push_back(view(DType::F32, {2}, orig));
      else return {Code::BadInput, "sam_decoder: unknown input " + s.name};
    }
    RunOptions r = ro(o);
    r.outputs = {1, 2};  // iou predictions and the 256x256 low-res logits only: skips the 1024-wide upsampled masks
    std::vector<Tensor> res;
    Status st = run(dec, in, res, r);
    if (!st) return st;
    if (res.size() < 2 || res[1].count() != 256 * 256) return {Code::RunFailed, "sam_decoder: unexpected output"};
    out = Result{};
    out.w = out.h = 256;
    out.validW = int(std::nearbyint(lw_ / 4.0));
    out.validH = int(std::nearbyint(lh_ / 4.0));
    out.iou = res[0].f32()[0];
    out.mask.resize(256 * 256);
    for (int i = 0; i < 256 * 256; ++i) out.mask[size_t(i)] = sigmoidf(res[1].f32()[i]);
    return {};
  }

 private:
  int w_ = 0, h_ = 0, lw_ = 0, lh_ = 0;
  Tensor emb_;
};
}  // namespace

Status prepareTap(const Image& im, std::unique_ptr<TapSession>& out, TapEngine engine, const TaskOptions& o) {
  out.reset();
  if (auto s = checkImage(im); !s) return s;
  if (engine == TapEngine::MagicTouch) {
    auto p = std::make_unique<MagicTouchSession>();
    if (auto s = p->init(im, o); !s) return s;
    out = std::move(p);
  } else {
    auto p = std::make_unique<MobileSamSession>();
    if (auto s = p->init(im, o); !s) return s;
    out = std::move(p);
  }
  return {};
}

// ---- depth ---------------------------------------------------------------------------------------------------------------------------------
Status depth(const Image& im, Depth& out, const TaskOptions& o) {
  out = Depth{};
  if (auto s = checkImage(im); !s) return s;
  const img::Rgb r = img::resizeBilinear(im, 518, 518);
  static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
  std::vector<float> in(size_t(3) * 518 * 518);
  for (int y = 0; y < 518; ++y)
    for (int x = 0; x < 518; ++x)
      for (int c = 0; c < 3; ++c) in[(size_t(c) * 518 + size_t(y)) * 518 + size_t(x)] = (float(r.px[(size_t(y) * 518 + size_t(x)) * 3 + size_t(c)]) / 255.0f - mean[c]) / stdv[c];
  std::vector<Tensor> res;
  Status st = run(modelFor("depth"), {view(DType::F32, {1, 3, 518, 518}, in.data())}, res, ro(o));
  if (!st) return st;
  if (res.empty() || res[0].count() != 518 * 518) return {Code::RunFailed, "depth: unexpected output"};
  out.w = out.h = 518;
  out.d.assign(res[0].f32(), res[0].f32() + 518 * 518);
  out.lo = *std::min_element(out.d.begin(), out.d.end());
  out.hi = *std::max_element(out.d.begin(), out.d.end());
  return {};
}

}  // namespace dapple::ai
