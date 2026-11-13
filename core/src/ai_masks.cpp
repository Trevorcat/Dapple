// AI masks service: jobs on a background thread, disk cache, tap sessions, re-creation of missing planes (see ai_masks.h, docs section 41).
#include "ai_masks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "local_eval.h"
#include "sysperf.h"

namespace dapple::aimask {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Sky: set to false when the prior is not good enough to ship (docs section 41.6): the group then reports "unavailable".
constexpr bool kShipSky = true;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

struct Label {
  const char* key;
  const char* zh;
};
const Label kLabels[] = {{"hair", "头发"},     {"faceskin", "脸部皮肤"}, {"bodyskin", "身体皮肤"}, {"clothes", "衣服"}, {"person", "人物"},
                         {"background", "背景"}, {"depth", "深度"},       {"sky", "天空 (粗略)"},  {"face", "脸"},       {"eyes", "眼睛"},
                         {"iris", "虹膜"},       {"sclera", "眼白"},      {"lips", "嘴唇"},        {"teeth", "牙齿"},     {"brows", "眉毛"}};
std::string labelOf(const std::string& key) {
  std::string k = key;
  if (k.size() > 3 && k[0] == 'f' && k[2] == ':') k = k.substr(3);
  if (k.compare(0, 4, "tap:") == 0) return "选区";
  for (const Label& l : kLabels)
    if (k == l.key) return l.zh;
  return key;
}

struct State {
  std::mutex m;
  std::string modelDir, cacheDir;
  // jobs
  struct Job {
    int id = 0;
    Source src;
    int tasks = 0, maxFaces = kMaxFaces;
    bool force = false;
    std::atomic<bool> cancel{false};
    std::atomic<int> progress1000{0};
    std::string stage, state = "queued";
    Result result;
  };
  std::map<int, std::shared_ptr<Job>> jobs;
  std::deque<std::shared_ptr<Job>> queue;
  std::shared_ptr<Job> running;
  int nextJob = 1;
  std::condition_variable cv, doneCv;
  bool workerStarted = false;
  // tap
  std::mutex tapM;
  struct Tap {
    std::string photo;
    std::unique_ptr<rt::TapHandle> handle;
    int w = 0, h = 0;
    Clock::time_point last = Clock::now();
    std::vector<TapPoint> taps;
    std::vector<TapMask> masks;
  } tap;
};
State& S() {
  static State* s = new State;  // never destroyed: the worker threads outlive static destruction
  return *s;
}

// ---- disk cache ---------------------------------------------------------------------------------------------------------------------------
fs::path groupPath(const std::string& photo, const std::string& file) {
  std::string dir;
  {
    std::lock_guard<std::mutex> l(S().m);
    dir = S().cacheDir;
  }
  if (dir.empty()) return {};
  return fs::path(std::u8string(dir.begin(), dir.end())) / "ai_masks" / photo / file;
}
bool loadSet(const std::string& photo, const std::string& file, PlaneSet& out) {
  const fs::path p = groupPath(photo, file);
  if (p.empty()) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!unpackSet(d.data(), d.size(), out)) return false;
  std::error_code ec;
  fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // LRU touch
  return true;
}
void saveSet(const std::string& photo, const std::string& file, const PlaneSet& s) {
  const fs::path p = groupPath(photo, file);
  if (p.empty()) return;
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const std::vector<std::uint8_t> d = packSet(s);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    if (!f) return;
  }
  fs::rename(tmp, p, ec);
  if (ec) fs::remove(tmp, ec);
}

// Registers the planes of a group in the raster store; appends their infos.
void publish(const std::string& photo, const std::string& group, const PlaneSet& set, bool cached, Result& r) {
  for (const SetEntry& e : set.entries) {
    const std::string id = makeId(photo, e.key);
    local::rasterSet(id, e.plane.w, e.plane.h, e.plane.px.data(), e.plane.w);
    PlaneInfo pi;
    pi.id = id, pi.key = e.key, pi.label = labelOf(e.key), pi.group = group, pi.w = e.plane.w, pi.h = e.plane.h, pi.fromCache = cached;
    if (e.key.size() > 3 && e.key[0] == 'f' && e.key[2] == ':') pi.face = e.key[1] - '0';
    r.planes.push_back(std::move(pi));
  }
}
bool metaFlag(const std::string& meta, const char* key) { return meta.find(std::string("\"") + key + "\":true") != std::string::npos; }

}  // namespace

