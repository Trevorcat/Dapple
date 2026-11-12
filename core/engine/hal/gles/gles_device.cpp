// OpenGL ES 3.1 compute backend of the HAL (docs/engine/pipeline-architecture.md 6.3, 6.4, 6.6, 10.4 stage c).
//
// Threading: ES contexts are current on one thread, so one dedicated GL thread owns the (surfaceless) context. Every HAL
// call that needs GL runs as a task on that thread; CommandLists only RECORD (plain structs, no GL), submit() enqueues the
// recorded list and returns immediately, and the fence is a glFenceSync created at the end of the list.
//
// Host-visible buffers: ES 3.1 has no glBufferStorage, so Upload / Readback buffers are a host shadow (BufferH::mapped)
// plus a GL buffer; flush() uploads the shadow, wait() (and invalidate()) maps the GL buffer back into the shadow.
//
// Specialisation: no spec constants in ES, so the ESSL carries SPIRV_CROSS_CONSTANT_ID_n macros with defaults and a
// variant is the source with `#define`s spliced after the `#version` line, linked as its own program (cached by key).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include "engine/exec/big_core.h"
#include "engine/hal/gles/gles_device.h"
#include "engine/hal/gles/gles_gl.h"
#include "engine/hal/hal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace dapple::hal::gles {

namespace {
std::uint64_t hostRamBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX m;
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && ps > 0 ? std::uint64_t(pages) * std::uint64_t(ps) : 0;
#endif
}
}  // namespace

struct GlesShaderBin {
  const char* name;
  const char* essl;
  const char* sig;
  int glb[12];
  int specId[12], specBool[12], specDef[12];
  int nspec;
  int local[3];
  int localSpec[2];
  int sharedBytes;
  int pingpong;  // ES_PINGPONG kernel: trailing sampler `uDin` aliases the first image (see GPipe::aliasRead)
};
#include "gles_shader_table.inc"

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// dynamic loading
void* openLib(const char* const* names) {
  for (; *names; ++names) {
#if defined(_WIN32)
    if (HMODULE h = LoadLibraryA(*names)) return h;
#else
    if (void* h = dlopen(*names, RTLD_NOW | RTLD_LOCAL)) return h;
#endif
  }
  return nullptr;
}
void* sym(void* lib, const char* n) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), n));
#else
  return dlsym(lib, n);
#endif
}
std::string envStr(const char* n) {
  const char* v = std::getenv(n);
  return v ? v : "";
}

// ---------------------------------------------------------------------------------------------------------------------
struct GBuffer {
  GLuint id = 0;
  std::uint64_t size = 0, uid = 0;
  MemKind kind = MemKind::DeviceLocal;
  std::uint8_t* shadow = nullptr;  // Upload / Readback
  bool gpuWritten = false;         // Readback: GL buffer newer than the shadow
};
struct GImage {
  GLuint tex = 0;
  GLenum target = GL_TEXTURE_2D, internal = GL_RGBA8;
  Format fmt = Format::RGBA8;
  std::uint32_t w = 0, h = 0, d = 1;
  std::uint64_t uid = 0;
};
struct GPipe {
  GLuint prog = 0;
  const GlesShaderBin* bin = nullptr;
  std::string key;
  std::string callerSig;     // signature as the caller sees it (PipelineH::bindings)
  std::uint32_t nSlots = 0;  // slots the CALLER binds
  // ES_PINGPONG kernels (nr_luma, nr_chroma, dehaze_apply, detail): ES 3.1 forbids a read-write rgba16f image, so the ESSL has a
  // write-only image `uD` plus a sampler `uDin` (the LAST signature letter). The caller keeps the Vulkan signature (one image slot)
  // and the device binds the SAME texture to both. Legal in practice because every invocation reads only its own pixel before
  // writing it (no other invocation touches it) and the previous dispatch ended with a texture-fetch barrier. The planner may
  // replace this by a real ping-pong later.
  bool aliasRead = false;
  std::uint32_t aliasFrom = 0;  // caller slot whose image is also bound to the trailing sampler
};

GLenum internalFormat(Format f) {
  switch (f) {
    case Format::R8: return GL_R8;
    case Format::RGBA8: return GL_RGBA8;
    case Format::R16F: return GL_R16F;
    case Format::RGBA16F: return GL_RGBA16F;
    case Format::R32F: return GL_R32F;
    case Format::RG32F: return GL_RG32F;
    case Format::RGBA32F: return GL_RGBA32F;
    case Format::R32UI: return GL_R32UI;
    default: return 0;
  }
}
void uploadFormat(Format f, GLenum& fmt, GLenum& type) {
  switch (f) {
    case Format::R8: fmt = GL_RED; type = GL_UNSIGNED_BYTE; break;
    case Format::RGBA8: fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
    case Format::R16F: fmt = GL_RED; type = GL_HALF_FLOAT; break;
    case Format::RGBA16F: fmt = GL_RGBA; type = GL_HALF_FLOAT; break;
    case Format::R32F: fmt = GL_RED; type = GL_FLOAT; break;
    case Format::RG32F: fmt = GL_RG; type = GL_FLOAT; break;
    case Format::RGBA32F: fmt = GL_RGBA; type = GL_FLOAT; break;
    case Format::R32UI: fmt = GL_RED_INTEGER; type = GL_UNSIGNED_INT; break;
    default: fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
  }
}
// valid ES 3.1 image-unit formats (table of image formats); read-write only for the 32 bit single channel ones
bool storageOk(Format f) { return f == Format::RGBA8 || f == Format::RGBA16F || f == Format::R32F || f == Format::RGBA32F || f == Format::R32UI; }
bool storageRwOk(Format f) { return f == Format::R32F || f == Format::R32UI; }
const char* layoutName(Format f) {
  switch (f) {
    case Format::RGBA8: return "rgba8";
    case Format::RGBA16F: return "rgba16f";
    case Format::R32F: return "r32f";
    case Format::RGBA32F: return "rgba32f";
    case Format::R32UI: return "r32ui";
    default: return nullptr;
  }
}

enum class Op : std::uint8_t { BindPipe, BindBuf, BindImg, Params, Dispatch, Barrier, B2I, B2B, I2B, I2I, Flush };
struct Cmd {
  Op op;
  std::uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
  std::uint64_t u = 0;
  void* p = nullptr;
  void* q = nullptr;
};

constexpr std::size_t kRing = 512 * 1024;
constexpr std::uint32_t kParamBytes = 256;

struct Slot {
  std::vector<Cmd> cmds;
  std::vector<std::uint8_t> params;  // host ring, uploaded when the list executes
  std::size_t paramOff = 0;
  GLuint ubo = 0;
  std::atomic<std::uint64_t> submitted{0}, done{0};
  GLsync sync = nullptr;
  std::vector<GBuffer*> touched;  // Readback buffers written by the submission
  SubmitStats stats;              // of the last submission (passes = dispatches + copies, barriers with a non-empty mask, parts = flushPart + 1)
  std::mutex m;
  std::condition_variable cv;
};

class GlThread {
 public:
  GlThread() = default;
  ~GlThread() { stop(); }
  void start() { th_ = std::thread([this] { loop(); }); }
  void stop() {
    if (!th_.joinable()) return;
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
    }
    cv_.notify_all();
    th_.join();
  }
  void post(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> l(m_);
      q_.push(std::move(f));
    }
    cv_.notify_one();
  }
  // run on the GL thread and wait
  void run(const std::function<void()>& f) {
    if (std::this_thread::get_id() == th_.get_id()) return f();
    std::mutex dm;
    std::condition_variable dc;
    bool done = false;
    post([&] {
      f();
      std::lock_guard<std::mutex> l(dm);
      done = true;
      dc.notify_one();
    });
    std::unique_lock<std::mutex> l(dm);
    dc.wait(l, [&] { return done; });
  }
  std::thread::id id() const { return th_.get_id(); }

 private:
  void loop() {
    dapple::engine::gpu::detail::pinCurrentThreadToBigCores();  // every GL call of the device is recorded and submitted here
    for (;;) {
      std::function<void()> f;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return quit_ || !q_.empty(); });
        if (q_.empty()) return;
        f = std::move(q_.front());
        q_.pop();
      }
      f();
    }
  }
  std::thread th_;
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  bool quit_ = false;
};

