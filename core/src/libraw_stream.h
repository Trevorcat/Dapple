#pragma once
// LibRaw datastream over std::filebuf (non-ASCII paths on Windows, reads only what LibRaw asks for).
// Used by the sequence detector, which only needs metadata; mirrors the stream in decoders/libraw_decoder.cpp.

#include <libraw/libraw.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>

namespace dapple {

class MetaDatastream final : public LibRaw_abstract_datastream {
 public:
  explicit MetaDatastream(const std::filesystem::path& path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (!ec) size_ = static_cast<INT64>(sz);
    buf_.open(path, std::ios::in | std::ios::binary);
  }
  int valid() override { return buf_.is_open() ? 1 : 0; }
  int read(void* ptr, size_t size, size_t nmemb) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    return int(buf_.sgetn(static_cast<char*>(ptr), std::streamsize(size * nmemb)) / (size > 0 ? size : 1));
  }
  int seek(INT64 o, int whence) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    std::ios_base::seekdir dir = whence == SEEK_CUR ? std::ios_base::cur
                                 : whence == SEEK_END ? std::ios_base::end
                                                      : std::ios_base::beg;
    return buf_.pubseekoff(std::streamoff(o), dir) < 0;
  }
  INT64 tell() override { return buf_.pubseekoff(0, std::ios_base::cur); }
  INT64 size() override { return size_; }
  int get_char() override { return buf_.sbumpc(); }
  char* gets(char* str, int sz) override {
    if (sz < 1) return nullptr;
    std::istream is(&buf_);
    is.getline(str, sz);
    return is.fail() ? nullptr : str;
  }
  int scanf_one(const char* fmt, void* val) override {
    std::istream is(&buf_);
    if (std::strcmp(fmt, "%d") == 0) {
      int d;
      is >> d;
      if (is.fail()) return EOF;
      *static_cast<int*>(val) = d;
    } else {
      float f;
      is >> f;
      if (is.fail()) return EOF;
      *static_cast<float*>(val) = f;
    }
    return 1;
  }
  int eof() override { return buf_.sgetc() == EOF; }

 private:
  std::filebuf buf_;
  INT64 size_ = 0;
};

}  // namespace dapple