// ---- configuration --------------------------------------------------------------------------------------------------------------------------
void configure(const std::string& modelDir, const std::string& cacheDir) {
  {
    std::lock_guard<std::mutex> l(S().m);
    S().modelDir = modelDir;
    S().cacheDir = cacheDir;
  }
  rt::configure(modelDir, 30000);
}
std::string cacheDir() {
  std::lock_guard<std::mutex> l(S().m);
  return S().cacheDir;
}
std::string modelDir() {
  std::lock_guard<std::mutex> l(S().m);
  return S().modelDir;
}
std::string availabilityJson() {
  const rt::Avail a = rt::availability(modelDir());
  const bool any = a.person || a.faces || a.depth || a.tap || (kShipSky && rt::compiled());
  std::string j = std::string("{\"compiled\":") + (rt::compiled() ? "true" : "false") + ",\"available\":" + (any ? "true" : "false") +
                  ",\"backends\":\"" + esc(a.backends) + "\",\"tasks\":{\"person\":" + (a.person ? "true" : "false") +
                  ",\"faces\":" + (a.faces ? "true" : "false") + ",\"depth\":" + (a.depth ? "true" : "false") +
                  ",\"tap\":" + (a.tap ? "true" : "false") + ",\"sky\":" + (kShipSky && rt::compiled() ? "true" : "false") + "},\"tap_engine\":\"" + a.tapEngine +
                  "\",\"reason\":\"" + esc(a.reason) + "\",\"resident_bytes\":" + std::to_string(rt::resident()) + "}";
  return j;
}

std::string Result::json() const {
  char b[96];
  std::string j = "{\"state\":\"" + state + "\",\"message\":\"" + esc(message) + "\",\"person_found\":" + (personFound ? "true" : "false");
  std::snprintf(b, sizeof b, ",\"faces\":%d,\"sky_found\":%s,\"render_ms\":%.1f,\"total_ms\":%.1f", faceCount, skyFound ? "true" : "false", renderMs, totalMs);
  j += b;
  j += ",\"groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    std::snprintf(b, sizeof b, ",\"ms\":%.1f}", groups[i].ms);
    j += std::string(i ? "," : "") + "{\"group\":\"" + groups[i].group + "\",\"state\":\"" + groups[i].state + "\",\"message\":\"" + esc(groups[i].message) + "\"" + b;
  }
  j += "],\"planes\":[";
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const PlaneInfo& p = planes[i];
    j += std::string(i ? "," : "") + "{\"id\":\"" + esc(p.id) + "\",\"key\":\"" + esc(p.key) + "\",\"label\":\"" + p.label + "\",\"group\":\"" + p.group +
         "\",\"face\":" + std::to_string(p.face) + ",\"w\":" + std::to_string(p.w) + ",\"h\":" + std::to_string(p.h) +
         ",\"cached\":" + (p.fromCache ? "true" : "false") + "}";
  }
  return j + "]}";
}

bool storeGroup(const std::string& photo, const std::string& group, const PlaneSet& set) {
  if (cacheDir().empty()) return false;
  saveSet(photo, group + ".dpl", set);
  PlaneSet back;
  return loadSet(photo, group + ".dpl", back);
}

