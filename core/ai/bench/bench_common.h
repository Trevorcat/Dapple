#pragma once
// Shared helpers of the on-device inference benchmarks (research tooling, not part of the library).
#include <sched.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bench {
inline double nowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// kB from /proc/self/status ("VmRSS", "VmHWM").
inline long procKb(const char* key) {
  std::ifstream f("/proc/self/status");
  std::string l;
  while (std::getline(f, l))
    if (l.rfind(key, 0) == 0) return std::atol(l.c_str() + std::strlen(key) + 1);
  return -1;
}
inline bool pin(const std::string& cpus) {
  if (cpus.empty()) return true;
  cpu_set_t s;
  CPU_ZERO(&s);
  size_t p = 0;
  while (p < cpus.size()) {
    size_t q = cpus.find(',', p);
    if (q == std::string::npos) q = cpus.size();
    CPU_SET(std::atoi(cpus.substr(p, q - p).c_str()), &s);
    p = q + 1;
  }
  return sched_setaffinity(0, sizeof s, &s) == 0;
}
inline std::vector<char> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline void writeFile(const std::string& p, const void* d, size_t n) {
  std::ofstream f(p, std::ios::binary);
  f.write(static_cast<const char*>(d), std::streamsize(n));
}
struct Args {
  std::string model, cpus, out, mode = "default";
  std::vector<std::string> in, shapes;  // shapes: "idx=1x3x4"
  int threads = 1, runs = 10, warm = 1;
  bool parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
      if (a == "--model") model = nx();
      else if (a == "--threads") threads = std::atoi(nx().c_str());
      else if (a == "--runs") runs = std::atoi(nx().c_str());
      else if (a == "--cpus") cpus = nx();
      else if (a == "--out") out = nx();
      else if (a == "--mode") mode = nx();
      else if (a == "--in") in.push_back(nx());
      else if (a == "--shape") shapes.push_back(nx());
      else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return false; }
    }
    return !model.empty();
  }
};
inline std::vector<int64_t> parseDims(const std::string& s) {
  std::vector<int64_t> d;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find('x', p);
    if (q == std::string::npos) q = s.size();
    d.push_back(std::atoll(s.substr(p, q - p).c_str()));
    p = q + 1;
  }
  return d;
}
inline double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}
}  // namespace bench
