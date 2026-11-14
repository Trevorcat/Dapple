// dapple_ai tests: compare the C++ task wrappers with the Python reference (scripts/ai-eval/ai_ref.py -> build/ai-eval/ai-ref/<image>/).
//   dapple_ai_tests --ref <ai-ref dir> --models <model dir> [--images a,b] [--only pil,face,person,tap,sam,depth,api] [--threads N]
// The model-free checks (pil: resampling bit-exact against PIL) run anywhere; the model checks run on the phone binary (LiteRT + ORT) or on
// the desktop with ORT for the ONNX models (sam, depth).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "ai/tasks.h"
#include "imageops.h"

using namespace dapple::ai;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (cond) { ++g_pass; }                       \
    else { ++g_fail; std::printf("  FAIL %s: ", #cond); std::printf(__VA_ARGS__); std::printf("\n"); } \
  } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
template <class T>
static std::vector<T> readT(const std::string& p) {
  auto b = readAll(p);
  std::vector<T> v(b.size() / sizeof(T));
  if (!v.empty()) std::memcpy(v.data(), b.data(), v.size() * sizeof(T));
  return v;
}

struct Ref {
  int w = 0, h = 0, faces = 0, valid = 0;
  struct F { float score, box[4], kp[12], flag; };
  std::vector<F> f;
  int trusted = 0;
  std::string reason;
  float blob = 0, face = 0, hair = 0, body = 0, person = 0;
  float tx = 0, ty = 0, samIou = 0, dlo = 0, dhi = 0;
  int samW = 0, samH = 0;
  bool hasSam = false;
};
static Ref parseRef(const std::string& path) {
  Ref r;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream s(line);
    std::string k;
    s >> k;
    if (k == "size") s >> r.w >> r.h;
    else if (k == "faces") { std::string v; s >> r.faces >> v >> r.valid; }
    else if (k == "face") {
      Ref::F f;
      s >> f.score;
      for (float& b : f.box) s >> b;
      for (float& b : f.kp) s >> b;
      std::string fl;
      s >> fl >> f.flag;
      r.f.push_back(f);
    } else if (k == "person") {
      std::string t;
      s >> r.trusted >> r.reason >> t >> r.blob >> t >> r.face >> t >> r.hair >> t >> r.body >> t >> r.person;
    } else if (k == "tap") s >> r.tx >> r.ty;
    else if (k == "sam") { s >> r.samW >> r.samH >> r.samIou; r.hasSam = true; }
    else if (k == "depth") s >> r.dlo >> r.dhi;
  }
  return r;
}

struct Case {
  std::string name, dir;
  Ref ref;
  std::vector<uint8_t> rgb;
  Image im;
};
static bool loadCase(const std::string& root, const std::string& name, Case& c) {
  c.name = name;
  c.dir = root + "/" + name + "/";
  auto b = readAll(c.dir + "image.rgb");
  if (b.size() < 8) return false;
  uint32_t w, h;
  std::memcpy(&w, b.data(), 4);
  std::memcpy(&h, b.data() + 4, 4);
  c.rgb.assign(b.begin() + 8, b.end());
  c.im = Image{c.rgb.data(), int(w), int(h), int(w) * 3};
  c.ref = parseRef(c.dir + "ref.txt");
  return c.rgb.size() == size_t(w) * h * 3;
}

static double iouMask(const std::vector<float>& a, const std::vector<float>& b) {
  double i = 0, u = 0;
  for (size_t k = 0; k < a.size(); ++k) {
    const bool x = a[k] > 0.5f, y = b[k] > 0.5f;
    i += x && y;
    u += x || y;
  }
  return u > 0 ? i / u : 1.0;
}
static double maxAbs(const float* a, const float* b, size_t n) {
  double m = 0;
  for (size_t k = 0; k < n; ++k) m = std::max(m, double(std::fabs(a[k] - b[k])));
  return m;
}