// ---- compute ------------------------------------------------------------------------------------------------------------------------------
Result compute(const Source& src, int tasks, int maxFaces, bool force, const Control& ctl) {
  Result r;
  const auto t0 = Clock::now();
  maxFaces = std::clamp(maxFaces, 1, kMaxFaces);
  Rendition rend;
  bool rendered = false, renderFailed = false;
  std::string renderErr;
  auto cancelled = [&] { return ctl.cancel && ctl.cancel->load(); };
  auto prog = [&](float f, const char* stage) {
    if (ctl.progress) ctl.progress(std::clamp(f, 0.f, 1.f), stage);
  };
  auto needRender = [&]() -> bool {
    if (rendered) return true;
    if (renderFailed) return false;
    prog(0.02f, "render");
    const auto t = Clock::now();
    if (!src.render || !src.render(rend, renderErr) || rend.rgb.empty()) {
      renderFailed = true;
      if (renderErr.empty()) renderErr = "no picture";
      return false;
    }
    r.renderMs = msSince(t);
    rendered = true;
    return true;
  };
  auto img = [&] {
    Img i;
    i.rgb = rend.rgb.data(), i.w = rend.w, i.h = rend.h, i.stride = rend.w * 3;
    return i;
  };
  const std::string& ph = src.photoId;
  Plane depthFar;  // for the sky prior, when available
  const rt::Avail av = rt::availability(modelDir());

  struct G {
    int bit;
    const char* name;
    float w0, w1;
  };
  const G groups[] = {{kTaskPerson, "person", 0.10f, 0.40f}, {kTaskFaces, "faces", 0.40f, 0.55f}, {kTaskDepth, "depth", 0.55f, 0.90f}, {kTaskSky, "sky", 0.90f, 1.0f}};
  for (const G& g : groups) {
    if (!(tasks & g.bit)) continue;
    if (cancelled()) {
      r.state = "cancelled";
      break;
    }
    if (ctl.yield) sys::yieldToInteraction();
    GroupStatus gs;
    gs.group = g.name;
    const auto tg = Clock::now();
    prog(g.w0, g.name);
    const std::string file = std::string(g.name) + ".dpl";
    PlaneSet set;
    bool have = false, cachedHit = false;
    if (!force && loadSet(ph, file, set)) have = cachedHit = true;
    const bool sky = g.bit == kTaskSky;
    if (!have) {
      auto unavailable = [&](const std::string& why) {
        gs.state = "unavailable", gs.message = why;
      };
      if (sky && !(kShipSky && rt::compiled())) {
        unavailable(kShipSky ? "AI masks are not part of this build" : "sky prior not shipped");
      } else if (g.bit == kTaskPerson && !av.person) {
        unavailable(av.reason.empty() ? "person model unavailable" : av.reason);
      } else if (g.bit == kTaskFaces && !av.faces) {
        unavailable(av.reason.empty() ? "face models unavailable" : av.reason);
      } else if (g.bit == kTaskDepth && !av.depth) {
        unavailable(av.reason.empty() ? "depth model unavailable" : av.reason);
      } else if (!needRender()) {
        gs.state = "error", gs.message = renderErr;
      } else {
        const Img im = img();
        rt::Res res;
        res.ok = true;
        if (g.bit == kTaskPerson) {
          rt::PersonOut po;
          res = rt::person(im, po, ctl.cancel);
          if (res.ok) {
            char mb[96];
            std::snprintf(mb, sizeof mb, "{\"found\":%s,\"gate\":\"%s\",\"faces\":%d}", po.trusted ? "true" : "false", po.gate.c_str(), po.faces);
            set.meta = mb;
            if (po.trusted) {
              const PartPlanes pp = splitParts(po.prob.data(), 256, rend.w, rend.h);
              set.entries = {{"hair", pp.hair}, {"faceskin", pp.faceSkin}, {"bodyskin", pp.bodySkin}, {"clothes", pp.clothes}, {"person", pp.person}, {"background", pp.background}};
            }
          }
        } else if (g.bit == kTaskFaces) {
          std::vector<FaceMesh> meshes;
          res = rt::faces(im, maxFaces, meshes, ctl.cancel);
          if (res.ok) {
            std::string fj;
            int n = 0;
            for (const FaceMesh& m : meshes) {
              FaceFeatures ff;
              if (!faceFeatures(m, rend.w, rend.h, rend.rgb.data(), rend.w * 3, ff)) continue;
              ++n;
              const std::string p = "f" + std::to_string(n) + ":";
              set.entries.push_back({p + "face", ff.face});
              if (ff.frontal) {
                set.entries.push_back({p + "eyes", ff.eyes});
                set.entries.push_back({p + "iris", ff.iris});
                set.entries.push_back({p + "sclera", ff.sclera});
                set.entries.push_back({p + "lips", ff.lips});
                if (ff.open && !ff.teeth.empty()) set.entries.push_back({p + "teeth", ff.teeth});
                set.entries.push_back({p + "brows", ff.brows});
              }
              char b[120];
              std::snprintf(b, sizeof b, "%s{\"yaw\":%.1f,\"frontal\":%s,\"mouth_open\":%.3f,\"teeth\":%s}", fj.empty() ? "" : ",", ff.yawDeg,
                            ff.frontal ? "true" : "false", ff.mouthOpen, ff.open && !ff.teeth.empty() ? "true" : "false");
              fj += b;
            }
            set.meta = "{\"count\":" + std::to_string(n) + ",\"list\":[" + fj + "]}";
          }
        } else if (g.bit == kTaskDepth) {
          rt::DepthOut d;
          res = rt::depth(im, d, ctl.cancel);
          if (res.ok) {
            Plane p = depthFarPlane(d.d.data(), d.w, d.h, rend.w, rend.h);
            if (p.empty()) res = {false, false, "depth: empty output"};
            else set.entries.push_back({"depth", std::move(p)}), set.meta = "{\"found\":true}";
          }
        } else {  // sky
          if (depthFar.empty() && av.depth) {  // the far part of the depth plane takes part whenever depth is available: cached, else computed here
            PlaneSet ds;
            if (!loadSet(ph, "depth.dpl", ds)) {
              rt::DepthOut d;
              const rt::Res dr = rt::depth(im, d, ctl.cancel);
              if (dr.cancelled) res = dr;
              if (dr.ok) {
                Plane p = depthFarPlane(d.d.data(), d.w, d.h, rend.w, rend.h);
                if (!p.empty()) ds.entries.push_back({"depth", p}), ds.meta = "{\"found\":true}", saveSet(ph, "depth.dpl", ds);
              }
            }
            for (const SetEntry& e : ds.entries) depthFar = e.plane;
          }
          const Plane* far = depthFar.empty() ? nullptr : &depthFar;
          float conf = 0;
          Plane p = skyPrior(rend.rgb.data(), rend.w, rend.h, rend.w * 3, far, &conf);
          char b[80];
          std::snprintf(b, sizeof b, "{\"found\":%s,\"confidence\":%.3f,\"depth\":%s}", p.empty() ? "false" : "true", conf, far ? "true" : "false");
          set.meta = b;
          if (!p.empty()) set.entries.push_back({"sky", std::move(p)});
        }
        if (!res.ok) {
          if (res.cancelled) r.state = "cancelled", gs.state = "cancelled";
          else gs.state = "error", gs.message = res.err;
        } else {
          saveSet(ph, file, set);
          have = true;
        }
      }
    }
    if (have) {
      publish(ph, g.name, set, cachedHit, r);
      gs.state = set.entries.empty() ? "none" : cachedHit ? "cached" : "ok";
      if (g.bit == kTaskPerson) {
        r.personFound = metaFlag(set.meta, "found");
        if (!r.personFound) gs.message = "no person found";
      }
      if (g.bit == kTaskFaces) {
        const auto k = set.meta.find("\"count\":");
        r.faceCount = k == std::string::npos ? 0 : std::atoi(set.meta.c_str() + k + 8);
        if (!r.faceCount) gs.message = "no face mesh found";
      }
      if (sky) {
        r.skyFound = !set.entries.empty();
        if (!r.skyFound) gs.message = "no sky-like region";
      }
      if (g.bit == kTaskDepth)
        for (const SetEntry& e : set.entries) depthFar = e.plane;
    }
    gs.ms = msSince(tg);
    r.groups.push_back(gs);
    if (r.state == "cancelled") break;
    prog(g.w1, g.name);
  }
  // sky needs the depth plane only when depth ran before it in the same call; when depth is cached but not requested the prior runs without it
  // (same on every call that asks for sky alone: deterministic)
  if (r.state == "done") {
    bool anyOk = false, anyErr = false;
    for (const GroupStatus& g : r.groups) anyOk |= g.state == "ok" || g.state == "cached" || g.state == "none", anyErr |= g.state == "error";
    if (anyErr && !anyOk) r.state = "error";
    for (const GroupStatus& g : r.groups)
      if (g.state == "error" && r.message.empty()) r.message = g.group + ": " + g.message;
    if (!anyOk && !anyErr && !r.groups.empty()) {
      r.message = r.groups[0].message;
      r.state = "error";
    }
  }
  r.totalMs = msSince(t0);
  prog(1.f, "done");
  pruneCache(256ull << 20);
  return r;
}

