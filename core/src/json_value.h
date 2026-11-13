#pragma once
// Internal: the small tolerant JSON reader (a full parser, unlike json_lite.h) shared by the EditParams reader (edit_params_json.cpp) and the local-adjustment reader
// (local_adjust.cpp). No external dependency. Also a writer of a parsed value (unknown local mask components are kept verbatim).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace dapple::jsonlite {

struct JValue {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  double number = 0;
  bool boolean = false;
  std::string str;
  std::vector<JValue> items;
  std::vector<std::pair<std::string, JValue>> members;

  const JValue* get(const char* key) const {
    if (kind != Object) return nullptr;
    for (const auto& m : members)
      if (m.first == key) return &m.second;
    return nullptr;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& s) : s_(s) {}

  bool parse(JValue& out) {
    skipWs();
    if (!value(out, 0)) return false;
    skipWs();
    return pos_ == s_.size();
  }
  std::string error() const { return "JSON syntax error at offset " + std::to_string(pos_); }

 private:
  const std::string& s_;
  std::size_t pos_ = 0;

  void skipWs() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool lit(const char* word) {
    const std::size_t n = std::char_traits<char>::length(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }
  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned& v) {
    if (pos_ + 4 > s_.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {
            pos_ += 2;
            unsigned lo = 0;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: out += e; break;  // \" \\ \/ and anything unknown
      }
    }
    return false;
  }
  bool value(JValue& v, int depth) {
    if (depth > 32 || pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      v.kind = JValue::Object;
      skipWs();
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      for (;;) {
        skipWs();
        std::string key;
        if (!string(key)) return false;
        skipWs();
        if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
        skipWs();
        JValue child;
        if (!value(child, depth + 1)) return false;
        v.members.emplace_back(std::move(key), std::move(child));
        skipWs();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == '}') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      v.kind = JValue::Array;
      skipWs();
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      for (;;) {
        skipWs();
        JValue child;
        if (!value(child, depth + 1)) return false;
        v.items.push_back(std::move(child));
        skipWs();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '"') {
      v.kind = JValue::String;
      return string(v.str);
    }
    if (lit("true")) { v.kind = JValue::Bool; v.boolean = true; return true; }
    if (lit("false")) { v.kind = JValue::Bool; return true; }
    if (lit("null")) { v.kind = JValue::Null; return true; }
    char* end = nullptr;
    const double d = std::strtod(s_.c_str() + pos_, &end);
    if (end == s_.c_str() + pos_) return false;
    pos_ = std::size_t(end - s_.c_str());
    v.kind = JValue::Number;
    v.number = d;
    return true;
  }
};

inline bool asNumber(const JValue& v, float& out) {
  if (v.kind == JValue::Number) {
    out = float(v.number);
    return std::isfinite(out);
  }
  if (v.kind == JValue::String) {
    char* end = nullptr;
    const float f = std::strtof(v.str.c_str(), &end);
    if (end != v.str.c_str() && std::isfinite(f)) {
      out = f;
      return true;
    }
  }
  return false;
}


// Compact JSON text of a parsed value (numbers with up to 9 significant digits).
inline void writeValue(const JValue& v, std::string& o) {
  switch (v.kind) {
    case JValue::Null: o += "null"; break;
    case JValue::Bool: o += v.boolean ? "true" : "false"; break;
    case JValue::Number: {
      char b[40];
      std::snprintf(b, sizeof b, "%.9g", v.number);
      o += b;
      break;
    }
    case JValue::String:
      o += '"';
      for (char c : v.str) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
          o += b;
        } else {
          o += c;
        }
      }
      o += '"';
      break;
    case JValue::Array:
      o += '[';
      for (std::size_t i = 0; i < v.items.size(); ++i) {
        if (i) o += ',';
        writeValue(v.items[i], o);
      }
      o += ']';
      break;
    case JValue::Object:
      o += '{';
      for (std::size_t i = 0; i < v.members.size(); ++i) {
        if (i) o += ',';
        JValue k;
        k.kind = JValue::String;
        k.str = v.members[i].first;
        writeValue(k, o);
        o += ':';
        writeValue(v.members[i].second, o);
      }
      o += '}';
      break;
  }
}

}  // namespace dapple::jsonlite
