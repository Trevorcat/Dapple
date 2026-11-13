// Model registry: lazy loading, backend dispatch by file extension, idle release.
#include "ai/ai.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>

#include "affinity.h"
#include "backend.h"

namespace dapple::ai {
namespace {
using Clock = std::chrono::steady_clock;

struct Registry {
  std::mutex mu;
  Config cfg;
  std::vector<std::weak_ptr<Model>> models;
};
Registry& reg() {
  static Registry* r = new Registry;  // never destroyed: sessions may outlive static destruction order
  return *r;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
}  // namespace

struct Model::Impl {
  std::string id;
  std::string path;              // resolved or explicit
  std::string ext;               // ".tflite" ...
  std::vector<uint8_t> memory;   // memory model bytes
  bool fromMemory = false;
  bool lazyId = false;           // path is resolved from the model directory at load time
  std::mutex mu;                 // serializes load / run / release on this model
  std::unique_ptr<Session> sess;
  int sessThreads = 0;
  std::size_t bytes = 0;
  Clock::time_point last = Clock::now();
  std::string runtimeName;
};

Model::Model() : impl_(new Impl) {}
Model::~Model() = default;

namespace {
std::shared_ptr<Model> track(std::shared_ptr<Model> m) {
  auto& r = reg();
  std::lock_guard<std::mutex> l(r.mu);
  r.models.erase(std::remove_if(r.models.begin(), r.models.end(), [](const auto& w) { return w.expired(); }), r.models.end());
  r.models.push_back(m);
  return m;
}
}  // namespace

std::shared_ptr<Model> Model::open(const std::string& id) {
  std::shared_ptr<Model> m(new Model);
  m->impl_->id = id;
  m->impl_->lazyId = true;
  return track(m);
}
std::shared_ptr<Model> Model::openFile(const std::string& path) {
  std::shared_ptr<Model> m(new Model);
  m->impl_->path = path;
  m->impl_->id = std::filesystem::path(path).stem().string();
  m->impl_->ext = lower(std::filesystem::path(path).extension().string());
  return track(m);
}
std::shared_ptr<Model> Model::openMemory(const void* data, std::size_t size, const std::string& ext) {
  std::shared_ptr<Model> m(new Model);
  m->impl_->fromMemory = true;
  m->impl_->id = "memory";
  m->impl_->ext = lower(ext);
  m->impl_->memory.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  return track(m);
}

const std::string& Model::id() const { return impl_->id; }
bool Model::loaded() const { return impl_->sess != nullptr; }
std::string Model::runtime() const { return impl_->runtimeName; }

namespace {
// lock held by the caller
Status ensure(Model::Impl& m, int threads) {
  if (threads <= 0) threads = defaultThreads();
  if (m.sess && m.sessThreads == threads) return {};
  m.sess.reset();  // a different thread count rebuilds the session (pools are fixed at creation)
  SessionArgs a;
  a.threads = threads;
  a.cpus = inferenceCpus();
  if (m.fromMemory) {
    a.data = m.memory.data();
    a.size = m.memory.size();
  } else {
    if (m.lazyId) {
      const std::string dir = config().modelDir;
      std::string found;
      for (const char* e : {".tflite", ".ort", ".onnx"}) {
        const auto p = std::filesystem::path(dir) / (m.id + e);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) { found = p.string(); m.ext = e; break; }
      }
      if (found.empty()) return {Code::NoModel, "model '" + m.id + "' not found in '" + dir + "'"};
      m.path = found;
    }
    a.path = m.path;
    std::error_code ec;
    m.bytes = std::size_t(std::filesystem::file_size(m.path, ec));
    if (ec) return {Code::NoModel, "cannot read " + m.path};
  }
  if (m.fromMemory) m.bytes = m.memory.size();
  Status st;
  if (m.ext == ".tflite") m.sess = makeLiteRtSession(a, st);
  else if (m.ext == ".ort" || m.ext == ".onnx") m.sess = makeOrtSession(a, st);
  else return {Code::LoadFailed, "unknown model extension '" + m.ext + "'"};
  if (!m.sess) return st.ok() ? Status{Code::LoadFailed, "load failed"} : st;
  m.sessThreads = threads;
  m.runtimeName = m.sess->runtime();
  return {};
}
}  // namespace

Status Model::load() {
  std::lock_guard<std::mutex> l(impl_->mu);
  impl_->last = Clock::now();
  return ensure(*impl_, impl_->sessThreads);
}
Status Model::inputs(std::vector<TensorSpec>& out) {
  Status st = load();
  if (!st) return st;
  std::lock_guard<std::mutex> l(impl_->mu);
  if (!impl_->sess) return {Code::NoModel, "released"};
  out = impl_->sess->inputs();
  return {};
}
Status Model::outputs(std::vector<TensorSpec>& out) {
  Status st = load();
  if (!st) return st;
  std::lock_guard<std::mutex> l(impl_->mu);
  if (!impl_->sess) return {Code::NoModel, "released"};
  out = impl_->sess->outputs();
  return {};
}

Status run(Model& m, const std::vector<TensorView>& in, std::vector<Tensor>& out, const RunOptions& opt) {
  Model::Impl& i = *m.impl();
  std::lock_guard<std::mutex> l(i.mu);
  if (opt.cancel && opt.cancel->load()) return {Code::Cancelled, "cancelled"};
  Status st = ensure(i, opt.threads);
  if (!st) return st;
  i.last = Clock::now();
  st = i.sess->run(in, out, opt.cancel, opt.outputs);
  i.last = Clock::now();
  return st;
}

void setConfig(const Config& c) {
  auto& r = reg();
  std::lock_guard<std::mutex> l(r.mu);
  r.cfg = c;
}
Config config() {
  auto& r = reg();
  std::lock_guard<std::mutex> l(r.mu);
  return r.cfg;
}

namespace {
int releaseWhere(bool all) {
  const int idleMs = config().idleReleaseMs;
  std::vector<std::shared_ptr<Model>> live;
  {
    auto& r = reg();
    std::lock_guard<std::mutex> l(r.mu);
    for (auto& w : r.models)
      if (auto p = w.lock()) live.push_back(p);
  }
  int n = 0;
  const auto now = Clock::now();
  for (auto& m : live) {
    std::unique_lock<std::mutex> l(m->impl()->mu, std::try_to_lock);  // a model that is running is not idle
    if (!l.owns_lock() || !m->impl()->sess) continue;
    const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - m->impl()->last).count();
    if (all || idle >= idleMs) {
      m->impl()->sess.reset();
      m->impl()->sessThreads = 0;
      ++n;
    }
  }
  return n;
}
}  // namespace
int releaseIdle() { return releaseWhere(false); }
int releaseAll() { return releaseWhere(true); }

std::size_t residentBytes() {
  std::vector<std::shared_ptr<Model>> live;
  {
    auto& r = reg();
    std::lock_guard<std::mutex> l(r.mu);
    for (auto& w : r.models)
      if (auto p = w.lock()) live.push_back(p);
  }
  std::size_t n = 0;
  for (auto& m : live) {
    std::unique_lock<std::mutex> l(m->impl()->mu, std::try_to_lock);
    if (l.owns_lock() && m->impl()->sess) n += m->impl()->bytes;
  }
  return n;
}

}  // namespace dapple::ai
