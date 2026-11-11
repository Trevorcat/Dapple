#pragma once

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace dapple {

// A photo that has fully arrived on the device and can be decoded.
struct IncomingPhoto {
  std::filesystem::path path;
  std::string sourceName;  // which IngestSource delivered it
};

using IncomingCallback = std::function<void(const IncomingPhoto&)>;

// Anything that delivers new photos: an FTP server the camera uploads to, a
// USB/PTP session, the phone's media store, or a plain folder. The develop
// pipeline only ever sees IncomingPhoto, so transports and camera vendors can
// be added without touching it.
class IngestSource {
 public:
  virtual ~IngestSource() = default;
  virtual const char* name() const = 0;
  // Checks for new photos and invokes the callback for each. Non-blocking
  // sources may do nothing here and call back from their own thread.
  virtual void poll(const IncomingCallback& onPhoto) = 0;
};

// Watches a directory (portable polling). The FTP receiver will write into a
// directory and reuse this, and it is what the desktop CLI uses for testing.
class FolderWatchSource final : public IngestSource {
 public:
  struct Options {
    bool includeExisting = false;
    // A file is considered complete once its size has been stable this long
    // (cameras upload a 60-120 MB RAW over several seconds).
    std::chrono::milliseconds settleTime{1500};
  };

  FolderWatchSource(std::filesystem::path dir, std::function<bool(const std::filesystem::path&)> accept,
                    Options options);
  const char* name() const override { return "folder"; }
  void poll(const IncomingCallback& onPhoto) override;

 private:
  struct Pending {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    std::chrono::steady_clock::time_point since;
  };
  struct Stamp {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool operator==(const Stamp&) const = default;
  };
  std::filesystem::path dir_;
  std::function<bool(const std::filesystem::path&)> accept_;
  Options options_;
  std::map<std::filesystem::path, Stamp> delivered_;  // what was delivered, by size+mtime
  std::map<std::filesystem::path, Pending> pending_;
};

}  // namespace dapple