class GlesDev;

class GlesCmd final : public CommandList {
 public:
  GlesCmd(GlesDev& d, int slot) : d_(d), slot_(slot) {}
  int slot() const override { return slot_; }
  void bindPipeline(PipelineH p) override { cmds_.push_back({Op::BindPipe, 0, 0, 0, 0, 0, 0, p.impl}); }
  void bind(std::uint32_t s, BufferH b) override { cmds_.push_back({Op::BindBuf, s, 0, 0, 0, 0, 0, b.impl}); }
  void bind(std::uint32_t s, ImageH i) override { cmds_.push_back({Op::BindImg, s, 0, 0, 0, 0, 0, i.impl}); }
  Status setParams(const void* p, std::size_t n) override;
  Status dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) override {
    cmds_.push_back({Op::Dispatch, gx, gy, gz});
    return err_;
  }
  void barrier(std::uint32_t m) override { cmds_.push_back({Op::Barrier, m}); }
  void copyBufferToImage(BufferH b, ImageH i, std::uint32_t w, std::uint32_t h, std::uint32_t dd, std::uint64_t off) override {
    cmds_.push_back({Op::B2I, w, h, dd, 0, 0, off, b.impl, i.impl});
  }
  void copyBufferToBuffer(BufferH s, BufferH t, std::uint64_t bytes, std::uint64_t off) override {
    cmds_.push_back({Op::B2B, 0, 0, 0, 0, 0, bytes, s.impl, t.impl});
    cmds_.back().e = std::uint32_t(off);  // source offset (32 bit is enough for a staging offset)
  }
  void copyImageToBuffer(ImageH i, BufferH b, const Rect2& r, std::uint64_t off) override {
    cmds_.push_back({Op::I2B, r.x, r.y, r.w, r.h, 0, off, i.impl, b.impl});
  }
  void copyImage(ImageH s, ImageH t, std::uint32_t w, std::uint32_t h) override { cmds_.push_back({Op::I2I, w, h, 0, 0, 0, 0, s.impl, t.impl}); }
  void timestamp(const char*) override {}
  Status flushPart() override {
    cmds_.push_back({Op::Flush});
    return err_;
  }
  Status status() const override { return err_; }

  GlesDev& d_;
  int slot_;
  std::vector<Cmd> cmds_;
  std::vector<std::uint8_t> params_;
  std::size_t paramOff_ = 0;
  Status err_ = Status::Ok;
};

class GlesDev final : public Device {
 public:
  ~GlesDev() override;
  bool init(const DeviceOptions& opt, std::string& err);

  const DeviceCaps& caps() const override { return caps_; }
  DeviceState state() const override { return lost_ ? DeviceState::Lost : DeviceState::Ok; }
  const std::string& lastError() const override { return lastErr_; }

  Status createBuffer(const BufferDesc&, BufferH*) override;
  Status createImage(const ImageDesc&, ImageH*) override;
  Status createPipeline(const PipelineDesc&, PipelineH*) override;
  void destroy(BufferH&) override;
  void destroy(ImageH&) override;
  ImageH dummyImage(Format) override;
  void flush(BufferH, std::uint64_t off, std::uint64_t n) override;
  void invalidate(BufferH, std::uint64_t off, std::uint64_t n) override;

  std::unique_ptr<CommandList> begin() override;
  void pinSlot(int s) override { pinned_ = s; }
  Status submit(CommandList&, FenceH*) override;
  Status wait(FenceH, std::uint64_t timeoutNs) override;
  bool poll(FenceH) override;
  Status waitAll() override;
  void waitIdle() override { waitAll(); }
  const std::vector<PassTime>& passTimes(FenceH) const override { return noTimes_; }
  SubmitStats stats(FenceH f) const override { return f ? slots_[f.slot].stats : SubmitStats{}; }
  double gpuAccumMs() const override { return 0; }
  std::vector<std::uint8_t> pipelineCacheBlob() const override;
  void trim() override;
  double splitGroups() const override { return splitGroups_; }
  std::size_t poolBudget() const override { return poolBudget_; }

  // used by GlesCmd
  Slot& slotRef(int i) { return slots_[std::size_t(i)]; }
  void fail(const std::string& e, Status s) {
    lastErr_ = e;
    if (s == Status::DeviceLost) lost_ = true;
  }

 private:
  friend class GlesCmd;
  bool initGl(const DeviceOptions& opt, std::string& err);
  bool loadLibs(std::string& err);
  bool makeContext(std::string& err);
  void queryCaps();
  void exec(Slot& s, std::vector<Cmd>& cmds);
  GLuint helperProgram(const std::string& key, const std::string& src);
  void syncBuffer(GBuffer* b);  // GL thread: Readback buffer -> shadow
  GLuint buildProgram(const std::string& src, std::string& log);
  void loadProgramCache(const std::string& path);
  void saveProgramCache();

  GlThread th_;
  GlFuncs gl_;
  EglFuncs egl_;
  void* libEgl_ = nullptr;
  void* libGles_ = nullptr;
  EGLDisplay dpy_ = EGL_NO_DISPLAY;
  EGLContext ctx_ = EGL_NO_CONTEXT;
  EGLSurface surf_ = EGL_NO_SURFACE;
  DeviceCaps caps_;
  DeviceOptions opt_;
  std::string lastErr_;
  std::atomic<bool> lost_{false};
  std::atomic<std::uint64_t> uid_{1};
  Slot slots_[kSlots];
  int nextSlot_ = 0, pinned_ = -1;
  GLuint nearest_ = 0;
  std::unordered_map<std::string, std::unique_ptr<GPipe>> pipes_;
  std::mutex pipeMu_;
  std::unordered_map<std::string, GLuint> helpers_;
  std::unordered_map<int, ImageH> dummies_;
  std::vector<PassTime> noTimes_;
  double splitGroups_ = 0;
  std::size_t poolBudget_ = 0;
  // program binary cache: key -> (format, bytes)
  struct Blob {
    GLenum fmt;
    std::vector<std::uint8_t> data;
  };
  std::unordered_map<std::string, Blob> blobs_;
  std::string cachePath_, cachePrefix_;
  bool cacheDirty_ = false;
  GLint maxInvocations_ = 0, maxShared_ = 0, uboAlign_ = 256;
};

