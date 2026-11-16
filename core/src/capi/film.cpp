#include "session.h"

namespace {

// Computes (and stores in the session's key list) the film match of `params`. The caller holds s->matchMtx (filmCache and the proxy belong to it).
FilmExposure computeFilmMatch(DappleSession* s, const EditParams& params, const std::string& lutFile, const std::string& key, bool interactive, bool uncached, float warmPrintEv = std::nanf("")) {
  if (!s->keyProxy) {
    auto kp = std::make_shared<SourceImage>();
    kp->sceneReferred = s->source.sceneReferred;
    kp->linear = resizeToLongEdge(s->source.linear, 256);
    s->keyProxy = std::move(kp);
  }
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) lut = loadLut(lutFile);
  s->filmCache.proxyOwner = s->keyProxy;
  s->filmCache.lutOwner = lut;
  struct HintScope {  // warm start of the print-pad solve: the previous landed print EV of this stock (interactive background solves only)
    explicit HintScope(float v) { dapple::filmPrintStartHint() = v; }
    ~HintScope() { dapple::filmPrintStartHint() = std::nanf(""); }
  } hintScope(warmPrintEv);
  const FilmExposure ev = uncached ? matchFilmExposure(params, *s->camera, lut.get(), *s->keyProxy)
                                   : matchFilmExposureCached(params, *s->camera, lut.get(), lutFile, *s->keyProxy, s->filmCache, !interactive);
  const bool warmed = std::isfinite(warmPrintEv) && params.filmPrint != 0.f;  // a warm-started print solve is within its tolerance of the cold one, not equal: settled frames solve cold
  const bool exact = uncached || (!s->filmCache.lastApprox && !warmed);
  std::lock_guard<std::mutex> lock(s->keyMtx);
  for (auto it = s->filmKeys.begin(); it != s->filmKeys.end(); ++it)
    if (it->key == key) {
      s->filmKeys.erase(it);
      break;
    }
  if (s->filmKeys.size() >= 12) s->filmKeys.erase(s->filmKeys.begin());
  s->filmKeys.push_back({key, ev, exact});
  s->lastMatchStock = params.filmStock;
  s->stockLast[params.filmStock] = ev;
  return ev;
}

// Latest-wins background refresh of the match while a slider moves (wave 8): an interactive frame whose match is not cached uses the previous result
// of the same stock and queues the recompute; the repaint listeners are told when it lands. The settled frame is not interactive and computes the
// exact match itself (it waits for a running refresh at most one match long).
void queueFilmMatch(DappleSession* s, const EditParams& params, const std::string& lutFile, const std::string& key) {
  {
    std::lock_guard<std::mutex> lock(s->asyncMtx);
    s->asyncParams = params;
    s->asyncLut = lutFile;
    s->asyncKey = key;
    s->asyncHave = true;
    if (s->asyncRunning) return;
    s->asyncRunning = true;
  }
  s->matchBusy.fetch_add(1);
  try {
  std::thread([s] {
    dapple::ThreadPool::tlUse() = &dapple::ThreadPool::background();
    sys::utilityQos();
    for (;;) {
      EditParams p;
      std::string lf, k;
      {
        std::lock_guard<std::mutex> lock(s->asyncMtx);
        if (!s->asyncHave) {
          s->asyncRunning = false;
          break;
        }
        p = s->asyncParams;
        lf = s->asyncLut;
        k = s->asyncKey;
        s->asyncHave = false;
      }
      try {
        dapple::ftrace::Busy traceBusy(dapple::ftrace::S().bgMatch);
        std::lock_guard<std::mutex> ml(s->matchMtx);
        bool hit = false;
        {
          std::lock_guard<std::mutex> kl(s->keyMtx);
          for (auto& e : s->filmKeys) hit |= e.key == k;
        }
        float warm = std::nanf("");
        {
          std::lock_guard<std::mutex> kl(s->keyMtx);
          auto it = s->stockLast.find(p.filmStock);
          if (it != s->stockLast.end()) warm = it->second.printEv;
        }
        if (!hit) computeFilmMatch(s, p, lf, k, true, false, warm);
      } catch (...) {
      }
      bool more;
      {
        std::lock_guard<std::mutex> lock(s->asyncMtx);
        more = s->asyncHave;
      }
      if (!more) dapple::engine::filmnode::notifyRepaintIfMatchStale();
    }
    s->matchBusy.fetch_sub(1);
  }).detach();
  } catch (...) {  // no thread: undo the books, or the session destructor would wait for a worker that never ran
    {
      std::lock_guard<std::mutex> lock(s->asyncMtx);
      s->asyncRunning = false;
    }
    s->matchBusy.fetch_sub(1);
  }
}

}  // namespace

