// Film tables on disk (core/engine/nodes/film/film_tables.cpp "warm from disk"): bit-identical round trip, a corrupt file is rebuilt silently, a changed
// build salt (version mismatch) does not find the old files, the directory stays under its size cap (least recently used files go first), several
// processes baking the same stock at once leave only whole files (per-process temporary names).
//   dapple_film_disk_tests                  the whole test (the parent: runs itself as child processes, so every load starts with empty memory caches)
//   dapple_film_disk_tests child <dir> <salt> <stock>   one process: tables of the stock through tablesFor(), prints RESULT hits= misses= stores= corrupt= sum=
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../deterministic.h"
#include "../test_tmp.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "engine/nodes/film/film_async.h"
#include "engine/nodes/film/film_node_internal.h"
#include "engine/nodes/film/film_nodes.h"

namespace fs = std::filesystem;
namespace ef = dapple::engine::filmnode;

static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
template <class V>
static std::uint64_t fnvVec(std::uint64_t h, const V& v) {
  return fnv(h, v.data(), v.size() * sizeof(float));
}
static std::uint64_t sumTables(const ef::FilmTablesHandle& tb) {
  std::uint64_t h = 14695981039346656037ull;
  const auto& r = tb.raw->t;
  const auto& c = tb.curves->t;
  const auto& p = tb.print->t;
  h = fnv(h, r.mIn, sizeof r.mIn), h = fnvVec(h, r.raw);
  h = fnv(h, c.gamma, sizeof c.gamma), h = fnvVec(h, c.dn), h = fnvVec(h, c.d0), h = fnv(h, c.dirQ, sizeof c.dirQ);
  h = fnv(h, p.dmin, sizeof p.dmin), h = fnv(h, p.dscale, sizeof p.dscale), h = fnvVec(h, p.lut), h = fnvVec(h, p.srgbDecode), h = fnv(h, p.glareRgb, sizeof p.glareRgb);
  return h;
}

static int child(const std::string& dir, const std::string& salt, const std::string& stock) {
  const char* envData = std::getenv("DAPPLE_FILM_DATA");
  const fs::path pack = (envData ? fs::path(envData) : fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film") / "dapple.dfp";
  std::string err;
  if (!dapple::film::setLibraryPath(pack, &err)) return std::printf("SKIP no film pack (%s)\n", err.c_str()), 77;
  ef::setFilmTableDisk(dir, salt);
  auto lib = dapple::film::defaultLibrary();
  ef::FilmFrameArgs a;
  a.lib = lib.get();
  a.stock = stock.c_str();
  const std::string paper = dapple::film::defaultPaperFor(*lib, stock);
  a.paper = paper.c_str();
  a.w = a.h = 64;
  a.quality = 2;
  ef::FilmResolved r;
  if (!ef::filmFrameResolved(a, r)) return std::printf("RESULT resolve failed\n"), 1;
  ef::FilmTablesHandle tb;
  if (!ef::tablesFor(r, tb)) return std::printf("RESULT tables failed\n"), 1;
  ef::filmTableDiskDrain();
  unsigned long long h = 0, m = 0, s = 0, c = 0;
  ef::filmTableDiskStats(&h, &m, &s, &c);
  std::printf("RESULT hits=%llu misses=%llu stores=%llu corrupt=%llu sum=%016llx\n", h, m, s, c, (unsigned long long)sumTables(tb));
  return 0;
}

struct Res {
  unsigned long long hits = 0, misses = 0, stores = 0, corrupt = 0, sum = 0;
  bool ok = false;
};
static std::string g_self;
static Res run(const std::string& dir, const std::string& salt, const std::string& stock, const std::string& env = "") {
#ifdef _WIN32
  const std::string cmd = "\"" + (env.empty() ? std::string() : "set " + env + "&& ") + "\"" + g_self + "\" child \"" + dir + "\" " + salt + " " + stock + " 2>&1\"";
#else
  const std::string cmd = (env.empty() ? "" : env + " ") + "\"" + g_self + "\" child \"" + dir + "\" " + salt + " " + stock + " 2>&1";
#endif
  Res r;
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return r;
  char line[512];
  while (std::fgets(line, sizeof line, p)) {
    if (std::strncmp(line, "RESULT", 6) == 0) {
      unsigned long long s = 0;
      r.ok = std::sscanf(line, "RESULT hits=%llu misses=%llu stores=%llu corrupt=%llu sum=%llx", &r.hits, &r.misses, &r.stores, &r.corrupt, &s) == 5;
      r.sum = s;
    }
  }
  pclose(p);
  return r;
}
static std::uint64_t dirBytes(const fs::path& d, int* files = nullptr) {
  std::uint64_t t = 0;
  int n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(d, ec))
    if (e.is_regular_file(ec) && e.path().extension() == ".bin") t += e.file_size(ec), ++n;
  if (files) *files = n;
  return t;
}