Status GlesCmd::setParams(const void* p, std::size_t n) {
  if (n > kParamBytes) return err_ = Status::InvalidArgument;
  const std::size_t align = std::max<std::size_t>(d_.caps().uniformOffsetAlign, 16);
  paramOff_ = (paramOff_ + align - 1) / align * align;
  if (paramOff_ + kParamBytes > kRing) {
    d_.fail("parameter ring exhausted", Status::OutOfMemory);
    return err_ = Status::OutOfMemory;
  }
  if (params_.size() < paramOff_ + kParamBytes) params_.resize(paramOff_ + kParamBytes);
  std::memcpy(params_.data() + paramOff_, p, n);
  cmds_.push_back({Op::Params, std::uint32_t(paramOff_)});
  paramOff_ += kParamBytes;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
bool GlesDev::loadLibs(std::string& err) {
#if defined(_WIN32)
  const std::string dir = envStr("DAPPLE_ANGLE_DIR");
  std::string egl = dir.empty() ? "libEGL.dll" : dir + "\\libEGL.dll";
  std::string gles = dir.empty() ? "libGLESv2.dll" : dir + "\\libGLESv2.dll";
  const char* en[] = {egl.c_str(), nullptr};
  const char* gn[] = {gles.c_str(), nullptr};
  if (!dir.empty()) SetDllDirectoryA(dir.c_str());  // vk_swiftshader.dll / vulkan-1.dll next to it
#else
  const char* en[] = {"libEGL.so", "libEGL.so.1", nullptr};
  const char* gn[] = {"libGLESv3.so", "libGLESv2.so", "libGLESv2.so.2", nullptr};
#endif
  libEgl_ = openLib(en);
  if (!libEgl_) return err = "libEGL not found", false;
  libGles_ = openLib(gn);
  if (!libGles_) return err = "libGLES not found", false;
  egl_.getProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(sym(libEgl_, "eglGetProcAddress"));
#define E(field, name) egl_.field = reinterpret_cast<decltype(egl_.field)>(sym(libEgl_, name))
  E(getDisplay, "eglGetDisplay"); E(initialize, "eglInitialize"); E(terminate, "eglTerminate"); E(chooseConfig, "eglChooseConfig");
  E(createContext, "eglCreateContext"); E(destroyContext, "eglDestroyContext"); E(createPbufferSurface, "eglCreatePbufferSurface");
  E(destroySurface, "eglDestroySurface"); E(makeCurrent, "eglMakeCurrent"); E(queryString, "eglQueryString"); E(bindAPI, "eglBindAPI");
  E(getError, "eglGetError");
#undef E
  if (!egl_.getDisplay || !egl_.initialize || !egl_.createContext || !egl_.makeCurrent) return err = "libEGL is missing entry points", false;
  if (egl_.getProcAddress) egl_.getPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(egl_.getProcAddress("eglGetPlatformDisplayEXT"));
  return true;
}

bool GlesDev::makeContext(std::string& err) {
  const std::string backend = envStr("DAPPLE_ANGLE_BACKEND");  // "" | vulkan | swiftshader | d3d11 | gl (desktop only, for ANGLE)
  bool angle = false;
#if defined(_WIN32)
  angle = true;
#endif
  if (angle && egl_.getPlatformDisplayEXT) {
    std::vector<EGLAttrib> a;
    EGLint type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
    if (backend == "d3d11") type = 0x3208;
    else if (backend == "gl") type = 0x320D;
    a = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, type};
    if (backend == "swiftshader") {
      // ANGLE's Vulkan backend on the SwiftShader ICD shipped next to libEGL (loader selects it through the ICD file)
      const std::string dir = envStr("DAPPLE_ANGLE_DIR");
      if (!dir.empty()) {
#if defined(_WIN32)
        _putenv_s("VK_ICD_FILENAMES", (dir + "\\vk_swiftshader_icd.json").c_str());
        _putenv_s("VK_DRIVER_FILES", (dir + "\\vk_swiftshader_icd.json").c_str());
#endif
      }
    }
    a.push_back(EGL_NONE);
    std::vector<EGLint> a32;
    for (EGLAttrib v : a) a32.push_back(EGLint(v));
    dpy_ = egl_.getPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, a32.data());
  }
  if (dpy_ == EGL_NO_DISPLAY) dpy_ = egl_.getDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy_ == EGL_NO_DISPLAY) return err = "eglGetDisplay failed", false;
  EGLint maj = 0, min = 0;
  if (!egl_.initialize(dpy_, &maj, &min)) return err = "eglInitialize failed (EGL error " + std::to_string(egl_.getError()) + ")", false;
  if (egl_.bindAPI) egl_.bindAPI(EGL_OPENGL_ES_API);
  const char* ext = egl_.queryString(dpy_, EGL_EXTENSIONS);
  const bool surfaceless = ext && std::strstr(ext, "EGL_KHR_surfaceless_context");
  EGLConfig cfg = nullptr;
  EGLint n = 0;
  const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, surfaceless ? EGL_DONT_CARE : EGL_PBUFFER_BIT, EGL_NONE};
  if (!egl_.chooseConfig(dpy_, ca, &cfg, 1, &n) || n < 1) {
    const EGLint cb[] = {EGL_NONE};
    if (!egl_.chooseConfig(dpy_, cb, &cfg, 1, &n) || n < 1) return err = "eglChooseConfig found no config", false;
  }
  const EGLint cx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, 0x30FB /*EGL_CONTEXT_MINOR_VERSION*/, 1, EGL_NONE};
  ctx_ = egl_.createContext(dpy_, cfg, EGL_NO_CONTEXT, cx);
  if (ctx_ == EGL_NO_CONTEXT) return err = "eglCreateContext(ES 3.1) failed (EGL error " + std::to_string(egl_.getError()) + ")", false;
  if (!surfaceless) {
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surf_ = egl_.createPbufferSurface(dpy_, cfg, pb);
  }
  return true;
}