namespace capi {

namespace {
double (*g_filmClock)() = nullptr;
constexpr double kFilmEaseSeconds = 0.08;  // time constant of the interactive match easing
constexpr float kFilmEaseSnap = 0.004f;    // closer than this (EV) to the target the used value is the target
double filmNowSeconds() {
  if (g_filmClock) return g_filmClock();
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void setFilmClockForTest(double (*fn)()) { g_filmClock = fn; }

EditParams withFilmKey(DappleSession* s, EditParams params, const char* lutPath, bool interactive) {
  params.filmKeyEv = 0.f;
  params.filmPrintEv = 0.f;
  params.filmSceneX = s->source.meta.asShotIlluminantXy[0];  // film white balance: the scene's as-shot illuminant (0 = unknown)
  params.filmSceneY = s->source.meta.asShotIlluminantXy[1];
  if (params.filmStock.empty() || isModernId(params.filmStock) || std::getenv("DAPPLE_FILM_MATCH_OFF")) return params;
  const std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  // DAPPLE_FILM_MATCH_UNCACHED=1: the match as before section 26 (full key, every solve from scratch, no calibration warm-up): the A/B switch of the drag bench
  const char* ue = std::getenv("DAPPLE_FILM_MATCH_UNCACHED");
  const bool uncached = ue && ue[0];
  const std::string key = (uncached ? filmMatchKey(params) : filmMatchKeyLean(params)) + "|" + lutFile + "|" + std::to_string(params.filmSceneX) + "," + std::to_string(params.filmSceneY);
  if (!uncached) prewarmFilmCalibration(params, [] { sys::setBackground(true); });
  static const bool asyncOn = [] {
    const char* e = std::getenv("DAPPLE_FILM_MATCH_ASYNC");
    return !(e && e[0] == '0') && !dapple::deterministicMode();
  }();
  bool repaintSoon = false;
  {
    std::lock_guard<std::mutex> lock(s->keyMtx);
    // Interactive frames (async match on): the match values a frame uses ease toward their target instead of stepping when a background solve lands
    // (section 57). `target` = the exact cached value (hits too) or the newest landed match of this stock; `direct` = take it as it is.
    static const bool easeOff = [] {  // DAPPLE_FILM_EASE=0: the match values step as before (A/B switch, the test)
      const char* e = std::getenv("DAPPLE_FILM_EASE");
      return e && e[0] == '0';
    }();
    auto easeTo = [&](const FilmExposure& target, bool direct, int kind) {
      const double now = filmNowSeconds();
      auto& u = s->filmUsed;
      const bool sameStock = u.valid && u.stock == params.filmStock;
      const double dt = now - u.t;
      float ev = target.filmEv, pev = target.printEv;
      if (u.valid && !direct && !easeOff && dt <= 0.5 && (sameStock || !s->stockLast.count(params.filmStock))) {
        const float a = float(1.0 - std::exp(-std::max(dt, 0.0) / kFilmEaseSeconds));
        ev = u.ev + (target.filmEv - u.ev) * a;
        pev = u.pev + (target.printEv - u.pev) * a;
        if (std::fabs(ev - target.filmEv) < kFilmEaseSnap && std::fabs(pev - target.printEv) < kFilmEaseSnap) ev = target.filmEv, pev = target.printEv;
      }
      u = {true, params.filmStock, ev, pev, now};
      repaintSoon = ev != target.filmEv || pev != target.printEv;
      params.filmKeyEv = ev;
      params.filmPrintEv = pev;
      dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
        x.matchKind = kind;
        x.filmEv = ev;
        std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
      });
    };
    const bool easing = asyncOn && interactive && !uncached;
    // A result that interpolated remembered medians (a print solve while a slider moves) serves interactive frames only; the frame after the
    // drag solves for real (docs/engine/pipeline-architecture.md section 26).
    for (auto& e : s->filmKeys)
      if (e.key == key && (e.exact || interactive)) {
        const int kind = e.exact ? dapple::ftrace::kMatchHit : dapple::ftrace::kMatchInterp;
        if (easing) {
          easeTo(e.ev, false, kind);
        } else {
          params.filmKeyEv = e.ev.filmEv;
          params.filmPrintEv = e.ev.printEv;
          dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
            x.matchKind = kind;
            x.filmEv = e.ev.filmEv;
            std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
          });
        }
        if (repaintSoon) dapple::engine::filmnode::requestRepaintSoon();
        return params;
      }
    if (easing && !s->filmKeys.empty()) {  // any stock: while another stock is selected the frame shows the previous stock too
      // The newest landed match of this stock now (a stock the map visited before shows its own match at once), the new one on the background pool.
      // A stock without a match: the previous stock's film EV and the neutral print EV 0 (the value of its prebaked coarse table), eased from what the
      // last frame showed; the repaint hook refines after the match lands.
      auto it = s->stockLast.find(params.filmStock);
      const bool known = it != s->stockLast.end();
      FilmExposure target = known ? it->second : FilmExposure{s->filmKeys.back().ev.filmEv, 0.f};
      easeTo(target, known && !(s->filmUsed.valid && s->filmUsed.stock == params.filmStock), dapple::ftrace::kMatchStalePrev);
      dapple::engine::filmnode::markMatchStale();
      queueFilmMatch(s, params, lutFile, key);
      if (repaintSoon) dapple::engine::filmnode::requestRepaintSoon();
      return params;
    }
  }
  std::lock_guard<std::mutex> ml(s->matchMtx);
  double traceMatchMs = 0;
  FilmExposure ev;
  {
    dapple::ftrace::Timer traceTimer(&traceMatchMs);
    ev = computeFilmMatch(s, params, lutFile, key, interactive, uncached);
  }
  dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
    x.matchKind = dapple::ftrace::kMatchComputed;
    x.matchMs += traceMatchMs;
    x.filmEv = ev.filmEv;
    std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
  });
  params.filmKeyEv = ev.filmEv;
  params.filmPrintEv = ev.printEv;
  if (asyncOn && interactive && !uncached) {  // the first frame of a drag solved for itself: the next ones ease from what it showed
    std::lock_guard<std::mutex> lock(s->keyMtx);
    s->filmUsed = {true, params.filmStock, ev.filmEv, ev.printEv, filmNowSeconds()};
  }
  return params;
}

}  // namespace capi

