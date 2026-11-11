#pragma once
// Minimal lookups in a *flat* JSON object ({"key": value, ...}); enough for the C API option strings.
// Not a general parser: nested objects/arrays are skipped over by the caller's choice of keys.

#include <cstdlib>
#include <string>

namespace dapple::jsonlite {

// Position of the value after `"key":`, or npos.
inline std::size_t findValue(const std::string& json, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  std::size_t at = 0;
  while ((at = json.find(pat, at)) != std::string::npos) {
    std::size_t p = at + pat.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
    if (p < json.size() && json[p] == ':') {
      ++p;
      while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
      return p;
    }
    at += pat.size();
  }
  return std::string::npos;
}

inline double number(const std::string& json, const std::string& key, double def) {
  const std::size_t p = findValue(json, key);
  if (p == std::string::npos) return def;
  if (json.compare(p, 4, "true") == 0) return 1;
  if (json.compare(p, 5, "false") == 0) return 0;
  char* end = nullptr;
  const double v = std::strtod(json.c_str() + p, &end);
  return end == json.c_str() + p ? def : v;
}

inline bool boolean(const std::string& json, const std::string& key, bool def) {
  const std::size_t p = findValue(json, key);
  if (p == std::string::npos) return def;
  if (json.compare(p, 4, "true") == 0) return true;
  if (json.compare(p, 5, "false") == 0) return false;
  return number(json, key, def ? 1 : 0) != 0;
}

inline std::string text(const std::string& json, const std::string& key, const std::string& def) {
  const std::size_t p = findValue(json, key);
  if (p == std::string::npos || p >= json.size() || json[p] != '"') return def;
  std::string out;
  for (std::size_t i = p + 1; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) ++i;
    out += json[i];
  }
  return out;
}

}  // namespace dapple::jsonlite