bool GlesDev::initGl(const DeviceOptions& opt, std::string& err) {
  // runs on the GL thread
  if (!makeContext(err)) return false;
  if (!egl_.makeCurrent(dpy_, surf_, surf_, ctx_)) return err = "eglMakeCurrent failed", false;
  void* h = libGles_;
#define X(n) \
  gl_.n = reinterpret_cast<decltype(gl_.n)>(sym(h, #n)); \
  if (!gl_.n && egl_.getProcAddress) gl_.n = reinterpret_cast<decltype(gl_.n)>(egl_.getProcAddress(#n)); \
  if (!gl_.n) return err = std::string("missing GL entry point ") + #n, false;
  DAPPLE_GL_FUNCS(X)
#undef X
  const char* ver = reinterpret_cast<const char*>(gl_.glGetString(GL_VERSION));
  int maj = 0, min = 0;
  if (!ver || std::sscanf(ver, "OpenGL ES %d.%d", &maj, &min) != 2) return err = std::string("not an OpenGL ES context: ") + (ver ? ver : "?"), false;
  if (maj < 3 || (maj == 3 && min < 1)) return err = std::string("OpenGL ES 3.1 required, got ") + ver, false;
  queryCaps();
  GLint imgUnits = 0, ssbos = 0;
  gl_.glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &imgUnits);
  gl_.glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &ssbos);
  if (imgUnits < 4 || ssbos < 4) return err = "compute image / storage block limits below the HAL budget", false;
  // blocklist (same text format as every backend)
  std::vector<BlockRule> rules;
  for (const std::string& extra : {opt.blocklist, envStr("DAPPLE_GPU_BLOCKLIST")})
    for (BlockRule& b : parseBlocklist(extra)) rules.push_back(std::move(b));
  const std::string why = blockReason(caps_, rules, opt.allowSoftware);
  if (!why.empty()) return err = "rejected: " + why, false;
  for (auto& s : slots_) {
    s.params.assign(kRing, 0);
    gl_.glGenBuffers(1, &s.ubo);
    gl_.glBindBuffer(GL_UNIFORM_BUFFER, s.ubo);
    gl_.glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(kRing), nullptr, GL_DYNAMIC_DRAW);
  }
  gl_.glGenSamplers(1, &nearest_);
  gl_.glSamplerParameteri(nearest_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl_.glSamplerParameteri(nearest_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl_.glSamplerParameteri(nearest_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_.glSamplerParameteri(nearest_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl_.glSamplerParameteri(nearest_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  if (caps_.programBinaryCache && !opt.pipelineCachePath.empty()) loadProgramCache(opt.pipelineCachePath);
  return true;
}

void GlesDev::queryCaps() {
  DeviceCaps& c = caps_;
  auto str = [&](GLenum e) {
    const char* s = reinterpret_cast<const char*>(gl_.glGetString(e));
    return std::string(s ? s : "");
  };
  c.api = Api::Gles31;
  const std::string ver = str(GL_VERSION), ren = str(GL_RENDERER), ven = str(GL_VENDOR);
  int maj = 3, min = 1;
  std::sscanf(ver.c_str(), "OpenGL ES %d.%d", &maj, &min);
  c.apiVersion = std::uint32_t(maj) << 22 | std::uint32_t(min) << 12;
  c.name = ren;
  c.driver = ver;
  auto has = [&](const char* v) { return ven.find(v) != std::string::npos || ren.find(v) != std::string::npos; };
  c.vendorId = has("NVIDIA") ? 0x10DE : has("Qualcomm") || has("Adreno") ? 0x5143 : has("ARM") || has("Mali") ? 0x13B5 : has("Intel") ? 0x8086
               : has("AMD") || has("ATI") ? 0x1002 : has("Imagination") || has("PowerVR") ? 0x1010 : has("Google") || has("SwiftShader") ? 0x1AE0 : 0;
  // stable 32 bit id of the driver string (the blocklist ranges are textual for GLES)
  std::uint32_t h = 2166136261u;
  for (char ch : ver + "|" + ren) h = (h ^ std::uint8_t(ch)) * 16777619u;
  c.deviceId = h & 0xffff;
  c.driverVersion = h;
  c.software = ren.find("SwiftShader") != std::string::npos || ren.find("llvmpipe") != std::string::npos || ren.find("Software") != std::string::npos ||
               ren.find("softpipe") != std::string::npos;
  GLint v = 0;
  gl_.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
  c.maxImage2D = std::uint32_t(v);
  gl_.glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &v);
  c.maxImage3D = std::uint32_t(v);
  gl_.glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxInvocations_);
  c.maxWorkgroupInvocations = std::uint32_t(maxInvocations_);
  gl_.glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &maxShared_);
  c.maxSharedBytes = std::uint32_t(maxShared_);
  gl_.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlign_);
  c.uniformOffsetAlign = std::uint32_t(std::max(uboAlign_, 16));
  gl_.glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
  c.maxUniformBytes = std::uint32_t(v);
  GLint64 dummy = 0;
  (void)dummy;
  gl_.glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &v);
  c.maxBufferBytes = std::uint32_t(v) ? std::uint64_t(std::uint32_t(v)) : (128ull << 20);
  GLint nb = 0;
  gl_.glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nb);
  c.programBinaryCache = nb > 0;
  c.specConstants = false;
  c.timestamps = false;
  c.unifiedMemory = false;
  std::string exts;
  GLint ne = 0;
  gl_.glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
  for (GLint i = 0; i < ne; i++) exts += std::string(reinterpret_cast<const char*>(gl_.glGetStringi(GL_EXTENSIONS, GLuint(i)))) + " ";
  c.linearFilterRgba32f = exts.find("GL_OES_texture_float_linear") != std::string::npos;
  c.fp16Arithmetic = false;
  c.storage16 = false;
  c.subgroups = false;
  for (int i = 0; i < kFormatCount; i++) {
    const Format f = Format(i);
    FormatCaps& fc = c.format[i];
    fc.sampled = true;
    fc.filterable = f != Format::RGBA32F && f != Format::R32F && f != Format::RG32F && f != Format::R32UI ? true : c.linearFilterRgba32f && f != Format::R32UI;
    fc.storageRead = fc.storageWrite = storageOk(f);
    fc.storageReadWrite = storageRwOk(f);
  }
  // GLES cannot query the heap size. Budget estimate for the planner (resident source limit etc.): half of host RAM, 1..4 GiB.
  // Mobile GLES devices are unified memory; on a desktop ANGLE this is conservative for a discrete GPU.
  {
    const std::uint64_t ram = hostRamBytes();
    caps_.deviceLocalBytes = std::clamp<std::uint64_t>(ram / 2, 1ull << 30, 4ull << 30);
#if defined(__ANDROID__)
    caps_.unifiedMemory = true;
#endif
  }
}

bool GlesDev::init(const DeviceOptions& opt, std::string& err) {
  opt_ = opt;
  splitGroups_ = opt.splitGroups;
  poolBudget_ = opt.poolBudgetBytes;
  cachePath_ = opt.pipelineCachePath;
  if (!loadLibs(err)) return false;
  th_.start();
  bool ok = false;
  th_.run([&] { ok = initGl(opt, err); });
  if (!ok) {
    th_.run([&] {
      if (egl_.makeCurrent) egl_.makeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    });
    th_.stop();
    return false;
  }
  return true;
}

GlesDev::~GlesDev() {
  if (th_.id() != std::thread::id()) {
    th_.run([&] {
      saveProgramCache();
      if (gl_.glFinish) gl_.glFinish();
      if (egl_.makeCurrent) egl_.makeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (surf_ != EGL_NO_SURFACE) egl_.destroySurface(dpy_, surf_);
      if (ctx_ != EGL_NO_CONTEXT) egl_.destroyContext(dpy_, ctx_);
      if (dpy_ != EGL_NO_DISPLAY) egl_.terminate(dpy_);
    });
    th_.stop();
  }
}

// ---------------------------------------------------------------------------------------------------------------------
Status GlesDev::createBuffer(const BufferDesc& d, BufferH* out) {
  if (lost_) return Status::DeviceLost;
  auto* b = new GBuffer;
  b->size = d.bytes;
  b->kind = d.kind;
  b->uid = uid_++;
  Status st = Status::Ok;
  th_.run([&] {
    gl_.glGenBuffers(1, &b->id);
    gl_.glBindBuffer(GL_SHADER_STORAGE_BUFFER, b->id);
    gl_.glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(d.bytes), nullptr, GL_DYNAMIC_COPY);
    const GLenum e = gl_.glGetError();
    if (e == GL_OUT_OF_MEMORY) st = Status::OutOfMemory;
    else if (e != GL_NO_ERROR) st = Status::Internal;
  });
  if (st != Status::Ok) {
    th_.run([&] { gl_.glDeleteBuffers(1, &b->id); });
    delete b;
    return st;
  }
  if (d.kind != MemKind::DeviceLocal) {
    b->shadow = static_cast<std::uint8_t*>(std::calloc(1, std::size_t(d.bytes)));
    if (!b->shadow) {
      th_.run([&] { gl_.glDeleteBuffers(1, &b->id); });
      delete b;
      return Status::OutOfMemory;
    }
  }
  out->id = b->uid;
  out->impl = b;
  out->bytes = d.bytes;
  out->mapped = b->shadow;
  return Status::Ok;
}

Status GlesDev::createImage(const ImageDesc& d, ImageH* out) {
  if (lost_) return Status::DeviceLost;
  if (d.w > caps_.maxImage2D || d.h > caps_.maxImage2D) return Status::Unsupported;
  auto* im = new GImage;
  im->fmt = d.fmt;
  im->w = d.w;
  im->h = d.h;
  im->d = d.d;
  im->internal = internalFormat(d.fmt);
  im->target = d.d > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
  im->uid = uid_++;
  Status st = Status::Ok;
  th_.run([&] {
    gl_.glGenTextures(1, &im->tex);
    gl_.glBindTexture(im->target, im->tex);
    if (d.d > 1) gl_.glTexStorage3D(GL_TEXTURE_3D, 1, im->internal, GLsizei(d.w), GLsizei(d.h), GLsizei(d.d));
    else gl_.glTexStorage2D(GL_TEXTURE_2D, 1, im->internal, GLsizei(d.w), GLsizei(d.h));
    const GLenum e = gl_.glGetError();
    if (e == GL_OUT_OF_MEMORY) st = Status::OutOfMemory;
    else if (e != GL_NO_ERROR) st = Status::Unsupported;
  });
  if (st != Status::Ok) {
    th_.run([&] { gl_.glDeleteTextures(1, &im->tex); });
    delete im;
    return st;
  }
  out->id = im->uid;
  out->impl = im;
  out->fmt = d.fmt;
  out->w = d.w;
  out->h = d.h;
  out->d = d.d;
  out->bytes = std::uint64_t(d.w) * d.h * d.d * bytesPerTexel(d.fmt);
  return Status::Ok;
}