int32_t dapple_film_load_pack(const char* path) {
  return guarded(
      [&]() -> int32_t {
        std::string err;
        if (!path || !film::setLibraryPath(toPath(path), &err)) {
          setError("film pack: " + (err.empty() ? std::string("bad path") : err));
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_film_load_pack_memory(const void* data, size_t size) {
  return guarded(
      [&]() -> int32_t {
        std::string err;
        auto lib = data ? film::FilmLibrary::loadPackMemory(data, size, &err) : nullptr;
        if (!lib) {
          setError("film pack: " + (err.empty() ? std::string("no data") : err));
          return DAPPLE_ERROR;
        }
        film::setLibrary(std::move(lib));
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

void dapple_film_set_pack_path(const char* path) { guardedVoid([&] { film::setLibraryPathLazy(toPath(path)); }); }

void dapple_film_prebake_start(void) { guardedVoid([&] { dapple::engine::filmnode::startFilmPrebake(); }); }
char* dapple_film_prebake_status(void) {
  return guarded(
      [&]() -> char* {
        char b[256];
        dapple::engine::filmnode::filmPrebakeStatus(b, int(sizeof b));
        return dupString(b);
      },
      nullptr);
}

char* dapple_film_list_json(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        std::string j = std::string("{\"loaded\":") + (lib ? "true" : "false") + ",\"stocks\":[";
        if (lib) {
          bool first = true;
          for (const film::StockInfo& s : film::listStocks(*lib)) {
            j += std::string(first ? "" : ",") + "{\"id\":\"" + esc(s.id) + "\",\"name\":\"" + esc(s.name) +
                 "\",\"type\":\"" + s.kind + "\"" + (s.paper.empty() ? std::string() : ",\"paper\":\"" + esc(s.paper) + "\"") + "}";
            first = false;
          }
        }
        return dupString(j + "]}");
      },
      nullptr);
}

char* dapple_film_attribution(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        return dupString(lib ? lib->attribution() : std::string());
      },
      nullptr);
}

// Fixed test patches (linear sRGB scene reflectances: 18 % grey, skin, sky) rendered through every camera stock with the
// default paper; the stock's swatch in the UI. The result is a pure function of the pack: callers cache it.
char* dapple_film_swatches_json(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        std::string j = "{";
        if (lib) {
          static const float kPatch[3][3] = {{0.18f, 0.18f, 0.18f}, {0.50f, 0.30f, 0.22f}, {0.15f, 0.28f, 0.55f}};
          constexpr int kCell = 16;  // pixels per patch edge: the centre pixel is read, away from any edge effect
          SourceImage src;
          src.sceneReferred = true;
          src.linear = ImageF(3 * kCell, kCell);
          for (int y = 0; y < kCell; ++y)
            for (int x = 0; x < 3 * kCell; ++x) color::srgbToProPhoto().apply(kPatch[x / kCell], src.linear.row(y) + 3 * x);
          const CameraProfile& cam = cameras().resolve("", "");
          bool first = true;
          for (const film::StockInfo& si : film::listStocks(*lib)) {
            if (si.kind == "paper") continue;
            EditParams p;
            p.filmStock = si.id;
            p.filmStrength = 100.f;
            ImageF out;
            if (!dapple::engine::renderImageF(p, cam, nullptr, src, false, out) || out.width != 3 * kCell) continue;
            j += std::string(first ? "" : ",") + "\"" + esc(si.id) + "\":[";
            for (int k = 0; k < 3; ++k) {
              const float* px = out.row(kCell / 2) + 3 * (k * kCell + kCell / 2);
              char hex[16];
              std::snprintf(hex, sizeof hex, "%s\"%02x%02x%02x\"", k ? "," : "",
                            int(std::lround(std::clamp(px[0], 0.f, 1.f) * 255.f)),
                            int(std::lround(std::clamp(px[1], 0.f, 1.f) * 255.f)),
                            int(std::lround(std::clamp(px[2], 0.f, 1.f) * 255.f)));
              j += hex;
            }
            j += "]";
            first = false;
          }
        }
        return dupString(j + "}");
      },
      nullptr);
}

char* dapple_looks_json(void) { return guarded([&]() -> char* { return dupString(looksJson()); }, nullptr); }

char* dapple_params_schema_json(void) { return guarded([&]() -> char* { return dupString(paramsSchemaJson()); }, nullptr); }

char* dapple_session_film_pad_json(DappleSession* session, const char* params_json) {
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        if (!ensureSource(session)) return nullptr;
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        const std::string lutFile = params.lutPath;
        FilmPadInfo pad;
        {
          std::lock_guard<std::mutex> lock(session->keyMtx);
          if (!session->keyProxy) {
            session->keyProxy = std::make_shared<SourceImage>();
            session->keyProxy->sceneReferred = session->source.sceneReferred;
            session->keyProxy->linear = resizeToLongEdge(session->source.linear, 256);
          }
          std::shared_ptr<const Lut3D> lut;
          if (!lutFile.empty()) lut = loadLut(lutFile);
          pad = filmPadFor(params, *session->camera, lut.get(), *session->keyProxy);
        }
        char buf[1800];
        if (!pad.ok) return dupString("{\"ok\":false}");
        std::snprintf(buf, sizeof buf,
                      "{\"ok\":true,\"shared_ev\":%.4f,\"final_ev\":%.4f,\"fraction\":%.4f,\"shoot_ev\":[%.4f,%.4f,%.4f],"
                      "\"print_l\":[%.3f,%.3f,%.3f],\"black_l\":[%.3f,%.3f,%.3f],\"latitude\":[%.3f,%.3f,%.3f],\"white_l\":%.3f}",
                      double(pad.sharedEv), double(pad.finalEv), double(pad.fractionAtFinal), double(pad.shootEv[0]), double(pad.shootEv[1]),
                      double(pad.shootEv[2]), double(pad.printL[0]), double(pad.printL[1]), double(pad.printL[2]), double(pad.blackL[0]),
                      double(pad.blackL[1]), double(pad.blackL[2]), double(pad.latitude[0]), double(pad.latitude[1]), double(pad.latitude[2]),
                      double(pad.whiteL));
        std::string js = buf;
        js.pop_back();  // the closing brace: more axes follow
        char b2[1100];
        auto tf = [](bool v) { return v ? "true" : "false"; };
        std::snprintf(b2, sizeof b2,
                      ",\"scene_cct\":%.0f,\"reference_cct\":%.0f,"
                      "\"effective_lo\":{\"film_shoot\":%s,\"film_print\":%s,\"film_black\":%s,\"film_wb\":%s,\"film_latitude\":%s},\"effective_hi\":{\"film_shoot\":%s,\"film_print\":%s,\"film_black\":%s,\"film_wb\":%s,\"film_latitude\":%s}}",
                      double(pad.sceneCct), double(pad.referenceCct), tf(pad.effectiveLo[0]), tf(pad.effectiveLo[1]), tf(pad.effectiveLo[2]),
                      tf(pad.effectiveLo[3]), tf(pad.effectiveLo[4]), tf(pad.effectiveHi[0]), tf(pad.effectiveHi[1]), tf(pad.effectiveHi[2]),
                      tf(pad.effectiveHi[3]), tf(pad.effectiveHi[4]));
        return dupString(js + b2);
      },
      static_cast<char*>(nullptr));
}
