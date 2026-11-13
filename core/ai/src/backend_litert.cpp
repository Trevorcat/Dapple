// LiteRT (TFLite) backend through the stable C API (libtensorflowlite_jni.so of the official LiteRT 1.4 AAR on Android, the TFLite C library
// elsewhere). The interpreter applies XNNPACK to float models by itself. Compiled only with DAPPLE_AI_LITERT.
#include "backend.h"

#ifdef DAPPLE_AI_LITERT
#include <cstring>

#include "affinity.h"
#include "tflite/c/c_api.h"

namespace dapple::ai {
namespace {

bool toDType(TfLiteType t, DType& d) {
  switch (t) {
    case kTfLiteFloat32: d = DType::F32; return true;
    case kTfLiteUInt8: d = DType::U8; return true;
    case kTfLiteInt32: d = DType::I32; return true;
    case kTfLiteInt64: d = DType::I64; return true;
    default: return false;
  }
}

std::vector<TensorSpec> specs(TfLiteInterpreter* it, bool in) {
  std::vector<TensorSpec> r;
  const int n = in ? TfLiteInterpreterGetInputTensorCount(it) : TfLiteInterpreterGetOutputTensorCount(it);
  for (int i = 0; i < n; ++i) {
    const TfLiteTensor* t = in ? TfLiteInterpreterGetInputTensor(it, i) : TfLiteInterpreterGetOutputTensor(it, i);
    TensorSpec s;
    s.name = TfLiteTensorName(t) ? TfLiteTensorName(t) : "";
    if (!toDType(TfLiteTensorType(t), s.type)) s.type = DType::F32;
    for (int k = 0; k < TfLiteTensorNumDims(t); ++k) s.dims.push_back(TfLiteTensorDim(t, k));
    r.push_back(std::move(s));
  }
  return r;
}

class LiteRtSession final : public Session {
 public:
  ~LiteRtSession() override {
    if (it_) TfLiteInterpreterDelete(it_);
    if (model_) TfLiteModelDelete(model_);
  }
  bool init(const SessionArgs& a, Status& st) {
    cpus_ = a.cpus;
    ScopedAffinity pin(a.cpus);  // the XNNPACK thread pool is created below and inherits the placement
    if (!a.path.empty()) model_ = TfLiteModelCreateFromFile(a.path.c_str());
    else { blob_.assign(static_cast<const uint8_t*>(a.data), static_cast<const uint8_t*>(a.data) + a.size); model_ = TfLiteModelCreate(blob_.data(), blob_.size()); }
    if (!model_) { st = {Code::LoadFailed, "TfLiteModelCreate failed"}; return false; }
    TfLiteInterpreterOptions* opt = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(opt, a.threads);
    TfLiteInterpreterOptionsEnableCancellation(opt, true);
    it_ = TfLiteInterpreterCreate(model_, opt);
    TfLiteInterpreterOptionsDelete(opt);
    if (!it_) { st = {Code::LoadFailed, "TfLiteInterpreterCreate failed (unsupported or custom op?)"}; return false; }
    if (TfLiteInterpreterAllocateTensors(it_) != kTfLiteOk) { st = {Code::LoadFailed, "AllocateTensors failed"}; return false; }
    return true;
  }
  const char* runtime() const override { return "litert"; }
  std::vector<TensorSpec> inputs() const override { return specs(it_, true); }
  std::vector<TensorSpec> outputs() const override { return specs(it_, false); }

  Status run(const std::vector<TensorView>& in, std::vector<Tensor>& out, const std::atomic<bool>* cancel, const std::vector<int>& want) override {
    const int n = TfLiteInterpreterGetInputTensorCount(it_);
    if (int(in.size()) != n) return {Code::BadInput, "expected " + std::to_string(n) + " inputs"};
    bool resized = false;
    for (int i = 0; i < n; ++i) {
      TfLiteTensor* t = TfLiteInterpreterGetInputTensor(it_, i);
      bool same = int(in[size_t(i)].dims.size()) == TfLiteTensorNumDims(t);
      for (int k = 0; same && k < TfLiteTensorNumDims(t); ++k) same = in[size_t(i)].dims[size_t(k)] == TfLiteTensorDim(t, k);
      if (!same) {
        std::vector<int> d(in[size_t(i)].dims.begin(), in[size_t(i)].dims.end());
        if (TfLiteInterpreterResizeInputTensor(it_, i, d.data(), int(d.size())) != kTfLiteOk) return {Code::BadInput, "cannot resize input " + std::to_string(i)};
        resized = true;
      }
    }
    if (resized && TfLiteInterpreterAllocateTensors(it_) != kTfLiteOk) return {Code::RunFailed, "AllocateTensors failed after resize"};
    for (int i = 0; i < n; ++i) {
      TfLiteTensor* t = TfLiteInterpreterGetInputTensor(it_, i);
      DType d;
      if (!toDType(TfLiteTensorType(t), d) || d != in[size_t(i)].type) return {Code::BadInput, "input " + std::to_string(i) + ": dtype mismatch"};
      if (TfLiteTensorCopyFromBuffer(t, in[size_t(i)].data, TfLiteTensorByteSize(t)) != kTfLiteOk) return {Code::BadInput, "copy input failed"};
    }
    TfLiteStatus s;
    {
      ScopedAffinity pin(cpus_);  // the calling thread is one of the XNNPACK workers
      CancelWatcher w(cancel, [this]() { TfLiteInterpreterCancel(it_); });
      s = TfLiteInterpreterInvoke(it_);
    }
    if (cancel && cancel->load()) return {Code::Cancelled, "cancelled"};
    if (s != kTfLiteOk) return {Code::RunFailed, "TfLiteInterpreterInvoke failed"};
    const int total = TfLiteInterpreterGetOutputTensorCount(it_);
    std::vector<int> idx = want;
    if (idx.empty()) for (int i = 0; i < total; ++i) idx.push_back(i);
    out.assign(idx.size(), Tensor{});
    for (size_t j = 0; j < idx.size(); ++j) {
      if (idx[j] < 0 || idx[j] >= total) return {Code::BadInput, "bad output index"};
      const TfLiteTensor* t = TfLiteInterpreterGetOutputTensor(it_, idx[j]);
      Tensor& o = out[j];
      if (!toDType(TfLiteTensorType(t), o.type)) return {Code::RunFailed, "unsupported output type"};
      for (int k = 0; k < TfLiteTensorNumDims(t); ++k) o.dims.push_back(TfLiteTensorDim(t, k));
      o.bytes.resize(TfLiteTensorByteSize(t));
      std::memcpy(o.bytes.data(), TfLiteTensorData(t), o.bytes.size());
    }
    return {};
  }

 private:
  TfLiteModel* model_ = nullptr;
  TfLiteInterpreter* it_ = nullptr;
  std::vector<uint8_t> blob_;
  std::vector<int> cpus_;
};

}  // namespace

std::unique_ptr<Session> makeLiteRtSession(const SessionArgs& a, Status& st) {
  auto s = std::make_unique<LiteRtSession>();
  if (!s->init(a, st)) return nullptr;
  return s;
}

}  // namespace dapple::ai
#else
namespace dapple::ai {
std::unique_ptr<Session> makeLiteRtSession(const SessionArgs&, Status& st) {
  st = {Code::NoBackend, "built without LiteRT (DAPPLE_AI_LITERT)"};
  return nullptr;
}
}  // namespace dapple::ai
#endif