void GlesDev::destroy(BufferH& b) {
  auto* g = static_cast<GBuffer*>(b.impl);
  if (!g) return;
  th_.run([&] { gl_.glDeleteBuffers(1, &g->id); });
  std::free(g->shadow);
  delete g;
  b = BufferH{};
}
void GlesDev::destroy(ImageH& i) {
  auto* g = static_cast<GImage*>(i.impl);
  if (!g) return;
  th_.run([&] { gl_.glDeleteTextures(1, &g->tex); });
  delete g;
  i = ImageH{};
}
ImageH GlesDev::dummyImage(Format f) {
  auto it = dummies_.find(int(f));
  if (it != dummies_.end()) return it->second;
  ImageH h;
  ImageDesc d;
  d.fmt = f;
  d.w = d.h = 1;
  createImage(d, &h);
  dummies_[int(f)] = h;
  return h;
}

void GlesDev::flush(BufferH b, std::uint64_t off, std::uint64_t n) {
  auto* g = static_cast<GBuffer*>(b.impl);
  if (!g || !g->shadow) return;
  th_.run([&] {
    gl_.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->id);
    gl_.glBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(off), GLsizeiptr(n), g->shadow + off);
  });
}
void GlesDev::syncBuffer(GBuffer* g) {
  if (!g->shadow || !g->gpuWritten) return;
  gl_.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->id);
  void* p = gl_.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(g->size), GL_MAP_READ_BIT);
  if (p) {
    std::memcpy(g->shadow, p, std::size_t(g->size));
    gl_.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  }
  g->gpuWritten = false;
}
void GlesDev::invalidate(BufferH b, std::uint64_t, std::uint64_t) {
  auto* g = static_cast<GBuffer*>(b.impl);
  if (!g) return;
  th_.run([&] { syncBuffer(g); });
}

// ---------------------------------------------------------------------------------------------------------------------
GLuint GlesDev::buildProgram(const std::string& src, std::string& log) {
  GLuint sh = gl_.glCreateShader(GL_COMPUTE_SHADER);
  const char* s = src.c_str();
  gl_.glShaderSource(sh, 1, &s, nullptr);
  gl_.glCompileShader(sh);
  GLint ok = 0;
  gl_.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char b[2048] = {};
    gl_.glGetShaderInfoLog(sh, sizeof b - 1, nullptr, b);
    log = std::string("compile: ") + b;
    gl_.glDeleteShader(sh);
    return 0;
  }
  GLuint p = gl_.glCreateProgram();
  gl_.glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  gl_.glAttachShader(p, sh);
  gl_.glLinkProgram(p);
  gl_.glDetachShader(p, sh);
  gl_.glDeleteShader(sh);
  gl_.glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char b[2048] = {};
    gl_.glGetProgramInfoLog(p, sizeof b - 1, nullptr, b);
    log = std::string("link: ") + b;
    gl_.glDeleteProgram(p);
    return 0;
  }
  return p;
}

GLuint GlesDev::helperProgram(const std::string& key, const std::string& src) {
  auto it = helpers_.find(key);
  if (it != helpers_.end()) return it->second;
  std::string log;
  const GLuint p = buildProgram(src, log);
  if (!p) fail("helper program " + key + ": " + log, Status::Internal);
  helpers_[key] = p;
  return p;
}

Status GlesDev::createPipeline(const PipelineDesc& pd, PipelineH* out) {
  if (lost_) return Status::DeviceLost;
  const GlesShaderBin* bin = nullptr;
  for (std::size_t i = 0; i < kShaderCount; i++)
    if (pd.shader == kShaders[i].name) bin = &kShaders[i];
  if (!bin) return fail("unknown shader " + pd.shader, Status::Unsupported), Status::Unsupported;
  const std::size_t sigLen = std::strlen(bin->sig);
  bool alias = false;
  if (sigLen >= 2 && bin->sig[sigLen - 1] == 'S' && std::strchr(bin->sig, 'I') && bin->pingpong) alias = true;
  if (!pd.bindings.empty() && pd.bindings != bin->sig && !(alias && pd.bindings == std::string(bin->sig, sigLen - 1))) return fail("binding signature mismatch for " + pd.shader, Status::InvalidArgument), Status::InvalidArgument;
  // effective local size: spec-driven (sepconv) or fixed; ES guarantees only 128 invocations
  std::int32_t loc[3] = {bin->local[0], bin->local[1], bin->local[2]};
  for (int a = 0; a < 2; a++)
    if (bin->localSpec[a] >= 0) {
      for (int k = 0; k < bin->nspec; k++)
        if (bin->specId[k] == bin->localSpec[a]) loc[a] = bin->specDef[k];
      for (auto& s : pd.spec)
        if (std::int32_t(s.id) == bin->localSpec[a]) loc[a] = s.value;
    }
  if (loc[0] * loc[1] * loc[2] > maxInvocations_ || std::uint32_t(bin->sharedBytes) > std::uint32_t(maxShared_))
    return fail(pd.shader + ": workgroup " + std::to_string(loc[0] * loc[1] * loc[2]) + " invocations / " + std::to_string(bin->sharedBytes) +
                    " B shared exceed this device (" + std::to_string(maxInvocations_) + " / " + std::to_string(maxShared_) + ")",
                Status::Unsupported),
           Status::Unsupported;
  std::string key = pd.shader;
  std::vector<SpecConst> sp = pd.spec;
  std::sort(sp.begin(), sp.end(), [](const SpecConst& a, const SpecConst& b) { return a.id < b.id; });
  for (auto& s : sp) key += "|" + std::to_string(s.id) + "=" + std::to_string(s.value);
  {
    std::lock_guard<std::mutex> l(pipeMu_);
    auto it = pipes_.find(key);
    if (it != pipes_.end()) return out->impl = it->second.get(), out->bindings = it->second->callerSig.c_str(), Status::Ok;
  }
  Status st = Status::Ok;
  GPipe* made = nullptr;
  th_.run([&] {
    // splice the specialisation #defines after the #version line
    std::string src = bin->essl;
    const std::size_t nl = src.find('\n');
    std::string defs;
    for (auto& s : sp) {
      bool isBool = false;
      for (int k = 0; k < bin->nspec; k++)
        if (std::uint32_t(bin->specId[k]) == s.id) isBool = bin->specBool[k] != 0;
      defs += "#define SPIRV_CROSS_CONSTANT_ID_" + std::to_string(s.id) + " " + (isBool ? (s.value ? "true" : "false") : std::to_string(s.value)) + "\n";
    }
    src.insert(nl + 1, defs);
    GLuint prog = 0;
    const std::string bk = cachePrefix_ + key;
    auto bit = blobs_.find(bk);
    if (bit != blobs_.end()) {
      prog = gl_.glCreateProgram();
      gl_.glProgramBinary(prog, bit->second.fmt, bit->second.data.data(), GLsizei(bit->second.data.size()));
      GLint ok = 0;
      gl_.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) {
        gl_.glDeleteProgram(prog);
        prog = 0;
        blobs_.erase(bit);
      }
    }
    if (!prog) {
      std::string log;
      prog = buildProgram(src, log);
      if (!prog) {
        fail(pd.shader + ": " + log, Status::Unsupported);
        st = Status::Unsupported;
        return;
      }
      if (caps_.programBinaryCache) {
        GLint len = 0;
        gl_.glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
        if (len > 0) {
          Blob b;
          b.data.resize(std::size_t(len));
          GLsizei got = 0;
          gl_.glGetProgramBinary(prog, len, &got, &b.fmt, b.data.data());
          b.data.resize(std::size_t(got));
          blobs_[bk] = std::move(b);
          cacheDirty_ = true;
        }
      }
    }
    made = new GPipe;
    made->prog = prog;
    made->bin = bin;
    made->key = key;
    made->callerSig.assign(bin->sig, std::strlen(bin->sig) - (alias ? 1u : 0u));
    made->nSlots = std::uint32_t(std::strlen(bin->sig)) - (alias ? 1u : 0u);
    made->aliasRead = alias;
    made->aliasFrom = alias ? std::uint32_t(std::strchr(bin->sig, 'I') - bin->sig) : 0u;
  });
  if (st != Status::Ok) return st;
  std::lock_guard<std::mutex> l(pipeMu_);
  pipes_[key].reset(made);
  out->impl = made;
  out->bindings = made->callerSig.c_str();
  return Status::Ok;
}