static void testPil(const Case& c) {
  auto ref = readAll(c.dir + "pil_resize256.rgb");
  if (ref.empty()) return;
  const img::Rgb r = img::resizeBilinear(c.im, 256, 256);
  size_t diff = 0;
  int mx = 0;
  for (size_t i = 0; i < ref.size() && i < r.px.size(); ++i) { diff += ref[i] != r.px[i]; mx = std::max(mx, std::abs(int(ref[i]) - int(r.px[i]))); }
  std::printf("  pil resize256: %zu of %zu bytes differ (max %d)\n", diff, ref.size(), mx);
  CHECK(diff == 0, "resize is not bit-exact");
  auto ref518 = readAll(c.dir + "pil_resize518.rgb");
  if (!ref518.empty()) {
    const img::Rgb r5 = img::resizeBilinear(c.im, 518, 518);
    size_t d5 = 0;
    for (size_t i = 0; i < ref518.size(); ++i) d5 += ref518[i] != r5.px[i];
    std::printf("  pil resize518: %zu of %zu bytes differ\n", d5, ref518.size());
    CHECK(d5 == 0, "resize518 is not bit-exact");
  }
  auto aff = readAll(c.dir + "pil_affine.rgb");
  auto lmv = readT<float>(c.dir + "landmarks.f32");
  if (!aff.empty() && lmv.size() >= 478 * 3 + 7) {
    double a[6];
    for (int i = 0; i < 6; ++i) a[i] = lmv[478 * 3 + size_t(i)];  // the crop affine was written as float32: rounding makes tiny differences
    const img::Rgb t = img::affineBilinear(c.im, a, 256, 256);
    size_t d2 = 0;
    int m2 = 0;
    for (size_t i = 0; i < aff.size(); ++i) { d2 += aff[i] != t.px[i]; m2 = std::max(m2, std::abs(int(aff[i]) - int(t.px[i]))); }
    std::printf("  pil affine: %zu of %zu bytes differ (max %d; float32 coefficients)\n", d2, aff.size(), m2);
    CHECK(m2 <= 2 && double(d2) < 0.01 * double(aff.size()), "affine crop differs from PIL");
  }
}

static void testFace(const Case& c, const TaskOptions& o) {
  std::vector<Face> f;
  Status st = detectFaces(c.im, f, 0.5f, o);
  CHECK(st.ok(), "detectFaces: %s", st.message.c_str());
  if (!st) return;
  std::printf("  faces: %zu (ref %d)\n", f.size(), c.ref.faces);
  CHECK(int(f.size()) == c.ref.faces, "face count");
  double maxd = 0;
  for (size_t i = 0; i < f.size() && i < c.ref.f.size(); ++i) {
    const auto& r = c.ref.f[i];
    maxd = std::max({maxd, double(std::fabs(f[i].cx - r.box[0])), double(std::fabs(f[i].cy - r.box[1])), double(std::fabs(f[i].w - r.box[2])), double(std::fabs(f[i].h - r.box[3])), double(std::fabs(f[i].score - r.score) * 100)});
    for (int k = 0; k < 6; ++k) maxd = std::max({maxd, double(std::fabs(f[i].kp[k][0] - r.kp[2 * k])), double(std::fabs(f[i].kp[k][1] - r.kp[2 * k + 1]))});
    Landmarks lm;
    st = faceLandmarks(c.im, f[i], lm, o);
    CHECK(st.ok(), "faceLandmarks: %s", st.message.c_str());
    if (!st) continue;
    CHECK(lm.valid == (r.flag > 0), "mesh face flag sign %f vs %f", lm.faceFlag, r.flag);
    CHECK(std::fabs(lm.faceFlag - r.flag) < 0.1f, "mesh flag %f vs %f", lm.faceFlag, r.flag);
    if (i == 0) {
      auto ref = readT<float>(c.dir + "landmarks.f32");
      double ml = 0;
      for (int k = 0; k < 478; ++k) ml = std::max({ml, double(std::fabs(lm.xy[k][0] - ref[size_t(k) * 2])), double(std::fabs(lm.xy[k][1] - ref[size_t(k) * 2 + 1])), double(std::fabs(lm.z[k] - ref[478 * 2 + size_t(k)]))});
      std::printf("  landmarks: max abs diff %.4f px, flag %.2f (ref %.2f)\n", ml, lm.faceFlag, r.flag);
      CHECK(ml < 0.3, "landmark diff %.4f px", ml);
    }
  }
  std::printf("  face boxes/keypoints: max diff %.4f px\n", maxd);
  CHECK(maxd < 0.5, "face box diff %.4f", maxd);
}