// ---- jobs ---------------------------------------------------------------------------------------------------------------------------------------
namespace {
void startWorker() {
  State& s = S();
  if (s.workerStarted) return;
  s.workerStarted = true;
  std::thread([] {
    sys::setBackground(true);
    sys::utilityQos();
    State& st = S();
    for (;;) {
      std::shared_ptr<State::Job> job;
      {
        std::unique_lock<std::mutex> l(st.m);
        while (st.queue.empty()) {
          if (st.cv.wait_for(l, std::chrono::seconds(5)) == std::cv_status::timeout && st.queue.empty()) {
            l.unlock();
            releaseIdle();  // janitor: model memory returns after the idle time
            l.lock();
          }
        }
        job = st.queue.front();
        st.queue.pop_front();
        if (job->cancel.load()) {
          job->state = "cancelled", job->result.state = "cancelled";
          st.doneCv.notify_all();
          continue;
        }
        job->state = "running";
        st.running = job;
      }
      Control c;
      c.cancel = &job->cancel;
      c.progress = [job](float f, const char* stage) {
        job->progress1000.store(int(f * 1000.f));
        std::lock_guard<std::mutex> l(S().m);
        job->stage = stage;
      };
      Result res;
      if (job->tasks & kTaskTapPrepare) {  // encoder warm-up (a tap does it itself otherwise)
        TapResult tr = tap(job->src, -1.f, -1.f, 4, c);
        res.state = tr.ok ? "done" : "error";
        res.message = tr.message;
      }
      if (job->tasks & ~kTaskTapPrepare) {
        Result r2 = compute(job->src, job->tasks & ~kTaskTapPrepare, job->maxFaces, job->force, c);
        if (res.state == "error" && r2.state == "done") r2.message = res.message;
        res = std::move(r2);
      }
      {
        std::lock_guard<std::mutex> l(st.m);
        job->result = std::move(res);
        job->state = job->result.state;
        job->progress1000.store(1000);
        st.running.reset();
        while (st.jobs.size() > 24) st.jobs.erase(st.jobs.begin());  // oldest ids first
      }
      st.doneCv.notify_all();
    }
  }).detach();
}
}  // namespace