void GlesDev::loadProgramCache(const std::string& path) {
  // prefix ties blobs to the driver + shader set: a driver or app update invalidates silently
  cachePrefix_ = caps_.name + "|" + caps_.driver + "|" + kShaderSetHash + "|";
  std::ifstream f(path, std::ios::binary);
  if (!f) return;
  auto rd32 = [&](std::uint32_t& v) { return bool(f.read(reinterpret_cast<char*>(&v), 4)); };
  std::uint32_t magic = 0, n = 0;
  if (!rd32(magic) || magic != 0x474C5042u || !rd32(n)) return;
  for (std::uint32_t i = 0; i < n; i++) {
    std::uint32_t kl = 0, fm = 0, dl = 0;
    if (!rd32(kl) || kl > 4096) return;
    std::string k(kl, '\0');
    f.read(&k[0], kl);
    if (!rd32(fm) || !rd32(dl) || dl > (64u << 20)) return;
    Blob b;
    b.fmt = fm;
    b.data.resize(dl);
    f.read(reinterpret_cast<char*>(b.data.data()), dl);
    if (!f) return;
    if (k.compare(0, cachePrefix_.size(), cachePrefix_) == 0) blobs_[k] = std::move(b);
  }
}
void GlesDev::saveProgramCache() {
  if (cachePath_.empty() || !cacheDirty_) return;
  std::ofstream f(cachePath_ + ".tmp", std::ios::binary);
  if (!f) return;
  auto wr32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  wr32(0x474C5042u);
  wr32(std::uint32_t(blobs_.size()));
  for (auto& kv : blobs_) {
    wr32(std::uint32_t(kv.first.size()));
    f.write(kv.first.data(), std::streamsize(kv.first.size()));
    wr32(kv.second.fmt);
    wr32(std::uint32_t(kv.second.data.size()));
    f.write(reinterpret_cast<const char*>(kv.second.data.data()), std::streamsize(kv.second.data.size()));
  }
  f.close();
  std::remove(cachePath_.c_str());
  std::rename((cachePath_ + ".tmp").c_str(), cachePath_.c_str());
  cacheDirty_ = false;
}
std::vector<std::uint8_t> GlesDev::pipelineCacheBlob() const { return {}; }  // persisted by path (saveProgramCache)
void GlesDev::trim() {
  th_.run([&] {
    for (auto& kv : helpers_) gl_.glDeleteProgram(kv.second);
    helpers_.clear();
    saveProgramCache();
  });
}

// ---------------------------------------------------------------------------------------------------------------------
std::unique_ptr<CommandList> GlesDev::begin() {
  if (lost_) return nullptr;
  int idx = nextSlot_;
  nextSlot_ = (nextSlot_ + 1) % kSlots;
  if (idx == pinned_) {
    idx = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
  }
  Slot& s = slots_[idx];
  if (s.submitted.load() != s.done.load()) {
    FenceH f;
    f.slot = std::uint32_t(idx);
    f.gen = s.submitted.load();
    if (wait(f, 30ull * 1000 * 1000 * 1000) != Status::Ok) return nullptr;
  }
  return std::make_unique<GlesCmd>(*this, idx);
}

Status GlesDev::submit(CommandList& cl, FenceH* signal) {
  if (lost_) return Status::DeviceLost;
  auto& c = static_cast<GlesCmd&>(cl);
  Slot& s = slots_[c.slot_];
  const std::uint64_t gen = s.submitted.load() + 1;
  s.submitted.store(gen);
  {
    SubmitStats st;
    st.parts = 1;
    for (const Cmd& k : c.cmds_) {
      switch (k.op) {
        case Op::Dispatch: case Op::B2I: case Op::B2B: case Op::I2B: case Op::I2I: st.passes++; break;
        case Op::Barrier: if (k.a) st.barriers++; break;
        case Op::Flush: st.parts++; break;
        default: break;
      }
    }
    s.stats = st;
  }
  auto cmds = std::make_shared<std::vector<Cmd>>(std::move(c.cmds_));
  auto params = std::make_shared<std::vector<std::uint8_t>>(std::move(c.params_));
  const int slot = c.slot_;
  th_.post([this, slot, gen, cmds, params] {
    Slot& sl = slots_[slot];
    sl.params.swap(*params);
    if (sl.params.size() < kRing) sl.params.resize(kRing);
    exec(sl, *cmds);
    std::lock_guard<std::mutex> l(sl.m);
    sl.done.store(gen);
    sl.cv.notify_all();
  });
  if (signal) {
    signal->slot = std::uint32_t(c.slot_);
    signal->gen = gen;
  }
  return Status::Ok;
}

// a fence is complete when the slot's GL thread task ran AND the GPU finished (glFenceSync created inside exec)
Status GlesDev::wait(FenceH f, std::uint64_t timeoutNs) {
  if (!f) return Status::Ok;
  Slot& s = slots_[f.slot];
  {
    std::unique_lock<std::mutex> l(s.m);
    if (!s.cv.wait_for(l, std::chrono::nanoseconds(timeoutNs), [&] { return s.done.load() >= f.gen; })) return Status::Timeout;
  }
  Status st = Status::Ok;
  th_.run([&] {
    if (s.sync && s.submitted.load() == f.gen) {
      const GLenum r = gl_.glClientWaitSync(s.sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeoutNs);
      if (r == GL_TIMEOUT_EXPIRED) st = Status::Timeout;
      else if (r == GL_WAIT_FAILED) st = Status::DeviceLost;
      else {
        gl_.glDeleteSync(s.sync);
        s.sync = nullptr;
      }
    }
    if (st == Status::Ok) {
      for (GBuffer* b : s.touched) syncBuffer(b);
      s.touched.clear();
      const GLenum e = gl_.glGetError();
      if (e == GL_OUT_OF_MEMORY) st = Status::OutOfMemory;
    }
  });
  if (st == Status::DeviceLost) lost_ = true;
  return st;
}
bool GlesDev::poll(FenceH f) {
  if (!f) return true;
  Slot& s = slots_[f.slot];
  if (s.done.load() < f.gen) return false;
  bool ready = true;
  th_.run([&] {
    if (s.sync && s.submitted.load() == f.gen) ready = gl_.glClientWaitSync(s.sync, 0, 0) != GL_TIMEOUT_EXPIRED;
  });
  return ready;
}
Status GlesDev::waitAll() {
  for (int i = 0; i < kSlots; i++) {
    FenceH f;
    f.slot = std::uint32_t(i);
    f.gen = slots_[i].submitted.load();
    if (f.gen) {
      const Status st = wait(f, 60ull * 1000 * 1000 * 1000);
      if (st != Status::Ok) return st;
    }
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
const char* kCopyB2B = R"(#version 310 es
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer S { uint s[]; };
layout(std430, binding = 1) writeonly buffer D { uint d[]; };
uniform uvec3 uP;  // source word offset, word count, destination word offset
void main() {
  uint i = gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * 64u) + gl_GlobalInvocationID.x;
  if (i < uP.y) d[uP.z + i] = s[uP.x + i];
}
)";

const char* kCopyI2B = R"(#version 310 es
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform highp SAMPLER uSrc;
layout(std430, binding = 0) buffer O { uint o[]; };
uniform ivec4 uR;   // x, y, w, h
uniform uint uOff;  // output offset in 32 bit words
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  BODY
}
)";