static void testPerson(const Case& c, const TaskOptions& o) {
  PersonParts p;
  Status st = segmentPerson(c.im, p, o);
  CHECK(st.ok(), "segmentPerson: %s", st.message.c_str());
  if (!st) return;
  auto cls = readAll(c.dir + "person_class.u8"), prob = readAll(c.dir + "person_prob.u8");
  size_t same = 0, N = 256 * 256;
  const bool tr = c.ref.trusted != 0;
  CHECK(p.trusted == tr, "gate %d vs ref %d", p.trusted, tr);
  CHECK(std::string(p.gateReason) == c.ref.reason, "gate reason %s vs %s", p.gateReason, c.ref.reason.c_str());
  if (p.trusted) {
    for (size_t i = 0; i < N; ++i) same += p.classMap[i] == cls[i];
    std::printf("  person: gate=%s faces=%d class map agreement %.4f%%\n", p.gateReason, p.faces, 100.0 * double(same) / double(N));
    CHECK(double(same) / double(N) > 0.999, "class map agreement %.5f", double(same) / double(N));
  } else {
    size_t nz = 0;
    for (size_t i = 0; i < N; ++i) nz += p.classMap[i] != 0;
    std::printf("  person: gate=%s, class map zeroed (%zu non-zero), network said person on %.1f%% of the frame\n", p.gateReason, nz, 100.0 * p.personArea);
    CHECK(nz == 0, "untrusted map must be all background");
  }
  int mx = 0;
  for (size_t i = 0; i < N * 6; ++i) mx = std::max(mx, std::abs(int(p.prob[i]) - int(prob[i])));
  CHECK(mx <= 2, "probability planes differ by %d/255", mx);
  CHECK(std::fabs(p.blobArea - c.ref.blob) < 0.002f, "blob %f vs %f", p.blobArea, c.ref.blob);
}

static void testTap(const Case& c, const TaskOptions& o, bool sam) {
  std::unique_ptr<TapSession> s;
  auto t0 = std::chrono::steady_clock::now();
  Status st = prepareTap(c.im, s, sam ? TapEngine::MobileSam : TapEngine::MagicTouch, o);
  const double enc = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st.ok(), "prepareTap: %s", st.message.c_str());
  if (!st) return;
  TapSession::Result r;
  t0 = std::chrono::steady_clock::now();
  st = s->tap(c.ref.tx, c.ref.ty, r, true, o);
  const double dec = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st.ok(), "tap: %s", st.message.c_str());
  if (!st) return;
  auto ref = readT<float>(c.dir + (sam ? "tap_sam_lowres.f32" : "tap_mt.f32"));
  CHECK(ref.size() == r.mask.size(), "mask size %zu vs %zu", r.mask.size(), ref.size());
  if (ref.size() != r.mask.size()) return;
  const double iou = iouMask(r.mask, ref), md = maxAbs(r.mask.data(), ref.data(), ref.size());
  std::printf("  tap %s: encoder %.0f ms, decoder %.0f ms, IoU vs reference %.5f, max abs diff %.4f%s\n", sam ? "MobileSAM" : "MagicTouch", enc, dec, iou, md, sam ? "" : " (int8 model)");
  CHECK(iou > (sam ? 0.999 : 0.98), "IoU %.5f", iou);
  if (sam) {
    CHECK(md < 1e-3, "max abs %.5f", md);
    CHECK(std::fabs(r.iou - c.ref.samIou) < 1e-3, "predicted iou %f vs %f", r.iou, c.ref.samIou);
  }
}

static void testDepth(const Case& c, const TaskOptions& o) {
  Depth d;
  auto t0 = std::chrono::steady_clock::now();
  Status st = depth(c.im, d, o);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st.ok(), "depth: %s", st.message.c_str());
  if (!st) return;
  auto ref = readT<float>(c.dir + "depth.f32");
  if (const char* dd = std::getenv("AI_TEST_DUMP")) { std::ofstream f(std::string(dd) + "/" + c.name + "_depth.f32", std::ios::binary); f.write(reinterpret_cast<const char*>(d.d.data()), std::streamsize(d.d.size() * 4)); }
  CHECK(ref.size() == d.d.size(), "depth size");
  if (ref.size() != d.d.size()) return;
  const double md = maxAbs(d.d.data(), ref.data(), ref.size()), rng = c.ref.dhi - c.ref.dlo;
  size_t bad = 0, at = 0;
  double sum = 0;
  for (size_t i = 0; i < ref.size(); ++i) {
    const double e = std::fabs(d.d[i] - ref[i]);
    sum += e;
    bad += e > 1e-3;
    if (e >= md) at = i;
  }
  std::printf("  depth: %.0f ms, max abs diff %.2e at (%zu,%zu) (range %.3f, rel %.2e), mean %.2e, %zu px over 1e-3\n", ms, md, at % 518, at / 518, rng, md / rng, sum / double(ref.size()), bad);
  CHECK(md / rng < 1e-3, "depth rel diff %.3e", md / rng);
}

