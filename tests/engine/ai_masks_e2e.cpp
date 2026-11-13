// AI masks end to end through the C API (desktop with ORT, Android with LiteRT + ORT): timings (cold / warm / cached), memory, plane files and overlay sheets.
//   dapple_ai_e2e <model_dir> <cache_dir> <out_dir> <photo> [--tasks N] [--taps "x,y;x,y;-x,y"] [--maxfaces N] [--name NAME] [--long N] [--bands]
// For every plane the service reports, the sheet shows the photo with the plane (refined through the engine's raster refinement, refine 60, at preview
// resolution via dapple_session_mask_preview) as a red overlay. Files: <out_dir>/<name>_sheet.jpg (tiles in the order of <name>_legend.txt), <name>_log.json.
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "dapple/dapple_c.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static long procKb(const char* key) {
#if defined(__linux__)
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.compare(0, std::strlen(key), key) == 0) return std::atol(line.c_str() + std::strlen(key) + 1);
#else
  (void)key;
#endif
  return -1;
}

struct Plane {
  std::string id, key, label;
};
static std::string field(const std::string& j, std::size_t from, const char* name, std::size_t* endOut = nullptr) {
  const std::string k = std::string("\"") + name + "\":\"";
  const std::size_t a = j.find(k, from);
  if (a == std::string::npos) return "";
  const std::size_t b = j.find('"', a + k.size());
  if (endOut) *endOut = b;
  return j.substr(a + k.size(), b - a - k.size());
}
static std::vector<Plane> parsePlanes(const std::string& j) {
  std::vector<Plane> out;
  std::size_t at = j.find("\"planes\":[");
  while (at != std::string::npos) {
    at = j.find("{\"id\":\"", at);
    if (at == std::string::npos) break;
    Plane p;
    std::size_t e = 0;
    p.id = field(j, at, "id", &e);
    p.key = field(j, e, "key", &e);
    p.label = field(j, e, "label", &e);
    out.push_back(p);
    at = e;
  }
  return out;
}
static std::string str(char* p) {
  std::string s = p ? p : "";
  dapple_free_string(p);
  return s;
}

struct Rgba {
  int w = 0, h = 0;
  std::vector<std::uint8_t> d;
};
static Rgba fromImage(DappleImage& im) {
  Rgba r;
  r.w = im.width, r.h = im.height;
  if (im.rgba) r.d.assign(im.rgba, im.rgba + std::size_t(im.width) * im.height * 4);
  dapple_free_image(&im);
  return r;
}

