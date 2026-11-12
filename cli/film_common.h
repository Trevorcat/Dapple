#pragma once
// Helpers shared by the film command-line tools (film_cmds.cpp, film_bench.cpp).
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dapple/film.h"

namespace dapple::filmtool {

inline std::filesystem::path P(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> kv;
  bool has(const char* k) const { return kv.count(k) != 0; }
  std::string get(const char* k, const std::string& d = "") const {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
};
// Flags without a value.
inline bool isSwitch(const std::string& k) { return k == "--no-spatial" || k == "--timings" || k == "--check"; }
inline Args parse(const std::vector<std::string>& a) {
  Args r;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].rfind("--", 0) == 0) {
      if (isSwitch(a[i])) r.kv[a[i]] = "1";
      else if (i + 1 < a.size()) {
        const std::string key = a[i];
        r.kv[key] = a[++i];
      }
    } else {
      r.pos.push_back(a[i]);
    }
  }
  return r;
}


std::string cpuName();
std::string findDefaultPack();
film::FilmParams paramsFromArgs(const Args& a, std::string& err);
std::shared_ptr<film::FilmLibrary> loadLibrary(const std::string& pack, std::string& err);
bool readPfm(const std::filesystem::path& path, dapple::ImageF& img, std::string& err);

int cmdBench(const std::vector<std::string>& args);
int cmdRenderBench(const std::vector<std::string>& args);

}  // namespace dapple::filmtool