std::string copyI2BSource(Format f) {
  std::string body, sampler = "sampler2D";
  switch (f) {
    case Format::RGBA32F:
      body = "if (p.x >= uR.z || p.y >= uR.w) return; vec4 v = texelFetch(uSrc, p + uR.xy, 0); uint i = uOff + uint(p.y * uR.z + p.x) * 4u;"
             " o[i] = floatBitsToUint(v.x); o[i+1u] = floatBitsToUint(v.y); o[i+2u] = floatBitsToUint(v.z); o[i+3u] = floatBitsToUint(v.w);";
      break;
    case Format::RG32F:
      body = "if (p.x >= uR.z || p.y >= uR.w) return; vec4 v = texelFetch(uSrc, p + uR.xy, 0); uint i = uOff + uint(p.y * uR.z + p.x) * 2u;"
             " o[i] = floatBitsToUint(v.x); o[i+1u] = floatBitsToUint(v.y);";
      break;
    case Format::R32F:
      body = "if (p.x >= uR.z || p.y >= uR.w) return; o[uOff + uint(p.y * uR.z + p.x)] = floatBitsToUint(texelFetch(uSrc, p + uR.xy, 0).x);";
      break;
    case Format::R32UI:
      sampler = "usampler2D";
      body = "if (p.x >= uR.z || p.y >= uR.w) return; o[uOff + uint(p.y * uR.z + p.x)] = texelFetch(uSrc, p + uR.xy, 0).x;";
      break;
    case Format::RGBA16F:
      body = "if (p.x >= uR.z || p.y >= uR.w) return; vec4 v = texelFetch(uSrc, p + uR.xy, 0); uint i = uOff + uint(p.y * uR.z + p.x) * 2u;"
             " o[i] = packHalf2x16(v.xy); o[i+1u] = packHalf2x16(v.zw);";
      break;
    case Format::RGBA8:
      body = "if (p.x >= uR.z || p.y >= uR.w) return; o[uOff + uint(p.y * uR.z + p.x)] = packUnorm4x8(texelFetch(uSrc, p + uR.xy, 0));";
      break;
    case Format::R8:  // 4 texels per thread along x
      body = "p.x *= 4; if (p.x >= uR.z || p.y >= uR.w) return; uint w = 0u;"
             " for (int k = 0; k < 4; k++) { int x = p.x + k; if (x < uR.z) w |= uint(clamp(texelFetch(uSrc, ivec2(x, p.y) + uR.xy, 0).x * 255.0 + 0.5, 0.0, 255.0)) << uint(8 * k); }"
             " o[uOff + uint(p.y * ((uR.z + 3) / 4) + p.x / 4)] = w;";
      break;
    case Format::R16F:  // 2 texels per thread along x
      body = "p.x *= 2; if (p.x >= uR.z || p.y >= uR.w) return; float a = texelFetch(uSrc, ivec2(p.x, p.y) + uR.xy, 0).x;"
             " float b = p.x + 1 < uR.z ? texelFetch(uSrc, ivec2(p.x + 1, p.y) + uR.xy, 0).x : 0.0;"
             " o[uOff + uint(p.y * ((uR.z + 1) / 2) + p.x / 2)] = packHalf2x16(vec2(a, b));";
      break;
    default: break;
  }
  std::string s = kCopyI2B;
  auto rep = [&](const char* k, const std::string& v) { s.replace(s.find(k), std::strlen(k), v); };
  rep("SAMPLER", sampler);
  rep("BODY", body);
  return s;
}

std::string copyI2ISource(Format f) {
  const bool u = f == Format::R32UI;
  std::string s = "#version 310 es\nlayout(local_size_x = 8, local_size_y = 8) in;\n";
  s += std::string("layout(binding = 0) uniform highp ") + (u ? "usampler2D" : "sampler2D") + " uSrc;\n";
  s += std::string("layout(binding = 0, ") + layoutName(f) + ") uniform highp writeonly " + (u ? "uimage2D" : "image2D") + " uDst;\n";
  s += "uniform ivec2 uSize;\nvoid main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); if (p.x >= uSize.x || p.y >= uSize.y) return;\n";
  s += "  imageStore(uDst, p, texelFetch(uSrc, p, 0)); }\n";
  return s;
}