struct Job {
  std::string json;
  double ms = 0;
};
static Job runJob(DappleSession* s, int tasks, bool force, int maxFaces) {
  Job j;
  const auto t0 = Clock::now();
  const int id = dapple_session_ai_start(s, tasks, force ? 1 : 0, maxFaces);
  if (id <= 0) {
    j.json = std::string("{\"error\":\"") + (dapple_last_error() ? dapple_last_error() : "") + "\"}";
    return j;
  }
  float last = -1;
  for (;;) {
    j.json = str(dapple_ai_job_json(id));
    if (j.json.find("\"state\":\"running\"") == std::string::npos && j.json.find("\"state\":\"queued\"") == std::string::npos) break;
    const std::size_t p = j.json.find("\"progress\":");
    const float f = p == std::string::npos ? 0.f : float(std::atof(j.json.c_str() + p + 11));
    if (f - last > 0.1f) std::fprintf(stderr, "  progress %.0f%% %s\n", f * 100, field(j.json, 0, "stage").c_str()), last = f;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  j.ms = msSince(t0);
  return j;
}

static std::string maskJson(const std::string& id, float lo, float hi, float feather, bool range) {
  char b[512];
  std::snprintf(b, sizeof b,
                "{\"local\":{\"v\":1,\"items\":[{\"id\":\"v\",\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"%s\",\"refine\":60%s}],\"adjust\":{\"exposure\":20}}]}}",
                id.c_str(), range ? (std::string(",\"lo\":") + std::to_string(lo) + ",\"hi\":" + std::to_string(hi) + ",\"feather\":" + std::to_string(feather)).c_str() : "");
  return b;
}

struct Sheet {
  int tw = 0, th = 0, cols = 4;
  std::vector<Rgba> tiles;
  void add(const Rgba& photo, const Rgba& mask) {
    Rgba t;
    t.w = photo.w, t.h = photo.h;
    t.d = photo.d;
    const int w = std::min(photo.w, mask.w), h = std::min(photo.h, mask.h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const float m = mask.d[(std::size_t(y) * mask.w + x) * 4] / 255.f * 0.62f;
        std::uint8_t* p = &t.d[(std::size_t(y) * t.w + x) * 4];
        p[0] = std::uint8_t(p[0] * (1 - m) + 255 * m), p[1] = std::uint8_t(p[1] * (1 - m) + 20 * m), p[2] = std::uint8_t(p[2] * (1 - m) + 40 * m);
      }
    tiles.push_back(std::move(t));
  }
  bool save(const fs::path& path) {
    if (tiles.empty()) return false;
    for (auto& t : tiles) tw = std::max(tw, t.w), th = std::max(th, t.h);
    const int rows = (int(tiles.size()) + cols - 1) / cols, W = cols * (tw + 4), H = rows * (th + 4);
    std::vector<std::uint8_t> out(std::size_t(W) * H * 3, 24);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
      const int ox = int(i % cols) * (tw + 4) + 2, oy = int(i / cols) * (th + 4) + 2;
      for (int y = 0; y < tiles[i].h; ++y)
        for (int x = 0; x < tiles[i].w; ++x)
          for (int c = 0; c < 3; ++c) out[(std::size_t(oy + y) * W + ox + x) * 3 + c] = tiles[i].d[(std::size_t(y) * tiles[i].w + x) * 4 + c];
    }
    return stbi_write_jpg(path.string().c_str(), W, H, 3, out.data(), 88) != 0;
  }
};

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <model_dir> <cache_dir> <out_dir> <photo> [--tasks N] [--taps spec] [--maxfaces N] [--name N] [--long N] [--bands] [--cold-only]\n", argv[0]);
    return 2;
  }
  const std::string models = argv[1], cache = argv[2], outDir = argv[3], photo = argv[4];
  int tasks = DAPPLE_AI_PERSON | DAPPLE_AI_FACES | DAPPLE_AI_DEPTH | DAPPLE_AI_SKY, maxFaces = 4, longEdge = 480;
  std::string taps, name = fs::path(photo).stem().string();
  bool bands = false, coldOnly = false;
  for (int i = 5; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--tasks" && i + 1 < argc) tasks = std::atoi(argv[++i]);
    else if (a == "--taps" && i + 1 < argc) taps = argv[++i];
    else if (a == "--maxfaces" && i + 1 < argc) maxFaces = std::atoi(argv[++i]);
    else if (a == "--name" && i + 1 < argc) name = argv[++i];
    else if (a == "--long" && i + 1 < argc) longEdge = std::atoi(argv[++i]);
    else if (a == "--bands") bands = true;
    else if (a == "--cold-only") coldOnly = true;
  }
  fs::create_directories(outDir);
  dapple_ai_configure(models.c_str(), cache.c_str());
  std::printf("status %s\n", str(dapple_ai_status_json()).c_str());
  const auto t0 = Clock::now();
  DappleSession* s = dapple_session_open(photo.c_str(), DAPPLE_QUALITY_PREVIEW, 2048);
  if (!s) {
    std::printf("open failed: %s\n", dapple_last_error());
    return 1;
  }
  const double openMs = msSince(t0);
  std::printf("opened %s in %.0f ms, RSS %ld MB\n", name.c_str(), openMs, procKb("VmRSS") / 1024);
  std::string log = "{\"photo\":\"" + name + "\",\"open_ms\":" + std::to_string(openMs);

  // cold: the first job of the process (models load from disk); the cache for this photo is wiped first when --cold-only or by the caller
  Job cold = runJob(s, tasks, true, maxFaces);
  std::printf("COLD   %.0f ms  (RSS %ld MB, peak %ld MB)\n", cold.ms, procKb("VmRSS") / 1024, procKb("VmHWM") / 1024);
  std::printf("  %s\n", cold.json.c_str());
  log += ",\"cold_ms\":" + std::to_string(cold.ms) + ",\"cold\":" + cold.json;
  std::vector<Plane> planes = parsePlanes(cold.json);
  if (!coldOnly) {
    Job warm = runJob(s, tasks, true, maxFaces);  // models resident, recompute
    std::printf("WARM   %.0f ms  (RSS %ld MB, peak %ld MB)\n", warm.ms, procKb("VmRSS") / 1024, procKb("VmHWM") / 1024);
    Job cached = runJob(s, tasks, false, maxFaces);  // from the disk cache
    std::printf("CACHED %.0f ms\n", cached.ms);
    log += ",\"warm_ms\":" + std::to_string(warm.ms) + ",\"cached_ms\":" + std::to_string(cached.ms);
    std::printf("  warm groups: ");
    const std::size_t g = warm.json.find("\"groups\":[");
    std::printf("%s\n", warm.json.substr(g, warm.json.find(']', g) - g + 1).c_str());
  }
  // sheet
  DappleImage ph{};
  Sheet sheet;
  std::string legend;
  if (dapple_session_render(s, "{}", nullptr, longEdge, &ph) != DAPPLE_OK) {
    std::printf("render failed\n");
    return 1;
  }
  const Rgba photoTile = fromImage(ph);
  {
    Rgba blank = photoTile;
    std::fill(blank.d.begin(), blank.d.end(), 0);
    sheet.add(photoTile, blank);
    legend += "0: photo\n";
  }
  auto addPlane = [&](const std::string& id, const std::string& label, float lo = 0, float hi = 100, float fe = 10, bool range = false) {
    const std::string js = maskJson(id, lo, hi, fe, range);
    DappleImage mi{};
    if (dapple_session_mask_preview(s, js.c_str(), 0, longEdge, 1, &mi) != DAPPLE_OK) {
      std::printf("  preview failed for %s: %s\n", id.c_str(), dapple_last_error());
      return;
    }
    sheet.add(photoTile, fromImage(mi));
    legend += std::to_string(sheet.tiles.size() - 1) + ": " + label + "  " + id + "\n";
  };
  for (const Plane& p : planes) addPlane(p.id, p.key + " " + p.label);
  if (bands)
    for (const Plane& p : planes)
      if (p.key == "depth") {
        addPlane(p.id, "depth near band (0-33)", 0, 33, 6, true);
        addPlane(p.id, "depth mid band (33-66)", 33, 66, 6, true);
        addPlane(p.id, "depth far band (66-100)", 66, 100, 6, true);
      }
  // taps
  if (!taps.empty() && (tasks & 31)) {
    std::size_t i = 0;
    std::string lastId;
    while (i < taps.size()) {
      const bool sub = taps[i] == '-';
      if (taps[i] == '+' || taps[i] == '-') ++i;
      float u = 0, v = 0;
      int used = 0;
      if (std::sscanf(taps.c_str() + i, "%f,%f%n", &u, &v, &used) != 2) break;
      i += std::size_t(used);
      if (i < taps.size() && taps[i] == ';') ++i;
      const auto tt = Clock::now();
      const std::string r = str(dapple_session_ai_tap_json(s, u, v, sub ? 1 : 0));
      std::printf("TAP %s%.3f,%.3f  %.0f ms wall: %s\n", sub ? "-" : "+", u, v, msSince(tt), r.c_str());
      lastId = field(r, 0, "id");
      if (!lastId.empty()) {
        addPlane(lastId, std::string("tap ") + (sub ? "-" : "+") + std::to_string(u).substr(0, 4) + "," + std::to_string(v).substr(0, 4));
        log += ",\"tap_" + std::to_string(sheet.tiles.size()) + "\":" + r;
      }
    }
  }
  // the export rule on a real plane: drop the first plane from the store, export an edit that uses it (restored from the cache), time it
  if (!planes.empty()) {
    const Plane& p = planes[0];
    dapple_mask_raster_remove(p.id.c_str());
    const std::string js = maskJson(p.id, 0, 100, 10, false);
    const auto te = Clock::now();
    const int rc = dapple_session_export_jpeg(s, js.c_str(), nullptr, 1024, 90, (fs::path(outDir) / (name + "_export.jpg")).string().c_str());
    std::printf("EXPORT with a missing plane (%s): rc=%d in %.0f ms, plane back in store: %d%s%s\n", p.key.c_str(), rc, msSince(te), dapple_mask_raster_has(p.id.c_str()),
                rc ? " error: " : "", rc ? dapple_last_error() : "");
  }
  std::printf("legend:\n%s", legend.c_str());
  std::ofstream(fs::path(outDir) / (name + "_legend.txt")) << legend;
  sheet.save(fs::path(outDir) / (name + "_sheet.jpg"));
  log += ",\"rss_mb\":" + std::to_string(procKb("VmRSS") / 1024) + ",\"peak_mb\":" + std::to_string(procKb("VmHWM") / 1024) + "}";
  std::ofstream(fs::path(outDir) / (name + "_log.json")) << log;
  dapple_ai_release(1);
  std::printf("after release: RSS %ld MB\n", procKb("VmRSS") / 1024);
  dapple_session_close(s);
  return 0;
}
