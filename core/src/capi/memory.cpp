#include "session.h"

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static char* memoryJsonImpl();
char* dapple_memory_json(void) { return guarded([&]() -> char* { return memoryJsonImpl(); }, nullptr); }
static char* memoryJsonImpl() {
  char b[192];
  std::snprintf(b, sizeof b, "{\"footprint\":%llu,\"resident\":%llu,\"available\":%llu,\"exportBudget\":%llu,\"stream\":\"%s\"}",
                (unsigned long long)mem::footprint(), (unsigned long long)mem::resident(), (unsigned long long)mem::available(),
                (unsigned long long)xstream::budgetBytes(),
                xstream::envMode() == xstream::Mode::On ? "on" : xstream::envMode() == xstream::Mode::Off ? "off" : "auto");
  return dupString(b);
}

namespace {

namespace {
// The mosaic cache's budget on a phone: one 61 MP mosaic (121 MB) and a neighbour, never more than 1/32 of the device (3 GB: 96 MB, 8 GB: 256 MB).
[[maybe_unused]] std::size_t mosaicBudgetFor(std::size_t total, bool lowRam) {
  constexpr std::size_t MB = std::size_t(1) << 20;
  return lowRam ? std::size_t(64) * MB : std::clamp(total / 32, std::size_t(96) * MB, std::size_t(320) * MB);
}
std::string memoryReport() {
  char b[1024];
  std::size_t sessBytes = 0;
  int sessions = 0, lazy = 0;
  {
    std::lock_guard<std::mutex> lk(sessionRegistryMutex());
    for (DappleSession* s : sessionRegistry()) {
      ++sessions;
      const std::size_t px = std::size_t(std::max(0, s->source.linear.width)) * std::size_t(std::max(0, s->source.linear.height));
      sessBytes += px * 12;
      if (s->lazyOpen.load()) ++lazy;
    }
  }
  std::size_t mosBytes = 0, mosBudget = 0;
  int mosEntries = 0;
  mosaicCacheUsage(&mosBytes, &mosBudget, &mosEntries);
  unsigned long long heapAlloc = 0, heapRetained = 0;
#if defined(__ANDROID__) || defined(__linux__)
  {
    const struct mallinfo mi = mallinfo();
    heapAlloc = (unsigned long long)(unsigned)mi.uordblks;
    heapRetained = (unsigned long long)(unsigned)mi.fordblks;
  }
#endif
  unsigned long long pooled = 0, poolBudget = 0, gpuBudget = 0, gpuUsage = 0;
  int unified = -1;
#ifdef DAPPLE_HAVE_GPU
  if (dapple::engine::GpuProvider* p = dapple::engine::gpuProvider()) {
    dapple::engine::GpuProvider::MemoryInfo mi;
    if (p->memoryInfo(&mi)) pooled = mi.pooled, poolBudget = mi.poolBudget, gpuBudget = mi.budget, gpuUsage = mi.usage, unified = mi.unified ? 1 : 0;
  }
#endif
  long long gpuLive = 0;
  int gpuAllocs = 0;
#ifdef DAPPLE_HAVE_GPU
  gpuLive = (long long)hal::g_liveMemBytes.load(), gpuAllocs = hal::g_liveMemAllocs.load();
#endif
  int planes = 0, rasters = 0;
  const std::size_t planeBytes = local::planeCacheBytes(&planes), rasterBytes = local::rasterStoreBytes(&rasters);
  const xstream::Budget bud = xstream::currentBudget();
  std::snprintf(b, sizeof b,
                "{\"footprint\":%llu,\"available\":%llu,\"deviceTotal\":%llu,\"lowRam\":%s,\"nativeHeap\":{\"allocated\":%llu,\"free\":%llu},"
                "\"sessions\":{\"count\":%d,\"lazy\":%d,\"pictureBytes\":%llu},\"mosaicCache\":{\"bytes\":%llu,\"budget\":%llu,\"entries\":%d},"
                "\"gpu\":{\"pooled\":%llu,\"poolBudget\":%llu,\"budget\":%llu,\"usage\":%llu,\"unified\":%d,\"deviceMemory\":%lld,\"allocations\":%d},\"ai\":{\"modelBytes\":%llu},"
                "\"local\":{\"planeBytes\":%llu,\"planes\":%d,\"rasterBytes\":%llu,\"rasters\":%d},"
                "\"exportBudget\":{\"bytes\":%llu,\"how\":\"%s\"}}",
                (unsigned long long)mem::footprint(), (unsigned long long)mem::available(), (unsigned long long)mem::deviceTotal(),
                mem::deviceClass().lowRam.load() ? "true" : "false", heapAlloc, heapRetained, sessions, lazy, (unsigned long long)sessBytes,
                (unsigned long long)mosBytes, (unsigned long long)mosBudget, mosEntries, pooled, poolBudget, gpuBudget, gpuUsage, unified, gpuLive, gpuAllocs,
                (unsigned long long)dapple::aimask::rt::resident(), (unsigned long long)planeBytes, planes, (unsigned long long)rasterBytes, rasters,
                (unsigned long long)bud.bytes, esc(bud.how).c_str());
  return b;
}
// One short line for the log: the figures that move.
std::string memoryLine() {
  std::size_t mosBytes = 0;
  int mosEntries = 0;
  mosaicCacheUsage(&mosBytes, nullptr, &mosEntries);
  int sessions = 0;
  std::size_t sessBytes = 0;
  {
    std::lock_guard<std::mutex> lk(sessionRegistryMutex());
    for (DappleSession* s : sessionRegistry()) {
      ++sessions;
      sessBytes += std::size_t(std::max(0, s->source.linear.width)) * std::size_t(std::max(0, s->source.linear.height)) * 12;
    }
  }
  std::size_t pooled = 0;
  long long gpuLive = 0;
  int gpuAllocs = 0;
#ifdef DAPPLE_HAVE_GPU
  if (dapple::engine::GpuProvider* p = dapple::engine::gpuProvider()) {
    dapple::engine::GpuProvider::MemoryInfo mi;
    if (p->memoryInfo(&mi)) pooled = mi.pooled;
  }
  gpuLive = (long long)hal::g_liveMemBytes.load(), gpuAllocs = hal::g_liveMemAllocs.load();
#endif
  unsigned long long heapAlloc = 0, heapFree = 0;
#if defined(__ANDROID__) || defined(__linux__)
  {
    const struct mallinfo mi = mallinfo();
    heapAlloc = (unsigned long long)(unsigned)mi.uordblks;
    heapFree = (unsigned long long)(unsigned)mi.fordblks;
  }
#endif
  int planes = 0, rasters = 0;
  const std::size_t planeBytes = local::planeCacheBytes(&planes), rasterBytes = local::rasterStoreBytes(&rasters);
  char b[448];
  std::snprintf(b, sizeof b,
                "footprint %.0f MB, native heap in use %.0f MB (free %.0f), %d sessions (%.0f MB of pictures), mosaic cache %.0f MB (%d), GPU pool %.0f MB (device memory %.0f MB in %d), AI models %.0f MB, "
                "mask planes %.0f MB (%d), rasters %.0f MB (%d)",
                double(mem::footprint()) / 1048576.0, double(heapAlloc) / 1048576.0, double(heapFree) / 1048576.0, sessions, double(sessBytes) / 1048576.0, double(mosBytes) / 1048576.0,
                mosEntries, double(pooled) / 1048576.0, double(gpuLive) / 1048576.0, gpuAllocs, double(dapple::aimask::rt::resident()) / 1048576.0, double(planeBytes) / 1048576.0, planes,
                double(rasterBytes) / 1048576.0, rasters);
  return b;
}
}  // namespace

}  // namespace

