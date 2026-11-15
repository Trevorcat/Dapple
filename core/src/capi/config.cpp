#include "session.h"

// ---- optional GPU backend (core/gpu): runtime flag + status ----
int32_t dapple_gpu_set_enabled(int32_t mode) {
#ifdef DAPPLE_HAVE_GPU
  return guarded(
      [&]() -> int32_t {
        dapple::engine::gpu::installProvider();
        dapple::engine::gpu::setRuntimeEnabled(mode);
        return dapple::engine::gpu::runtimeEnabled() ? 1 : 0;
      },
      0);
#else
  (void)mode;
  return 0;
#endif
}

void dapple_gpu_configure(const char* cache_dir, const char* app_version, const char* fingerprint) {
  guardedVoid([&] {
#ifdef DAPPLE_HAVE_GPU
    dapple::engine::gpu::configure(cache_dir ? cache_dir : "", app_version ? app_version : "", fingerprint ? fingerprint : "");
#else
    // CPU-only build: the same cache directory still holds the film tables and the print-exposure constants (the prebake persists them there)
    if (cache_dir && cache_dir[0])
      dapple::engine::filmnode::setFilmTableDisk((fs::path(cache_dir) / "film_tables").string(), std::string(app_version ? app_version : "") + "|" + (fingerprint ? fingerprint : ""));
#endif
  });
}

void dapple_gpu_warmup(void) {
#ifdef DAPPLE_HAVE_GPU
  guardedVoid([&] { dapple::engine::gpu::startWarmup(); });
#endif
}

char* dapple_engine_stats_json(void) { return guarded([&]() -> char* { return dupString(dapple::engine::executorStatsJson()); }, nullptr); }

char* dapple_gpu_status_json(void) {
  return guarded(
      [&]() -> char* {
#ifdef DAPPLE_HAVE_GPU
        dapple::engine::gpu::installProvider();
        return dupString(dapple::engine::gpu::statusJson());
#else
        return dupString("{\"compiled\":false,\"enabled\":false,\"available\":false}");
#endif
      },
      nullptr);
}

char* dapple_engine_info_json(void) { return guarded([&]() -> char* { return dupString(dapple::engine::infoJson()); }, nullptr); }

void dapple_thread_background(int32_t on) { sys::setBackground(on != 0); }

void dapple_frame_trace_enable(int32_t on) {
  guardedVoid([&] {
    dapple::ftrace::S().on.store(on != 0);
    if (on) dapple::ftrace::clear();
  });
}
int32_t dapple_frame_trace_dump(const char* path) { return guarded([&]() -> int32_t { return path && dapple::ftrace::dump(path) ? DAPPLE_OK : DAPPLE_ERROR; }, DAPPLE_ERROR); }
void dapple_frame_trace_clear(void) { guardedVoid([&] { dapple::ftrace::clear(); }); }

void dapple_set_interaction(int32_t active) {
  dapple::demosaic::setInteraction(active != 0);
  sys::setInteraction(active != 0);
}

void dapple_perf_configure(int32_t thread_limit, int32_t target_frame_ms) {
  guardedVoid([&] {
    ThreadPool::instance().setLimit(thread_limit);
    sys::adpf().setTarget(int64_t(std::max(0, target_frame_ms)) * 1000000);
  });
}

float dapple_thermal_headroom(int32_t forecast_seconds) { return sys::adpf().headroom(forecast_seconds); }

static int32_t setRepaintCallbackImpl(DappleRepaintFn fn, void* user);
int32_t dapple_set_repaint_callback(DappleRepaintFn fn, void* user) {
  return guarded([&]() -> int32_t { return setRepaintCallbackImpl(fn, user); }, DAPPLE_ERROR);
}
static int32_t setRepaintCallbackImpl(DappleRepaintFn fn, void* user) {
  static DappleRepaintFn cur = nullptr;
  static void* curUser = nullptr;
  using namespace dapple::engine::filmnode;
  if (cur) removeTableLandedListener(reinterpret_cast<TableLandedFn>(cur), curUser);
  cur = nullptr;
  curUser = nullptr;
  if (!fn) return DAPPLE_OK;
  cur = fn;
  curUser = user;
  addTableLandedListener(reinterpret_cast<TableLandedFn>(fn), user);
  return DAPPLE_OK;
}

static char* perfInfoImpl();
char* dapple_perf_info_json(void) { return guarded([&]() -> char* { return perfInfoImpl(); }, nullptr); }
static char* perfInfoImpl() {
  const sys::Topology& t = sys::topology();
  ThreadPool& pool = ThreadPool::instance();
  std::string j = "{\"cpus\":" + std::to_string(t.total) + ",\"perf\":" + std::to_string(t.perf.size()) +
                  ",\"efficient\":" + std::to_string(t.efficient.size()) + ",\"pool\":" + std::to_string(pool.size()) +
                  ",\"adpf\":" + (sys::adpf().hintAvailable() ? "true" : "false") +
                  ",\"adpf_session\":" + (sys::adpf().sessionActive() ? "true" : "false") +
                  ",\"thermal_status\":" + std::to_string(sys::adpf().status()) + "}";
  return dupString(j);
}
