#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fstream>

#include "dapple/encode.h"
#include "file_util.h"

namespace dapple {

Result<std::vector<std::uint8_t>> encodeJpeg(const Image8& image, const JpegOptions& options) {
  if (image.width <= 0 || image.height <= 0) return fail("empty image");
  std::vector<std::uint8_t> bytes;
  auto sink = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
  };
  if (!stbi_write_jpg_to_func(sink, &bytes, image.width, image.height, 3, image.data.data(), options.quality)) return fail("JPEG encode failed");
  return bytes;
}

Result<void> writeJpeg(const Image8& image, const std::filesystem::path& path, const JpegOptions& options) {
  if (image.width <= 0 || image.height <= 0) return fail("empty image");
  // Write to a temp file and rename, so a failed or interrupted write never
  // leaves a truncated JPEG at the final path.
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return fail("Cannot write " + pathToUtf8(path));
    auto sink = [](void* ctx, void* data, int size) {
      static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size);
    };
    const bool ok = stbi_write_jpg_to_func(sink, &out, image.width, image.height, 3, image.data.data(), options.quality) != 0;
    out.flush();
    if (!ok || !out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      return fail(ok ? "Write failed: " + pathToUtf8(path) : "JPEG encode failed");
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::error_code ec2;
    std::filesystem::rename(tmp, path, ec2);
    if (ec2) {
      std::filesystem::remove(tmp, ec);
      return fail("Cannot write " + pathToUtf8(path));
    }
  }
  return {};
}

}  // namespace dapple