static std::string g_apiDims;  // "1x3x518x518": dims of input 0 when the model has dynamic ones
static void testApi(const std::string& models, const std::string& id, const TaskOptions& o) {
  setModelDir(models);
  auto m = Model::open(id);
  CHECK(!m->loaded(), "lazy: opening must not load");
  std::vector<TensorSpec> in, out;
  CHECK(m->inputs(in).ok() && m->outputs(out).ok(), "specs of %s", id.c_str());
  CHECK(m->loaded() && !in.empty() && !out.empty(), "io %zu/%zu", in.size(), out.size());
  std::printf("  api: %s runtime=%s, %zu inputs (first %s, %zu dims), %zu outputs, resident %zu KB\n", id.c_str(), m->runtime().c_str(), in.size(), in.empty() ? "?" : in[0].name.c_str(), in.empty() ? size_t(0) : in[0].dims.size(), out.size(), residentBytes() / 1024);
  // zero inputs shaped by the specs (a dynamic dimension becomes 1)
  std::vector<std::vector<uint8_t>> bufs;
  std::vector<TensorView> views;
  for (size_t k = 0; k < in.size(); ++k) {
    const auto& sp = in[k];
    TensorView v;
    v.type = sp.type;
    for (auto d : sp.dims) v.dims.push_back(d < 1 ? 1 : d);
    if (k == 0 && !g_apiDims.empty()) {
      v.dims.clear();
      std::stringstream ss(g_apiDims);
      std::string t;
      while (std::getline(ss, t, 'x')) v.dims.push_back(std::atoll(t.c_str()));
    }
    int64_t n = 1;
    for (auto d : v.dims) n *= d;
    bufs.emplace_back(size_t(n) * dtypeSize(sp.type), 0);
    views.push_back(v);
  }
  for (size_t i = 0; i < views.size(); ++i) views[i].data = bufs[i].data();
  Config cfg = config();
  cfg.idleReleaseMs = 0;
  setConfig(cfg);
  CHECK(releaseIdle() >= 1 && !m->loaded() && residentBytes() == 0, "idle release frees the session");
  cfg.idleReleaseMs = 30000;
  setConfig(cfg);
  std::atomic<bool> stop{true};
  std::vector<Tensor> res;
  RunOptions ro;
  ro.cancel = &stop;
  Status st = run(*m, views, res, ro);
  CHECK(st.code == Code::Cancelled, "cancel flag set before the run -> %d", int(st.code));
  stop = false;
  auto t0 = std::chrono::steady_clock::now();
  st = run(*m, views, res, ro);
  const double full = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st.ok() && res.size() == out.size() && !m->runtime().empty(), "run after release reloads the model: %s", st.message.c_str());
  // cancel in the middle of a run: a helper raises the flag after 20% of a full run; the run must stop well before the end
  std::thread th([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(int(full * 0.2))); stop = true; });
  t0 = std::chrono::steady_clock::now();
  st = run(*m, views, res, ro);
  const double cut = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  th.join();
  std::printf("  api: full run %.0f ms (incl. load), cancelled after 20%% -> returned in %.0f ms, code %d\n", full, cut, int(st.code));
  CHECK(st.code == Code::Cancelled && cut < full * 0.8, "mid-run cancel (%.0f ms of %.0f ms)", cut, full);
  stop = false;
  CHECK(run(*m, views, res, ro).ok(), "the model works after a cancelled run");
  auto missing = Model::open("no_such_model");
  CHECK(run(*missing, views, res).code == Code::NoModel, "missing model reported as NoModel");
  auto bad = Model::openMemory("junk", 4, ".tflite");
  CHECK(!bad->load().ok(), "junk model refused");
  (void)o;
}