void GlesDev::exec(Slot& s, std::vector<Cmd>& cmds) {
  static const bool debugErrors = envStr("DAPPLE_GLES_DEBUG")[0] == '1';
  gl_.glBindBuffer(GL_UNIFORM_BUFFER, s.ubo);
  // upload the used part of the parameter ring in one piece
  std::uint32_t used = 0;
  for (const Cmd& c : cmds)
    if (c.op == Op::Params) used = std::max(used, c.a + kParamBytes);
  if (used) gl_.glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(used), s.params.data());
  const GLbitfield kAllBits = GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                              GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT |
                              GL_PIXEL_BUFFER_BARRIER_BIT | GL_COMMAND_BARRIER_BIT;
  gl_.glMemoryBarrier(kAllBits);  // list boundary: previous lists' writes visible
  // Upload buffers are host shadows (no persistent mapping in ES 3.1). The HAL contract of the Vulkan backend (host-coherent memory)
  // lets callers write through map() without flush(), so every Upload buffer the list reads is re-sent in full here.
  // TODO(perf): track the written range (callers know it) or glMapBufferRange(UNSYNCHRONIZED) for big staging buffers.
  {
    std::vector<GBuffer*> ups;
    auto note = [&](void* p) {
      auto* b = static_cast<GBuffer*>(p);
      if (b && b->kind == MemKind::Upload && b->shadow && std::find(ups.begin(), ups.end(), b) == ups.end()) ups.push_back(b);
    };
    for (const Cmd& c : cmds)
      if (c.op == Op::BindBuf || c.op == Op::B2I || c.op == Op::B2B) note(c.p);
    for (GBuffer* b : ups) {
      gl_.glBindBuffer(GL_SHADER_STORAGE_BUFFER, b->id);
      gl_.glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(b->size), b->shadow);
    }
    if (!ups.empty()) gl_.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
  }
  if (debugErrors)
    while (const GLenum ge = gl_.glGetError()) std::fprintf(stderr, "dapple gles: GL error 0x%x in list prologue (param / upload buffers)\n", ge);
  GPipe* pipe = nullptr;
  void* bres[12] = {};
  bool isImg[12] = {};
  std::uint32_t curParam = 0;
  bool haveParam = false;
  for (const Cmd& c : cmds) {
    switch (c.op) {
      case Op::BindPipe:
        pipe = static_cast<GPipe*>(c.p);
        gl_.glUseProgram(pipe ? pipe->prog : 0);
        std::memset(bres, 0, sizeof bres);
        break;
      case Op::BindBuf:
        if (c.a < 12) bres[c.a] = c.p, isImg[c.a] = false;
        break;
      case Op::BindImg:
        if (c.a < 12) bres[c.a] = c.p, isImg[c.a] = true;
        break;
      case Op::Params:
        curParam = c.a;
        haveParam = true;
        break;
      case Op::Dispatch: {
        if (!pipe) break;
        if (debugErrors) {
          std::string miss;
          for (std::uint32_t i = 0; i < pipe->nSlots; i++)
            if (!bres[i]) miss += std::to_string(i) + " ";
          std::fprintf(stderr, "dispatch %s %ux%ux%u unbound slots: %s\n", pipe->key.c_str(), c.a, c.b, c.c, miss.c_str());
        }
        if (haveParam) gl_.glBindBufferRange(GL_UNIFORM_BUFFER, 0, s.ubo, GLintptr(curParam), kParamBytes);
        for (std::uint32_t i = 0; i < pipe->nSlots; i++) {
          const char k = pipe->bin->sig[i];
          const GLuint unit = GLuint(pipe->bin->glb[i]);
          if (!bres[i]) continue;
          if (k == 'B') {
            auto* b = static_cast<GBuffer*>(bres[i]);
            gl_.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, unit, b->id);
          } else if (k == 'I') {
            auto* im = static_cast<GImage*>(bres[i]);
            gl_.glBindImageTexture(unit, im->tex, 0, im->target == GL_TEXTURE_3D ? GL_TRUE : GL_FALSE, 0, GL_READ_WRITE, im->internal);
          } else {
            auto* im = static_cast<GImage*>(bres[i]);
            gl_.glActiveTexture(GL_TEXTURE0 + unit);
            gl_.glBindTexture(im->target, im->tex);
            gl_.glBindSampler(unit, nearest_);
          }
        }
        if (pipe->aliasRead && bres[pipe->aliasFrom]) {
          auto* im = static_cast<GImage*>(bres[pipe->aliasFrom]);
          const GLuint unit = GLuint(pipe->bin->glb[pipe->nSlots]);
          gl_.glActiveTexture(GL_TEXTURE0 + unit);
          gl_.glBindTexture(im->target, im->tex);
          gl_.glBindSampler(unit, nearest_);
        }
        gl_.glDispatchCompute(c.a, c.b, c.c);
        break;
      }
      case Op::Barrier: {
        GLbitfield m = 0;
        if (c.a & (kImgStoreToImgLoad)) m |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        if (c.a & kImgStoreToSampled) m |= GL_TEXTURE_FETCH_BARRIER_BIT;
        if (c.a & kBufWriteToBufRead) m |= GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
        if (c.a & (kComputeToTransfer | kTransferToCompute)) m |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT;
        if (c.a & kComputeToHost) m |= GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
        if (m) gl_.glMemoryBarrier(m);
        break;
      }
      case Op::B2I: {
        auto* b = static_cast<GBuffer*>(c.p);
        auto* im = static_cast<GImage*>(c.q);
        GLenum fmt, type;
        uploadFormat(im->fmt, fmt, type);
        gl_.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b->id);
        gl_.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl_.glBindTexture(im->target, im->tex);
        const void* off = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(c.u));
        if (im->target == GL_TEXTURE_3D) gl_.glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, GLsizei(c.a), GLsizei(c.b), GLsizei(c.c), fmt, type, off);
        else gl_.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(c.a), GLsizei(c.b), fmt, type, off);
        gl_.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl_.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        break;
      }
      case Op::B2B: {
        auto* sb = static_cast<GBuffer*>(c.p);
        auto* db = static_cast<GBuffer*>(c.q);
        if (sb->shadow && sb->kind == MemKind::Upload) {
          // host shadow -> device buffer directly (ANGLE rejected glCopyBufferSubData out of a freshly specified buffer with
          // GL_INVALID_VALUE; this also saves the extra copy)
          gl_.glBindBuffer(GL_COPY_WRITE_BUFFER, db->id);
          gl_.glBufferSubData(GL_COPY_WRITE_BUFFER, 0, GLsizeiptr(c.u), sb->shadow + c.e);
        } else {
          if (c.u % 4 == 0 && c.e % 4 == 0) {
            // ANGLE (Vulkan) fails glCopyBufferSubData with GL_INVALID_VALUE when the source was just written by glBufferSubData
            // (staged update), so buffer -> buffer copies run as a compute pass (4 byte words).
            const GLuint prog = helperProgram("b2b", kCopyB2B);
            if (prog) {
              const std::uint32_t words = std::uint32_t(c.u / 4), groups = (words + 63) / 64, gx = std::min<std::uint32_t>(groups, 65535u),
                                  gy = (groups + gx - 1) / gx;
              gl_.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
              gl_.glUseProgram(prog);
              gl_.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sb->id);
              gl_.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, db->id);
              gl_.glUniform3ui(gl_.glGetUniformLocation(prog, "uP"), c.e / 4, words, 0);
              gl_.glDispatchCompute(gx, gy, 1);
              pipe = nullptr;
            }
          } else {
            gl_.glBindBuffer(GL_COPY_READ_BUFFER, sb->id);
            gl_.glBindBuffer(GL_COPY_WRITE_BUFFER, db->id);
            gl_.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GLintptr(c.e), 0, GLsizeiptr(c.u));
          }
        }
        gl_.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        if (db->shadow && db->kind == MemKind::Readback) {  // host-visible destination: wait() maps it back into the shadow
          db->gpuWritten = true;
          if (std::find(s.touched.begin(), s.touched.end(), db) == s.touched.end()) s.touched.push_back(db);
        }
        break;
      }
      case Op::I2B: {
        auto* im = static_cast<GImage*>(c.p);
        auto* b = static_cast<GBuffer*>(c.q);
        const GLuint prog = helperProgram(std::string("i2b") + std::to_string(int(im->fmt)), copyI2BSource(im->fmt));
        if (!prog) break;
        gl_.glUseProgram(prog);
        gl_.glActiveTexture(GL_TEXTURE0);
        gl_.glBindTexture(GL_TEXTURE_2D, im->tex);
        gl_.glBindSampler(0, nearest_);
        gl_.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b->id);
        gl_.glUniform4i(gl_.glGetUniformLocation(prog, "uR"), GLint(c.a), GLint(c.b), GLint(c.c), GLint(c.d));
        gl_.glUniform1ui(gl_.glGetUniformLocation(prog, "uOff"), GLuint(c.u / 4));
        std::uint32_t tw = c.c, th = c.d;
        if (im->fmt == Format::R8) tw = (c.c + 3) / 4;
        else if (im->fmt == Format::R16F) tw = (c.c + 1) / 2;
        gl_.glDispatchCompute((tw + 7) / 8, (th + 7) / 8, 1);
        gl_.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        if (b->shadow) {
          b->gpuWritten = true;
          if (std::find(s.touched.begin(), s.touched.end(), b) == s.touched.end()) s.touched.push_back(b);
        }
        pipe = nullptr;
        break;
      }
      case Op::I2I: {
        auto* a = static_cast<GImage*>(c.p);
        auto* b = static_cast<GImage*>(c.q);
        if (!layoutName(b->fmt) || a->fmt != b->fmt) { fail("copyImage: unsupported format", Status::Unsupported); break; }
        const GLuint prog = helperProgram(std::string("i2i") + std::to_string(int(a->fmt)), copyI2ISource(a->fmt));
        if (!prog) break;
        gl_.glUseProgram(prog);
        gl_.glActiveTexture(GL_TEXTURE0);
        gl_.glBindTexture(GL_TEXTURE_2D, a->tex);
        gl_.glBindSampler(0, nearest_);
        gl_.glBindImageTexture(0, b->tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, b->internal);
        gl_.glUniform2i(gl_.glGetUniformLocation(prog, "uSize"), GLint(c.a), GLint(c.b));
        gl_.glDispatchCompute((c.a + 7) / 8, (c.b + 7) / 8, 1);
        gl_.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        pipe = nullptr;
        break;
      }
      case Op::Flush: gl_.glFlush(); break;
    }
    if (debugErrors) {  // DAPPLE_GLES_DEBUG=1: attribute GL errors to the recorded op
      const GLenum ge = gl_.glGetError();
      if (ge) std::fprintf(stderr, "dapple gles: GL error 0x%x after op %d (shader %s)\n", ge, int(c.op), pipe ? pipe->key.c_str() : "-");
    }
  }
  gl_.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
  if (s.sync) gl_.glDeleteSync(s.sync);
  s.sync = gl_.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  gl_.glFlush();
  const GLenum e = gl_.glGetError();
  if (e == GL_OUT_OF_MEMORY) fail("GL_OUT_OF_MEMORY during submission", Status::OutOfMemory);
  else if (e == 0x0507 /*GL_CONTEXT_LOST*/) fail("GL context lost", Status::DeviceLost);
}

}  // namespace

std::unique_ptr<Device> createGlesDevice(const DeviceOptions& opt, std::string& err) {
  auto d = std::make_unique<GlesDev>();
  if (!d->init(opt, err)) return nullptr;
  return d;
}

}  // namespace dapple::hal::gles

namespace dapple::hal::gles {
std::vector<std::string> shaderNames() {
  std::vector<std::string> v;
  for (std::size_t i = 0; i < kShaderCount; i++) v.push_back(kShaders[i].name);
  return v;
}
const char* shaderSetHash() { return kShaderSetHash; }
}  // namespace dapple::hal::gles
