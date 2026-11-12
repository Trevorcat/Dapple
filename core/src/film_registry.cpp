// Process-wide film library and the pool of simulators with cached bakes (see film.h).
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>

#include "dapple/film.h"

namespace dapple::film {

namespace {

struct Registry {
  std::mutex mtx;
  std::shared_ptr<const FilmLibrary> lib;
  std::filesystem::path lazyPath;
  bool lazyFailed = false;
  unsigned lazyGen = 0;  // bumped whenever the lazy path / library is replaced
  std::string lastError;
  std::mutex loadMtx;    // serialises the (slow) lazy load without holding `mtx`
};
Registry& registry() {
  static Registry* r = new Registry;  // leaked on purpose (see ThreadPool)
  return *r;
}

struct PoolEntry {
  std::shared_ptr<const FilmLibrary> lib;
  std::string film, paper;
  std::unique_ptr<FilmSimulator> sim;
  bool busy = false;
  std::uint64_t used = 0;
};
struct Pool {
  std::mutex mtx;
  std::vector<std::unique_ptr<PoolEntry>> entries;
  std::uint64_t tick = 0;
};
Pool& pool() {
  static Pool* p = new Pool;
  return *p;
}
constexpr std::size_t kPoolMax = 4;

}  // namespace

void setLibrary(std::shared_ptr<const FilmLibrary> lib) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mtx);
  r.lib = std::move(lib);
  r.lazyFailed = false;
  r.lastError.clear();
  ++r.lazyGen;
}

bool setLibraryPath(const std::filesystem::path& path, std::string* err) {
  std::error_code ec;
  std::shared_ptr<FilmLibrary> lib = std::filesystem::is_directory(path, ec) ? FilmLibrary::loadJsonDir(path, err)
                                                                              : FilmLibrary::loadPack(path, err);
  if (!lib) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mtx);
    r.lastError = err ? *err : std::string("cannot load film library");
    return false;
  }
  setLibrary(std::move(lib));
  return true;
}

void setLibraryPathLazy(const std::filesystem::path& path) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mtx);
  r.lazyPath = path;
  r.lazyFailed = false;
  r.lastError.clear();
  r.lib.reset();
  ++r.lazyGen;
}

std::shared_ptr<const FilmLibrary> defaultLibrary() {
  Registry& r = registry();
  {
    std::lock_guard<std::mutex> lock(r.mtx);
    if (r.lib || r.lazyPath.empty() || r.lazyFailed) return r.lib;
  }
  // The load takes up to ~1.5 s (JSON directory): do it outside `mtx` so setLibrary()/lastLibraryError() and
  // readers that already have a library never wait for it; concurrent first callers queue on loadMtx and the
  // later ones find the library installed.
  std::lock_guard<std::mutex> loading(r.loadMtx);
  std::filesystem::path path;
  unsigned gen;
  {
    std::lock_guard<std::mutex> lock(r.mtx);
    if (r.lib || r.lazyPath.empty() || r.lazyFailed) return r.lib;
    path = r.lazyPath;
    gen = r.lazyGen;
  }
  std::string err;
  std::error_code ec;
  std::shared_ptr<FilmLibrary> lib;
  try {
    lib = std::filesystem::is_directory(path, ec) ? FilmLibrary::loadJsonDir(path, &err) : FilmLibrary::loadPack(path, &err);
  } catch (const std::exception& e) {
    err = e.what();
  }
  std::lock_guard<std::mutex> lock(r.mtx);
  if (gen != r.lazyGen) return r.lib;  // replaced while loading: the newer setting wins
  if (lib) {
    r.lib = std::move(lib);
  } else {
    r.lazyFailed = true;
    r.lastError = err.empty() ? "cannot load film library from " + path.string() : err;
  }
  return r.lib;
}

std::string lastLibraryError() {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mtx);
  return r.lastError;
}

std::vector<StockInfo> listStocks(const FilmLibrary& lib) {
  std::vector<StockInfo> out;
  for (const std::string& id : lib.names()) {
    const FilmStock* s = lib.find(id);
    if (!s) continue;
    const char* kind = s->isPrintMedium() ? "paper" : (s->positive() ? "slide" : "negative");
    out.push_back({id, s->name.empty() ? id : s->name, kind, std::string(kind) == "negative" ? defaultPaperFor(lib, id) : std::string()});
  }
  return out;
}

std::string defaultPaperFor(const FilmLibrary& lib, const std::string& filmId) {
  const FilmStock* f = lib.find(filmId);
  if (!f || f->positive()) return "none";
  // the paper the profile is made to be printed on (info.target_print): Fuji negatives on Crystal Archive, Kodak stills on Portra Endura,
  // cine negatives on 2383
  if (!f->targetPrint.empty() && lib.find(f->targetPrint) && lib.find(f->targetPrint)->isPrintMedium()) return f->targetPrint;
  if (f->use == "cine" && lib.find("kodak_2383")) return "kodak_2383";
  return lib.find("kodak_portra_endura") ? "kodak_portra_endura" : "none";
}

SimulatorLease::SimulatorLease(SimulatorLease&& o) noexcept : sim_(o.sim_), entry_(o.entry_) {
  o.sim_ = nullptr;
  o.entry_ = nullptr;
}
SimulatorLease& SimulatorLease::operator=(SimulatorLease&& o) noexcept {
  if (this != &o) {
    reset();
    sim_ = o.sim_;
    entry_ = o.entry_;
    o.sim_ = nullptr;
    o.entry_ = nullptr;
  }
  return *this;
}
SimulatorLease::~SimulatorLease() { reset(); }
void SimulatorLease::reset() {
  if (!entry_) return;
  Pool& p = pool();
  std::lock_guard<std::mutex> lock(p.mtx);
  PoolEntry* e = static_cast<PoolEntry*>(entry_);
  e->busy = false;
  entry_ = nullptr;
  sim_ = nullptr;
  // the pool grows past kPoolMax only while every entry is leased: shrink back as soon as one is released
  if (p.entries.size() > kPoolMax)
    p.entries.erase(std::remove_if(p.entries.begin(), p.entries.end(),
                                   [&](const std::unique_ptr<PoolEntry>& u) { return u.get() == e; }),
                    p.entries.end());
}

// Entries are keyed by (library pointer, film, paper). On a miss the least recently used idle entry is recycled
// with a fresh simulator: its baked tables are discarded (they belong to another stock or library).
SimulatorLease acquireSimulator(const std::shared_ptr<const FilmLibrary>& lib, const std::string& film,
                                const std::string& paper) {
  Pool& p = pool();
  std::lock_guard<std::mutex> lock(p.mtx);
  PoolEntry* hit = nullptr;
  for (auto& e : p.entries)
    if (!e->busy && e->lib == lib && e->film == film && e->paper == paper) hit = e.get();
  if (!hit) {
    if (p.entries.size() >= kPoolMax)
      for (auto& e : p.entries)
        if (!e->busy && (!hit || e->used < hit->used)) hit = e.get();
    if (!hit) {  // pool not full yet, or every entry is leased by a concurrent render
      p.entries.push_back(std::make_unique<PoolEntry>());
      hit = p.entries.back().get();
    }
    hit->lib = lib;
    hit->film = film;
    hit->paper = paper;
    hit->sim = std::make_unique<FilmSimulator>(lib);
  }
  hit->busy = true;
  hit->used = ++p.tick;
  SimulatorLease l;
  l.sim_ = hit->sim.get();
  l.entry_ = hit;
  return l;
}

}  // namespace dapple::film