int32_t dapple_memory_configure(const char* json) {
  return guarded(
      [&]() -> int32_t {
        if (!json) return DAPPLE_ERROR;
        const std::string j = json;
        auto num = [&](const char* key, double* out) {
          const std::string k = std::string("\"") + key + "\"";
          const std::size_t p = j.find(k);
          if (p == std::string::npos) return false;
          const std::size_t c = j.find(':', p + k.size());
          if (c == std::string::npos) return false;
          char* end = nullptr;
          const double v = std::strtod(j.c_str() + c + 1, &end);
          if (end == j.c_str() + c + 1) return false;
          *out = v;
          return true;
        };
        mem::DeviceClass& d = mem::deviceClass();
        double v = 0;
        if (num("totalBytes", &v) && v > 0) d.totalBytes.store(std::size_t(v));
        if (num("memoryClassMb", &v)) d.memoryClassMb.store(int(v));
        if (num("largeMemoryClassMb", &v)) d.largeMemoryClassMb.store(int(v));
        if (const std::size_t p = j.find("\"lowRam\""); p != std::string::npos) d.lowRam.store(j.find("true", p) == j.find_first_not_of(" :", p + 8));
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        setMosaicCacheBudget(mosaicBudgetFor(mem::deviceTotal(), d.lowRam.load()));
#endif
        const xstream::Budget bud = xstream::currentBudget();
        stageLog("memory_configure: device %.0f MB%s, memory class %d / %d MB; export budget now %s", double(mem::deviceTotal()) / 1048576.0,
                 d.lowRam.load() ? " (low RAM)" : "", d.memoryClassMb.load(), d.largeMemoryClassMb.load(), bud.how.c_str());
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_memory_report_json(void) {
  return guarded([&]() -> char* { return dupString(memoryReport()); }, static_cast<char*>(nullptr));
}

int64_t dapple_memory_trim(int32_t level) {
  return guarded(
      [&]() -> int64_t {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string before = memoryLine();
        std::size_t freed = 0;
        // the cached mosaics (80 - 121 MB each): reopening a photo unpacks it again (~1 s), the editor closed or memory is short
        freed += trimMosaicCache(0);
        if (level >= 2) clearMosaicCache();  // and the exposure anchors
        freed += engine::filmnode::releaseFrameWorkspaces();
#ifdef DAPPLE_HAVE_GPU
        if (engine::GpuProvider* p = engine::gpuProvider()) freed += p->trimPool(0);
        const bool demosaicDevice = demosaic::gpuBackend() && demosaic::gpuBackend()->trim();  // the decoder's own GPU device (created again on use)
#else
        const bool demosaicDevice = false;
#endif
        dapple_ai_release(level >= 1 ? 1 : 0);
        if (level >= 1) local::resetPlaneCache();
#if defined(__ANDROID__) && defined(M_PURGE)
        mallopt(M_PURGE, 0);  // hand the allocator's free pages back to the OS (scudo / jemalloc purge)
#elif defined(__linux__)
        malloc_trim(0);
#endif
        std::string held;
#ifdef DAPPLE_HAVE_GPU
        held = hal::liveMemSummary();
#endif
        stageLog("memory_trim %d: released %.0f MB of caches%s in %.0f ms; before: %s; after: %s%s%s", int(level), double(freed) / 1048576.0,
                 demosaicDevice ? " and the demosaic GPU device" : "", msSince(t0), before.c_str(), memoryLine().c_str(),
                 held.empty() ? "" : "; device memory held: ", held.c_str());
        return int64_t(freed);
      },
      int64_t(-1));
}
