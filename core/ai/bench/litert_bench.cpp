// LiteRT / TFLite C API benchmark: load, first inference, steady latency, peak RSS, outputs to files.
#include <dlfcn.h>
#include <vector>
#include "bench_common.h"
#include "tflite/c/c_api.h"
#include "tflite/delegates/gpu/delegate.h"

int main(int argc, char** argv) {
  bench::Args a;
  if (!a.parse(argc, argv)) return 2;
  std::printf("pin=%d ", bench::pin(a.cpus));
  const long rss0 = bench::procKb("VmRSS");
  const double t0 = bench::nowMs();
  TfLiteModel* model = TfLiteModelCreateFromFile(a.model.c_str());
  if (!model) { std::printf("{\"error\":\"model load failed\"}\n"); return 1; }
  TfLiteInterpreterOptions* opt = TfLiteInterpreterOptionsCreate();
  TfLiteInterpreterOptionsSetNumThreads(opt, a.threads);
  TfLiteDelegate* del = nullptr;
  if (a.mode == "gpu") {
    TfLiteGpuDelegateOptionsV2 go = TfLiteGpuDelegateOptionsV2Default();
    go.inference_preference = TFLITE_GPU_INFERENCE_PREFERENCE_SUSTAINED_SPEED;
    del = TfLiteGpuDelegateV2Create(&go);
    if (!del) { std::printf("{\"error\":\"gpu delegate create failed\"}\n"); return 1; }
    TfLiteInterpreterOptionsAddDelegate(opt, del);
  }  // "default": the interpreter applies XNNPACK itself with num_threads
  TfLiteInterpreter* it = TfLiteInterpreterCreate(model, opt);
  if (!it) { std::printf("{\"error\":\"interpreter create failed (custom op?)\"}\n"); return 1; }
  if (TfLiteInterpreterAllocateTensors(it) != kTfLiteOk) { std::printf("{\"error\":\"allocate failed\"}\n"); return 1; }
  const double loadMs = bench::nowMs() - t0;
  const long rssLoad = bench::procKb("VmRSS");
  const int nin = TfLiteInterpreterGetInputTensorCount(it), nout = TfLiteInterpreterGetOutputTensorCount(it);
  std::string shapes;
  for (int i = 0; i < nin; ++i) {
    TfLiteTensor* t = TfLiteInterpreterGetInputTensor(it, i);
    std::vector<char> d(TfLiteTensorByteSize(t), 0);
    if (i < int(a.in.size())) {
      auto f = bench::readFile(a.in[size_t(i)]);
      if (f.size() != d.size()) { std::printf("{\"error\":\"input %d size %zu != %zu\"}\n", i, f.size(), d.size()); return 1; }
      d = f;
    }
    TfLiteTensorCopyFromBuffer(t, d.data(), d.size());
    shapes += "in" + std::to_string(i) + ":";
    for (int k = 0; k < TfLiteTensorNumDims(t); ++k) shapes += (k ? "x" : "") + std::to_string(TfLiteTensorDim(t, k));
    shapes += "/t" + std::to_string(int(TfLiteTensorType(t))) + " ";
  }
  std::vector<double> ts;
  double first = 0;
  for (int r = 0; r < a.runs + 1; ++r) {
    const double s = bench::nowMs();
    if (TfLiteInterpreterInvoke(it) != kTfLiteOk) { std::printf("{\"error\":\"invoke failed\"}\n"); return 1; }
    const double e = bench::nowMs() - s;
    if (r == 0) first = e; else ts.push_back(e);
  }
  for (int i = 0; i < nout; ++i) {
    const TfLiteTensor* t = TfLiteInterpreterGetOutputTensor(it, i);
    shapes += "out" + std::to_string(i) + ":";
    for (int k = 0; k < TfLiteTensorNumDims(t); ++k) shapes += (k ? "x" : "") + std::to_string(TfLiteTensorDim(t, k));
    shapes += "/t" + std::to_string(int(TfLiteTensorType(t))) + " ";
    if (!a.out.empty()) bench::writeFile(a.out + "_" + std::to_string(i) + ".bin", TfLiteTensorData(t), TfLiteTensorByteSize(t));
  }
  std::printf("{\"rt\":\"litert\",\"mode\":\"%s\",\"threads\":%d,\"load_ms\":%.1f,\"first_ms\":%.1f,\"median_ms\":%.2f,\"min_ms\":%.2f,"
              "\"rss_load_mb\":%.1f,\"rss_delta_mb\":%.1f,\"peak_rss_mb\":%.1f,\"io\":\"%s\"}\n",
              a.mode.c_str(), a.threads, loadMs, first, bench::median(ts), *std::min_element(ts.begin(), ts.end()),
              rssLoad / 1024.0, (rssLoad - rss0) / 1024.0, bench::procKb("VmHWM") / 1024.0, shapes.c_str());
  return 0;
}