int startJob(const Source& src, int tasks, int maxFaces, bool force) {
  State& s = S();
  auto job = std::make_shared<State::Job>();
  job->src = src, job->tasks = tasks, job->maxFaces = maxFaces, job->force = force;
  std::lock_guard<std::mutex> l(s.m);
  job->id = s.nextJob++;
  s.jobs[job->id] = job;
  s.queue.push_back(job);
  startWorker();
  s.cv.notify_all();
  return job->id;
}
std::string jobJson(int id) {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  auto it = s.jobs.find(id);
  if (it == s.jobs.end()) return "{\"job\":" + std::to_string(id) + ",\"state\":\"unknown\"}";
  const State::Job& j = *it->second;
  char b[64];
  std::snprintf(b, sizeof b, "%.3f", float(j.progress1000.load()) / 1000.f);
  std::string out = "{\"job\":" + std::to_string(id) + ",\"state\":\"" + j.state + "\",\"progress\":" + b + ",\"stage\":\"" + esc(j.stage) + "\"";
  if (j.state != "queued" && j.state != "running") out += ",\"result\":" + j.result.json();
  return out + "}";
}
void cancelJob(int id) {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  auto it = s.jobs.find(id);
  if (it != s.jobs.end()) it->second->cancel.store(true);
}
void cancelOwner(void* owner) {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  for (auto& [id, j] : s.jobs)
    if (j->src.owner == owner) j->cancel.store(true);
}
void waitOwner(void* owner) {
  State& s = S();
  std::unique_lock<std::mutex> l(s.m);
  s.doneCv.wait(l, [&] {
    if (s.running && s.running->src.owner == owner) return false;
    for (auto& j : s.queue)
      if (j->src.owner == owner) return false;
    return true;
  });
}
bool waitJob(int id, int timeoutMs) {
  State& s = S();
  std::unique_lock<std::mutex> l(s.m);
  auto it = s.jobs.find(id);
  if (it == s.jobs.end()) return true;
  auto j = it->second;
  return s.doneCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return j->state != "queued" && j->state != "running"; });
}

