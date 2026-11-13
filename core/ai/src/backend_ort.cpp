// ONNX Runtime backend through the C API (no exceptions). CPU execution provider (MLAS: NEON / dot-product kernels): the XNNPACK EP was
// measured slower on the ViT models and fails to create on Depth Anything's Resize, so it is not used. Compiled only with DAPPLE_AI_ORT.
#include "backend.h"

#ifdef DAPPLE_AI_ORT
#include <cstring>

#include "affinity.h"
#include "onnxruntime_c_api.h"

namespace dapple::ai {
namespace {

const OrtApi* api() {
  static const OrtApi* a = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  return a;
}

std::string take(OrtStatus* s) {
  if (!s) return {};
  std::string m = api()->GetErrorMessage(s);
  api()->ReleaseStatus(s);
  return m.empty() ? "error" : m;
}

bool toDType(ONNXTensorElementDataType t, DType& d) {
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: d = DType::F32; return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: d = DType::U8; return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: d = DType::I32; return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: d = DType::I64; return true;
    default: return false;
  }
}
ONNXTensorElementDataType fromDType(DType d) {
  switch (d) {
    case DType::F32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case DType::U8: return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    case DType::I32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    default: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
  }
}

OrtEnv* env() {
  static OrtEnv* e = [] {
    OrtEnv* p = nullptr;
    take(api()->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "dapple-ai", &p));
    return p;
  }();
  return e;
}