int main(int argc, char** argv) {
  g_self = argv[0];
  if (argc >= 5 && std::string(argv[1]) == "child") return child(argv[2], argv[3], argv[4]);
  const fs::path dir = fs::temp_directory_path() / ("dapple_film_disk_" + testUniqueTag());
  fs::remove_all(dir);
  const std::string d = dir.string();
  int rc = 0;
  {
    // 1. cold: bakes and writes; second start: loads every table from disk, bit identical
    const Res cold = run(d, "saltA", "kodak_portra_400");
    if (!cold.ok) return std::printf("SKIP: the child did not run (no pack?)\n"), 77;
    int files = 0;
    dirBytes(dir, &files);
    CHECK(cold.hits == 0 && cold.stores >= 3 && files >= 3, "cold start: hits %llu stores %llu files %d", cold.hits, cold.stores, files);
    const Res warm = run(d, "saltA", "kodak_portra_400");
    CHECK(warm.ok && warm.hits >= 3 && warm.stores == 0 && warm.corrupt == 0, "second start: hits %llu misses %llu stores %llu corrupt %llu", warm.hits, warm.misses, warm.stores, warm.corrupt);
    CHECK(warm.sum == cold.sum, "round trip not bit identical: %016llx vs %016llx", warm.sum, cold.sum);

    // 2. corruption: one flipped byte -> that table is rebuilt silently (same content), the file is replaced by a valid one
    std::vector<fs::path> bins;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".bin") bins.push_back(e.path());
    {
      std::fstream f(bins[0], std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(200);
      char b = 0;
      f.seekg(200);
      f.read(&b, 1);
      f.seekp(200);
      b = char(b ^ 0x5A);
      f.write(&b, 1);
    }
    const Res bad = run(d, "saltA", "kodak_portra_400");
    CHECK(bad.ok && bad.corrupt == 1 && bad.sum == cold.sum && bad.stores == 1, "corrupt file: corrupt %llu stores %llu sum %s", bad.corrupt, bad.stores, bad.sum == cold.sum ? "same" : "DIFFERENT");
    const Res healed = run(d, "saltA", "kodak_portra_400");
    CHECK(healed.ok && healed.corrupt == 0 && healed.hits >= 3 && healed.sum == cold.sum, "after the rebuild: corrupt %llu hits %llu", healed.corrupt, healed.hits);
    // a truncated file
    fs::resize_file(bins[1], 30);
    const Res trunc = run(d, "saltA", "kodak_portra_400");
    CHECK(trunc.ok && trunc.corrupt == 1 && trunc.sum == cold.sum, "truncated file: corrupt %llu", trunc.corrupt);

    // 3. version mismatch (another build salt): the old files are not found, new ones are written next to them
    const Res other = run(d, "saltB", "kodak_portra_400");
    CHECK(other.ok && other.hits == 0 && other.stores >= 3 && other.sum == cold.sum, "other salt: hits %llu stores %llu", other.hits, other.stores);

    // 4. cap: 1 MB with two stocks of ~1.2 MB each: the directory ends up under the cap, least recently used first
    const fs::path cdir = fs::temp_directory_path() / ("dapple_film_disk_cap_" + testUniqueTag());
    fs::remove_all(cdir);
    run(cdir.string(), "saltA", "kodak_portra_400", "DAPPLE_FILM_DISK_CAP_MB=1");
    run(cdir.string(), "saltA", "kodak_ektar_100", "DAPPLE_FILM_DISK_CAP_MB=1");
    const std::uint64_t capped = dirBytes(cdir);
    CHECK(capped <= (1u << 20), "capped directory holds %llu bytes", (unsigned long long)capped);
    const fs::path udir = fs::temp_directory_path() / ("dapple_film_disk_nocap_" + testUniqueTag());
    fs::remove_all(udir);
    run(udir.string(), "saltA", "kodak_portra_400");
    run(udir.string(), "saltA", "kodak_ektar_100");
    CHECK(dirBytes(udir) > capped, "without a cap the directory is larger (%llu vs %llu)", (unsigned long long)dirBytes(udir), (unsigned long long)capped);
    fs::remove_all(cdir);
    fs::remove_all(udir);

    // 5. concurrent processes baking the same stock into the same directory: only whole files remain, and the content is the cold content
    const fs::path pdir = fs::temp_directory_path() / ("dapple_film_disk_par_" + testUniqueTag());
    fs::remove_all(pdir);
    std::vector<std::thread> th;
    std::vector<Res> res(4);
    for (int i = 0; i < 4; ++i) th.emplace_back([&, i] { res[std::size_t(i)] = run(pdir.string(), "saltA", "kodak_portra_400"); });
    for (auto& t : th) t.join();
    for (const Res& r : res) CHECK(r.ok && r.sum == cold.sum, "concurrent process: sum differs");
    const Res after = run(pdir.string(), "saltA", "kodak_portra_400");
    CHECK(after.ok && after.corrupt == 0 && after.hits >= 3 && after.sum == cold.sum, "after concurrent writers: corrupt %llu hits %llu", after.corrupt, after.hits);
    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(pdir)) leftovers += e.path().extension() == ".tmp";
    CHECK(leftovers == 0, "%d temporary files left behind", leftovers);
    fs::remove_all(pdir);
  }
  fs::remove_all(dir);
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : rc;
}