// ---- tap select -----------------------------------------------------------------------------------------------------------------------------
namespace {
std::string tapFile(const std::string& spec) {
  std::uint32_t h = 2166136261u;
  for (char c : spec) h = (h ^ std::uint8_t(c)) * 16777619u;
  char b[32];
  std::snprintf(b, sizeof b, "tap-%08x.dpl", h);
  return b;
}
// The mask of one tap at the rendition size.
Plane tapPlane(const rt::TapOut& o, int w, int h) {
  const int vw = o.validW > 0 ? o.validW : o.w, vh = o.validH > 0 ? o.validH : o.h;
  std::vector<float> crop(std::size_t(vw) * vh);
  for (int y = 0; y < vh; ++y)
    for (int x = 0; x < vw; ++x) crop[std::size_t(y) * vw + x] = o.mask[std::size_t(y) * o.w + x];
  return resizePlaneF(crop.data(), vw, vh, w, h, 0.f, 1.f);
}
}  // namespace

TapResult tap(const Source& src, float u, float v, int op, const Control& ctl) {
  TapResult res;
  const auto t0 = Clock::now();
  State& s = S();
  std::lock_guard<std::mutex> tl(s.tapM);
  auto& T = s.tap;
  if (T.photo != src.photoId) {
    T = State::Tap{};
    T.photo = src.photoId;
  }
  T.last = Clock::now();
  const bool prepareOnly = op == 4;
  if (op == 3) {
    T.taps.clear(), T.masks.clear();
    res.ok = true;
    return res;
  }
  if (op == 2) {
    if (!T.taps.empty()) T.taps.pop_back(), T.masks.pop_back();
  } else {
    if (!prepareOnly && int(T.taps.size()) >= kMaxTaps) {
      res.message = "too many taps (10)";
      return res;
    }
    const rt::Avail av = rt::availability(modelDir());
    if (!av.tap) {
      res.message = av.reason.empty() ? "tap select unavailable" : av.reason;
      return res;
    }
    Rendition rend;
    if (!T.handle) {
      std::string err;
      if (!src.render || !src.render(rend, err) || rend.rgb.empty()) {
        res.message = err.empty() ? "no picture" : err;
        return res;
      }
      const auto tp = Clock::now();
      if (ctl.progress) ctl.progress(0.3f, "tap-encoder");
      Img im;
      im.rgb = rend.rgb.data(), im.w = rend.w, im.h = rend.h, im.stride = rend.w * 3;
      rt::Res pr = rt::prepareTap(im, modelDir(), T.handle, ctl.cancel);
      if (!pr.ok) {
        T.handle.reset();
        res.message = pr.cancelled ? "cancelled" : pr.err;
        return res;
      }
      T.w = rend.w, T.h = rend.h;
      res.prepareMs = msSince(tp);
      // masks of earlier taps (after an idle release) are kept: their planes do not depend on the session
    }
    if (prepareOnly) {
      res.ok = true;
      res.w = T.w, res.h = T.h;
      return res;
    }
    rt::TapOut out;
    rt::Res tr = T.handle->tap(std::clamp(u, 0.f, 1.f) * float(T.w), std::clamp(v, 0.f, 1.f) * float(T.h), out, ctl.cancel);
    if (!tr.ok) {
      res.message = tr.cancelled ? "cancelled" : tr.err;
      return res;
    }
    TapMask m;
    m.add = op == 0;
    m.mask = tapPlane(out, T.w, T.h);
    T.taps.push_back({u, v, op == 0});
    T.masks.push_back(std::move(m));
  }
  res.taps = T.taps;
  res.w = T.w, res.h = T.h;
  if (T.taps.empty()) {
    res.ok = true;
    return res;
  }
  const std::string spec = tapSpec(T.taps);
  res.id = makeId(src.photoId, "tap:" + spec);
  Plane acc = accumulateTaps(T.masks, T.w, T.h);
  PlaneSet set;
  set.entries.push_back({"tap:" + spec, acc});
  set.meta = "{\"taps\":" + std::to_string(T.taps.size()) + "}";
  saveSet(src.photoId, tapFile(spec), set);
  Result dummy;
  publish(src.photoId, "tap", set, false, dummy);
  res.ok = true;
  res.ms = msSince(t0);
  return res;
}

