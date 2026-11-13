// ONNX Runtime C++ API benchmark: load, first inference, steady latency, peak RSS, outputs to files.
// --mode cpu | xnn   (xnn = XNNPACK EP with CPU EP fallback)
#include <vector>
#include "bench_common.h"
#include "onnxruntime_cxx_api.h"

static size_t elemSize(ONNXTensorElementDataType t) {
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return 8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return 1;
    default: return 4;
  }
}

int main(int argc, char** argv) {
  bench::Args a;
  if (!a.parse(argc, argv)) return 2;
  std::printf("pin=%d ", bench::pin(a.cpus));
  try {
    const long rss0 = bench::procKb("VmRSS");
    const double t0 = bench::nowMs();
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "bench");
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(a.threads);
    so.SetInterOpNumThreads(1);
    so.SetExecutionMode(ORT_SEQUENTIAL);
    so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    if (a.mode == "xnn") {
      std::unordered_map<std::string, std::string> o{{"intra_op_num_threads", std::to_string(a.threads)}};
      so.AppendExecutionProvider("XNNPACK", o);
    }
    Ort::Session s(env, a.model.c_str(), so);
    const double loadMs = bench::nowMs() - t0;
    const long rssLoad = bench::procKb("VmRSS");
    Ort::AllocatorWithDefaultOptions alloc;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> ins;
    std::vector<std::vector<char>> bufs;
    std::vector<std::string> inNames, outNames;
    std::string io;
    for (size_t i = 0; i < s.GetInputCount(); ++i) {
      inNames.push_back(s.GetInputNameAllocated(i, alloc).get());
      auto tinfo = s.GetInputTypeInfo(i);
      auto ti = tinfo.GetTensorTypeAndShapeInfo();
      std::vector<int64_t> dims = ti.GetShape();
      for (auto& sh : a.shapes) if (std::atoi(sh.c_str()) == int(i)) dims = bench::parseDims(sh.substr(sh.find('=') + 1));
      size_t n = 1;
      for (auto d : dims) n *= size_t(d < 1 ? 1 : d);
      bufs.emplace_back(n * elemSize(ti.GetElementType()), 0);
      if (i < a.in.size()) {
        auto f = bench::readFile(a.in[i]);
        if (f.size() != bufs.back().size()) { std::printf("{\"error\":\"input %zu size %zu != %zu\"}\n", i, f.size(), bufs.back().size()); return 1; }
        bufs.back() = f;
      }
      ins.push_back(Ort::Value::CreateTensor(mem, bufs.back().data(), bufs.back().size(), dims.data(), dims.size(), ti.GetElementType()));
      io += "in" + std::to_string(i) + ":" + inNames.back() + "/";
      for (size_t k = 0; k < dims.size(); ++k) io += (k ? "x" : "") + std::to_string(dims[k]);
      io += " ";
    }
    for (size_t i = 0; i < s.GetOutputCount(); ++i) outNames.push_back(s.GetOutputNameAllocated(i, alloc).get());
    std::vector<const char*> inP, outP;
    for (auto& n : inNames) inP.push_back(n.c_str());
    for (auto& n : outNames) outP.push_back(n.c_str());
    std::vector<double> ts;
    double first = 0;
    std::vector<Ort::Value> outs;
    for (int r = 0; r < a.runs + 1; ++r) {
      const double st = bench::nowMs();
      outs = s.Run(Ort::RunOptions{nullptr}, inP.data(), ins.data(), ins.size(), outP.data(), outP.size());
      const double e = bench::nowMs() - st;
      if (r == 0) first = e; else ts.push_back(e);
    }
    for (size_t i = 0; i < outs.size(); ++i) {
      auto info = outs[i].GetTensorTypeAndShapeInfo();
      io += "out" + std::to_string(i) + ":";
      auto sh = info.GetShape();
      for (size_t k = 0; k < sh.size(); ++k) io += (k ? "x" : "") + std::to_string(sh[k]);
      io += " ";
      if (!a.out.empty()) bench::writeFile(a.out + "_" + std::to_string(i) + ".bin", outs[i].GetTensorRawData(), info.GetElementCount() * elemSize(info.GetElementType()));
    }
    std::printf("{\"rt\":\"ort\",\"mode\":\"%s\",\"threads\":%d,\"load_ms\":%.1f,\"first_ms\":%.1f,\"median_ms\":%.2f,\"min_ms\":%.2f,"
                "\"rss_load_mb\":%.1f,\"rss_delta_mb\":%.1f,\"peak_rss_mb\":%.1f,\"io\":\"%s\"}\n",
                a.mode.c_str(), a.threads, loadMs, first, bench::median(ts), *std::min_element(ts.begin(), ts.end()),
                rssLoad / 1024.0, (rssLoad - rss0) / 1024.0, bench::procKb("VmHWM") / 1024.0, io.c_str());
  } catch (const Ort::Exception& e) {
    std::printf("{\"error\":\"%s\"}\n", e.what());
    return 1;
  }
  return 0;
}