class OrtSession_ final : public Session {
 public:
  ~OrtSession_() override {
    if (ro_) api()->ReleaseRunOptions(ro_);
    if (s_) api()->ReleaseSession(s_);
    if (alloc_info_) api()->ReleaseMemoryInfo(alloc_info_);
  }
  bool init(const SessionArgs& a, Status& st) {
    cpus_ = a.cpus;
    OrtSessionOptions* so = nullptr;
    if (auto e = take(api()->CreateSessionOptions(&so)); !e.empty()) { st = {Code::LoadFailed, e}; return false; }
    api()->SetIntraOpNumThreads(so, a.threads);
    api()->SetInterOpNumThreads(so, 1);
    api()->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
    api()->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    std::string e;
    {
      ScopedAffinity pin(a.cpus);  // the intra-op pool threads are created with the session and inherit the placement
      if (!a.path.empty()) {
#ifdef _WIN32
        std::wstring w(a.path.begin(), a.path.end());
        e = take(api()->CreateSession(env(), w.c_str(), so, &s_));
#else
        e = take(api()->CreateSession(env(), a.path.c_str(), so, &s_));
#endif
      } else {
        e = take(api()->CreateSessionFromArray(env(), a.data, a.size, so, &s_));
      }
    }
    api()->ReleaseSessionOptions(so);
    if (!e.empty()) { st = {Code::LoadFailed, e}; return false; }
    if (auto e2 = take(api()->CreateRunOptions(&ro_)); !e2.empty()) { st = {Code::LoadFailed, e2}; return false; }
    // return the CPU arena to the OS after every run: idle memory is what the app pays for between edits
    api()->AddRunConfigEntry(ro_, "memory.enable_memory_arena_shrinkage", "cpu:0");
    take(api()->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &alloc_info_));
    OrtAllocator* al = nullptr;
    api()->GetAllocatorWithDefaultOptions(&al);
    size_t n = 0;
    api()->SessionGetInputCount(s_, &n);
    for (size_t i = 0; i < n; ++i) { char* nm = nullptr; api()->SessionGetInputName(s_, i, al, &nm); in_names_.push_back(nm); al->Free(al, nm); }
    api()->SessionGetOutputCount(s_, &n);
    for (size_t i = 0; i < n; ++i) { char* nm = nullptr; api()->SessionGetOutputName(s_, i, al, &nm); out_names_.push_back(nm); al->Free(al, nm); }
    return true;
  }
  const char* runtime() const override { return "ort"; }
  std::vector<TensorSpec> inputs() const override { return specs(true); }
  std::vector<TensorSpec> outputs() const override { return specs(false); }

  Status run(const std::vector<TensorView>& in, std::vector<Tensor>& out, const std::atomic<bool>* cancel, const std::vector<int>& want) override {
    if (in.size() != in_names_.size()) return {Code::BadInput, "expected " + std::to_string(in_names_.size()) + " inputs"};
    std::vector<OrtValue*> vals(in.size(), nullptr);
    struct Cleanup {
      std::vector<OrtValue*>& v;
      ~Cleanup() { for (auto* p : v) if (p) api()->ReleaseValue(p); }
    } cl{vals};
    for (size_t i = 0; i < in.size(); ++i) {
      size_t n = 1;
      for (auto d : in[i].dims) n *= size_t(d < 1 ? 1 : d);
      if (!in[i].data) return {Code::BadInput, "null input"};
      if (auto e = take(api()->CreateTensorWithDataAsOrtValue(alloc_info_, const_cast<void*>(in[i].data), n * dtypeSize(in[i].type), in[i].dims.data(), in[i].dims.size(), fromDType(in[i].type), &vals[i])); !e.empty())
        return {Code::BadInput, e};
    }
    std::vector<const char*> inN, outN;
    for (auto& s : in_names_) inN.push_back(s.c_str());
    if (want.empty()) { for (auto& s : out_names_) outN.push_back(s.c_str()); }
    else for (int k : want) {
      if (k < 0 || size_t(k) >= out_names_.size()) return {Code::BadInput, "bad output index"};
      outN.push_back(out_names_[size_t(k)].c_str());
    }
    std::vector<OrtValue*> res(outN.size(), nullptr);
    std::string e;
    {
      ScopedAffinity pin(cpus_);
      api()->RunOptionsUnsetTerminate(ro_);
      CancelWatcher w(cancel, [this]() { api()->RunOptionsSetTerminate(ro_); });
      e = take(api()->Run(s_, ro_, inN.data(), vals.data(), vals.size(), outN.data(), outN.size(), res.data()));
    }
    struct Cleanup2 {
      std::vector<OrtValue*>& v;
      ~Cleanup2() { for (auto* p : v) if (p) api()->ReleaseValue(p); }
    } cl2{res};
    if (cancel && cancel->load()) return {Code::Cancelled, "cancelled"};
    if (!e.empty()) return {Code::RunFailed, e};
    out.assign(res.size(), Tensor{});
    for (size_t i = 0; i < res.size(); ++i) {
      OrtTensorTypeAndShapeInfo* ti = nullptr;
      take(api()->GetTensorTypeAndShape(res[i], &ti));
      ONNXTensorElementDataType et;
      api()->GetTensorElementType(ti, &et);
      size_t nd = 0;
      api()->GetDimensionsCount(ti, &nd);
      out[i].dims.resize(nd);
      api()->GetDimensions(ti, out[i].dims.data(), nd);
      api()->ReleaseTensorTypeAndShapeInfo(ti);
      if (!toDType(et, out[i].type)) return {Code::RunFailed, "unsupported output type"};
      size_t cnt = 1;
      for (auto d : out[i].dims) cnt *= size_t(d);
      void* p = nullptr;
      take(api()->GetTensorMutableData(res[i], &p));
      out[i].bytes.resize(cnt * dtypeSize(out[i].type));
      std::memcpy(out[i].bytes.data(), p, out[i].bytes.size());
    }
    return {};
  }

 private:
  std::vector<TensorSpec> specs(bool in) const {
    std::vector<TensorSpec> r;
    size_t n = 0;
    in ? api()->SessionGetInputCount(s_, &n) : api()->SessionGetOutputCount(s_, &n);
    for (size_t i = 0; i < n; ++i) {
      TensorSpec s;
      s.name = in ? in_names_[i] : out_names_[i];
      OrtTypeInfo* info = nullptr;
      if (take(in ? api()->SessionGetInputTypeInfo(s_, i, &info) : api()->SessionGetOutputTypeInfo(s_, i, &info)).empty() && info) {
        const OrtTensorTypeAndShapeInfo* ti = nullptr;
        api()->CastTypeInfoToTensorInfo(info, &ti);
        if (ti) {
          ONNXTensorElementDataType et;
          api()->GetTensorElementType(ti, &et);
          if (!toDType(et, s.type)) s.type = DType::F32;
          size_t nd = 0;
          api()->GetDimensionsCount(ti, &nd);
          s.dims.resize(nd);
          api()->GetDimensions(ti, s.dims.data(), nd);
        }
        api()->ReleaseTypeInfo(info);
      }
      r.push_back(std::move(s));
    }
    return r;
  }

  OrtSession* s_ = nullptr;
  OrtRunOptions* ro_ = nullptr;
  OrtMemoryInfo* alloc_info_ = nullptr;
  std::vector<std::string> in_names_, out_names_;
  std::vector<int> cpus_;
};

}  // namespace

std::unique_ptr<Session> makeOrtSession(const SessionArgs& a, Status& st) {
  auto s = std::make_unique<OrtSession_>();
  if (!s->init(a, st)) return nullptr;
  return s;
}

}  // namespace dapple::ai
#else
namespace dapple::ai {
std::unique_ptr<Session> makeOrtSession(const SessionArgs&, Status& st) {
  st = {Code::NoBackend, "built without ONNX Runtime (DAPPLE_AI_ORT)"};
  return nullptr;
}
}  // namespace dapple::ai
#endif