// ---- ensure -----------------------------------------------------------------------------------------------------------------------------------
bool isAiId(const std::string& id) { return id.compare(0, 3, "ai:") == 0; }

std::vector<std::string> ensure(const Source& src, const std::vector<std::string>& ids, const Control& ctl, std::string* why) {
  std::vector<std::string> missing;
  int groupsNeeded = 0;
  std::vector<std::string> tapIds;
  auto note = [&](const std::string& w) {
    if (why && why->empty()) *why = w;
  };
  for (const std::string& id : ids) {
    if (local::rasterHas(id)) continue;
    const ParsedId p = parseId(id);
    if (!p.ok) {
      missing.push_back(id);
      note("'" + id + "' is not an AI plane id and was not supplied");
      continue;
    }
    if (p.photo != src.photoId) {
      missing.push_back(id);
      note("'" + id + "' belongs to another photo");
      continue;
    }
    if (p.group == "person") groupsNeeded |= kTaskPerson;
    else if (p.group == "faces") groupsNeeded |= kTaskFaces;
    else if (p.group == "depth") groupsNeeded |= kTaskDepth;
    else if (p.group == "sky") groupsNeeded |= kTaskSky;
    else tapIds.push_back(id);
  }
  if (groupsNeeded) {
    Result r = compute(src, groupsNeeded, kMaxFaces, false, ctl);
    for (const GroupStatus& g : r.groups)
      if (g.state == "error" || g.state == "unavailable") note(g.group + ": " + g.message);
  }
  for (const std::string& id : tapIds) {
    const ParsedId p = parseId(id);
    PlaneSet set;
    if (loadSet(src.photoId, tapFile(tapSpec(p.taps)), set) && !set.entries.empty()) {
      Result dummy;
      publish(src.photoId, "tap", set, true, dummy);
      continue;
    }
    // replay the taps (encoder + decoder): the session's tap state is reset to exactly this list
    tap(src, 0, 0, 3, ctl);
    for (const TapPoint& t : p.taps) {
      TapResult tr = tap(src, t.x, t.y, t.add ? 0 : 1, ctl);
      if (!tr.ok) {
        note("tap: " + tr.message);
        break;
      }
    }
  }
  for (const std::string& id : ids)
    if (!local::rasterHas(id)) {
      missing.push_back(id);
      note("'" + id + "' could not be produced for this photo");
    }
  std::sort(missing.begin(), missing.end());
  missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
  return missing;
}

// ---- housekeeping ---------------------------------------------------------------------------------------------------------------------------------
void releaseIdle() {
  rt::releaseIdle();
  State& s = S();
  std::unique_lock<std::mutex> tl(s.tapM, std::try_to_lock);
  if (tl.owns_lock() && s.tap.handle && msSince(s.tap.last) > 30000) s.tap.handle.reset();
}
void releaseAll() {
  rt::releaseAll();
  State& s = S();
  std::lock_guard<std::mutex> tl(s.tapM);
  s.tap.handle.reset();
}

void pruneCache(std::uint64_t maxBytes) {
  const std::string dir = cacheDir();
  if (dir.empty()) return;
  std::error_code ec;
  const fs::path root = fs::path(std::u8string(dir.begin(), dir.end())) / "ai_masks";
  struct D {
    fs::path p;
    fs::file_time_type t;
    std::uint64_t bytes = 0;
  };
  std::vector<D> v;
  std::uint64_t total = 0;
  for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    D d;
    d.p = it->path();
    for (auto f = fs::directory_iterator(d.p, ec); !ec && f != fs::directory_iterator(); f.increment(ec)) {
      std::error_code e2;
      d.bytes += f->file_size(e2);
      d.t = std::max(d.t, f->last_write_time(e2));
    }
    total += d.bytes;
    v.push_back(std::move(d));
  }
  if (total <= maxBytes) return;
  std::sort(v.begin(), v.end(), [](const D& a, const D& b) { return a.t < b.t; });
  for (const D& d : v) {
    if (total <= maxBytes) break;
    fs::remove_all(d.p, ec);
    total -= std::min(total, d.bytes);
  }
}

}  // namespace dapple::aimask