// Faces found with the person mask guided windows (findFaces). Expected counts (mesh-validated faces) were set by looking at the images:
// p1, p2, p3, p5, p6 one frontal face; p4 strict profile (the mesh model refuses it: face flag -14.7); p7 two women from behind; p8 three girls
// with the faces partly behind hands and masks; p9 three people frontal.
static void testFindFaces(const Case& c, const TaskOptions& o) {
  static const struct { const char* name; int n; } kExpect[] = {
      {"p1_glasses_flag", 1}, {"p2_dark_skin_dreads", 1}, {"p3_curly_glasses_outdoor", 1}, {"p4_profile_bw", 0}, {"p5_headscarf", 1},
      {"p6_asian_woman_grey", 1}, {"p7_backs_no_faces", 0}, {"p8_three_girls_busy_bg", 3}, {"p9_three_people_frontal", 3}};
  PersonParts p;
  Status st = segmentPerson(c.im, p, o);
  CHECK(st.ok(), "segmentPerson: %s", st.message.c_str());
  if (!st) return;
  std::vector<uint8_t> mask(256 * 256, 0);
  if (p.trusted)
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = p.classMap[i] != 0 ? 255 : 0;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<FoundFace> a;
  FaceFindStats fs;
  st = findFaces(c.im, p.trusted ? mask.data() : nullptr, 256, 256, a, o, &fs);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st.ok(), "findFaces: %s", st.message.c_str());
  if (!st) return;
  t0 = std::chrono::steady_clock::now();
  std::vector<FoundFace> b;
  st = findFaces(c.im, nullptr, 0, 0, b, o);
  const double ms0 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  findFaces: %zu faces (whole frame only: %zu), %d detector runs + %d extra meshes, %.0f ms (whole frame only %.0f ms)\n", a.size(), b.size(), fs.passes,
              fs.crops, ms, ms0);
  for (const FoundFace& f : a) std::printf("    face at (%.0f,%.0f) box %.0fx%.0f score %.2f mesh flag %.1f\n", f.face.cx, f.face.cy, f.face.w, f.face.h, f.face.score, f.lm.faceFlag);
  for (const auto& e : kExpect)
    if (c.name == e.name) CHECK(int(a.size()) == e.n, "%s: %zu faces, expected %d", e.name, a.size(), e.n);
  // a photo whose first pass already finds its one face gets exactly the old result, no extra detector run
  if (c.ref.faces == 1 && c.ref.valid == 1 && !a.empty()) {
    CHECK(fs.passes == 1 && fs.crops == 0, "single face photo ran extra detector passes (%d)", fs.passes);
    auto ref = readT<float>(c.dir + "landmarks.f32");
    double ml = 0;
    for (int k = 0; k < 478 && ref.size() >= 478 * 2; ++k) ml = std::max({ml, double(std::fabs(a[0].lm.xy[k][0] - ref[size_t(k) * 2])), double(std::fabs(a[0].lm.xy[k][1] - ref[size_t(k) * 2 + 1]))});
    std::printf("    first face landmarks vs the reference: max %.3f px\n", ml);
    CHECK(ml < 2.0, "single face landmarks moved by %.3f px", ml);
  }
}

int main(int argc, char** argv) {
  std::string apiModel = "face_detector", ref, models, images, only = "pil,face,person,tap,sam,depth,api,findfaces";
  TaskOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--ref") ref = nx();
    else if (a == "--models") models = nx();
    else if (a == "--images") images = nx();
    else if (a == "--only") only = nx();
    else if (a == "--threads") o.threads = std::atoi(nx().c_str());
    else if (a == "--api-model") apiModel = nx();
    else if (a == "--api-dims") g_apiDims = nx();
  }
  if (ref.empty()) { std::printf("usage: dapple_ai_tests --ref <dir> --models <dir> [--images a,b] [--only ...]\n"); return 2; }
  if (!models.empty()) setModelDir(models);
  std::set<std::string> want;
  { std::stringstream s(only); std::string t; while (std::getline(s, t, ',')) want.insert(t); }
  std::vector<std::string> names;
  { std::stringstream s(images); std::string t; while (std::getline(s, t, ',')) if (!t.empty()) names.push_back(t); }
  if (names.empty() && only == "findfaces") names = {"p1_glasses_flag", "p2_dark_skin_dreads", "p3_curly_glasses_outdoor", "p4_profile_bw", "p5_headscarf", "p6_asian_woman_grey", "p7_backs_no_faces", "p8_three_girls_busy_bg", "p9_three_people_frontal"};
  if (names.empty()) names = {"person", "fruit", "p1_glasses_flag", "p4_profile_bw", "p9_three_people_frontal"};
  for (const auto& n : names) {
    Case c;
    if (!loadCase(ref, n, c)) { std::printf("%s: cannot load reference\n", n.c_str()); ++g_fail; continue; }
    std::printf("%s (%dx%d)\n", n.c_str(), c.im.w, c.im.h);
    if (want.count("pil")) testPil(c);
    if (want.count("face")) testFace(c, o);
    if (want.count("findfaces")) testFindFaces(c, o);
    if (want.count("person")) testPerson(c, o);
    if (want.count("tap")) testTap(c, o, false);
    if (want.count("sam")) testTap(c, o, true);
    if (want.count("depth")) testDepth(c, o);
  }
  if (want.count("api") && !models.empty()) { std::printf("api\n"); testApi(models, apiModel, o); }
  std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
