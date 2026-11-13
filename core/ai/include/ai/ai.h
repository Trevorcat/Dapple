#pragma once
// Dapple on-device inference: a tiny runtime-agnostic interface. The engine includes this header (and tasks.h) only; which runtime
// (LiteRT / ONNX Runtime) executes a model is decided by the model file extension and by what the build links.
//
//   ai::setModelDir(dir);                        // the app tells where model files are
//   auto m = ai::Model::open("face_detector");   // lazy: nothing is read until the first run
//   ai::run(*m, inputs, outputs, {0, &flag});    // threads 0 = performance cores; flag set -> Code::Cancelled
//   ai::releaseIdle();                           // engine housekeeping tick: frees sessions unused for idleReleaseMs
//
// Threading: a Model can be used from several threads, runs on one Model are serialized. Failure is reported by Status, never by
// exception. No global state besides the registry below.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dapple::ai {

enum class DType : uint8_t { F32, U8, I32, I64 };
inline std::size_t dtypeSize(DType t) { return t == DType::U8 ? 1 : t == DType::I64 ? 8 : 4; }

struct TensorSpec {
  std::string name;
  DType type = DType::F32;
  std::vector<int64_t> dims;  // -1 = dynamic
  int64_t count() const {
    int64_t n = 1;
    for (auto d : dims) n *= d < 1 ? 1 : d;
    return n;
  }
};

// Non-owning input view. dims must be fully specified (a dynamic model dimension is bound by the view).
struct TensorView {
  DType type = DType::F32;
  std::vector<int64_t> dims;
  const void* data = nullptr;
};

// Owning output.
struct Tensor {
  DType type = DType::F32;
  std::vector<int64_t> dims;
  std::vector<uint8_t> bytes;
  int64_t count() const { return int64_t(bytes.size() / dtypeSize(type)); }
  const float* f32() const { return reinterpret_cast<const float*>(bytes.data()); }
  float* f32() { return reinterpret_cast<float*>(bytes.data()); }
};

enum class Code { Ok, NoModel, NoBackend, LoadFailed, BadInput, RunFailed, Cancelled };
struct Status {
  Code code = Code::Ok;
  std::string message;
  bool ok() const { return code == Code::Ok; }
  explicit operator bool() const { return ok(); }
};

struct RunOptions {
  int threads = 0;                            // 0 = automatic: performance cores of this device (sysperf poolCpus), at most 4
  const std::atomic<bool>* cancel = nullptr;  // polled while running; set it to abandon the run (Code::Cancelled)
  std::vector<int> outputs;                   // indices of the outputs wanted (empty = all). ONNX Runtime then skips the nodes only the others need
};

struct Config {
  std::string modelDir;       // where `<id>.tflite|.ort|.onnx` files live
  int idleReleaseMs = 30000;  // releaseIdle(): sessions unused for this long are destroyed (the Model handle stays usable)
};
void setConfig(const Config&);
Config config();
inline void setModelDir(const std::string& d) {
  Config c = config();
  c.modelDir = d;
  setConfig(c);
}

// Frees the sessions (weights, scratch, thread pools) of every model idle for longer than Config::idleReleaseMs. Cheap; call from a
// housekeeping tick. Returns the number released. releaseAll() ignores the idle time (memory-pressure callback).
int releaseIdle();
int releaseAll();
// Bytes of live sessions (best effort: model file sizes).
std::size_t residentBytes();

class Model {
 public:
  // Lazy handle to a model in the model directory: `id` is the file stem; the first of <id>.tflite, <id>.ort, <id>.onnx that exists is used.
  static std::shared_ptr<Model> open(const std::string& id);
  // Explicit file path (the extension picks the runtime).
  static std::shared_ptr<Model> openFile(const std::string& path);
  // Memory model; `ext` is ".tflite" / ".ort" / ".onnx". The bytes are copied.
  static std::shared_ptr<Model> openMemory(const void* data, std::size_t size, const std::string& ext);
  ~Model();

  Status load();  // forces loading now (otherwise done by the first run)
  bool loaded() const;
  const std::string& id() const;
  std::string runtime() const;  // "litert" / "ort" once loaded, else ""
  Status inputs(std::vector<TensorSpec>& out);  // loads if needed
  Status outputs(std::vector<TensorSpec>& out);

  struct Impl;
  Impl* impl() { return impl_.get(); }

 private:
  Model();
  std::unique_ptr<Impl> impl_;
};

Status run(Model& m, const std::vector<TensorView>& in, std::vector<Tensor>& out, const RunOptions& opt = {});

}  // namespace dapple::ai
