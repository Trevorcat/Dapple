#pragma once
// Small DOM JSON reader used for film profiles and film parameter overrides. Accepts `null` and the
// Python tokens NaN / Infinity. Not a validating parser; sufficient for trusted local files.

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dapple::film::detail {

struct JValue {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  bool boolean = false;
  std::string str;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue>> obj;

  const JValue* get(const std::string& key) const {
    for (const auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

class JsonParser {
 public:
  JsonParser(const char* b, const char* e) : p_(b), end_(e) {}
  bool parse(JValue& out, std::string* err) {
    ws();
    if (!value(out)) {
      if (err) *err = "JSON parse error near offset " + std::to_string(off_());
      return false;
    }
    ws();
    if (p_ != end_) {
      if (err) *err = "trailing characters in JSON";
      return false;
    }
    return true;
  }

 private:
  const char* p_;
  const char* end_;
  const char* begin_ = p_;
  long off_() const { return long(p_ - begin_); }
  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool lit(const char* s) {
    const char* q = p_;
    while (*s) {
      if (q >= end_ || *q != *s) return false;
      ++q;
      ++s;
    }
    p_ = q;
    return true;
  }
  bool str(std::string& out) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    out.clear();
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\\' && p_ + 1 < end_) {
        ++p_;
        switch (*p_) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (p_ + 4 < end_) {
              const unsigned cp = unsigned(std::strtoul(std::string(p_ + 1, p_ + 5).c_str(), nullptr, 16));
              p_ += 4;
              if (cp < 0x80) out += char(cp);
              else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
              } else {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
              }
            }
            break;
          }
          default: out += *p_; break;
        }
        ++p_;
      } else {
        out += *p_++;
      }
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }
  bool value(JValue& v) {
    if (p_ >= end_) return false;
    const char c = *p_;
    if (c == '{') {
      ++p_;
      v.type = JValue::Obj;
      ws();
      if (p_ < end_ && *p_ == '}') {
        ++p_;
        return true;
      }
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (p_ >= end_ || *p_ != ':') return false;
        ++p_;
        ws();
        JValue child;
        if (!value(child)) return false;
        v.obj.emplace_back(std::move(k), std::move(child));
        ws();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == '}') {
          ++p_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++p_;
      v.type = JValue::Arr;
      ws();
      if (p_ < end_ && *p_ == ']') {
        ++p_;
        return true;
      }
      for (;;) {
        ws();
        JValue child;
        if (!value(child)) return false;
        v.arr.push_back(std::move(child));
        ws();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == ']') {
          ++p_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      v.type = JValue::Str;
      return str(v.str);
    }
    if (lit("true")) {
      v.type = JValue::Bool;
      v.boolean = true;
      return true;
    }
    if (lit("false")) {
      v.type = JValue::Bool;
      return true;
    }
    if (lit("null")) {
      v.type = JValue::Null;
      return true;
    }
    if (lit("NaN")) {
      v.type = JValue::Num;
      v.num = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    if (lit("Infinity")) {
      v.type = JValue::Num;
      v.num = std::numeric_limits<double>::infinity();
      return true;
    }
    if (lit("-Infinity")) {
      v.type = JValue::Num;
      v.num = -std::numeric_limits<double>::infinity();
      return true;
    }
    char* e = nullptr;
    const double d = std::strtod(p_, &e);
    if (e == p_) return false;
    p_ = e;
    v.type = JValue::Num;
    v.num = d;
    return true;
  }
};

inline bool parseJson(const std::string& text, JValue& out, std::string* err) {
  JsonParser p(text.data(), text.data() + text.size());
  return p.parse(out, err);
}

// Flattens nested numeric arrays (null -> NaN) in row-major order.
inline void flattenNumbers(const JValue& v, std::vector<float>& out) {
  if (v.type == JValue::Arr) {
    for (const auto& c : v.arr) flattenNumbers(c, out);
  } else if (v.type == JValue::Num) {
    out.push_back(float(v.num));
  } else if (v.type == JValue::Null) {
    out.push_back(std::numeric_limits<float>::quiet_NaN());
  }
}

}  // namespace dapple::film::detail
