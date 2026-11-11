#include "dapple/ingest.h"

#include <system_error>

namespace dapple {

namespace fs = std::filesystem;

FolderWatchSource::FolderWatchSource(fs::path dir, std::function<bool(const fs::path&)> accept, Options options)
    : dir_(std::move(dir)), accept_(std::move(accept)), options_(options) {
  if (options_.includeExisting) return;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::error_code e1, e2;
    const auto size = entry.file_size(e1);
    const auto mtime = entry.last_write_time(e2);
    if (!e1 && !e2) delivered_[entry.path()] = Stamp{size, mtime};
  }
}

void FolderWatchSource::poll(const IncomingCallback& onPhoto) {
  const auto now = std::chrono::steady_clock::now();
  std::error_code ec;
  std::set<fs::path> present;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const fs::path& path = entry.path();
    present.insert(path);
    if (accept_ && !accept_(path)) continue;

    std::error_code e1, e2;
    const auto size = entry.file_size(e1);
    const auto mtime = entry.last_write_time(e2);
    if (e1 || e2) continue;
    const Stamp stamp{size, mtime};
    if (auto d = delivered_.find(path); d != delivered_.end() && d->second == stamp) continue;

    auto [it, inserted] = pending_.try_emplace(path, Pending{size, mtime, now});
    if (inserted) continue;
    if (it->second.size != size || it->second.mtime != mtime) {  // still being written
      it->second = Pending{size, mtime, now};
      continue;
    }
    if (size > 0 && now - it->second.since >= options_.settleTime) {
      pending_.erase(it);
      delivered_[path] = stamp;
      onPhoto(IncomingPhoto{path, name()});
    }
  }
  if (ec) return;  // listing failed; do not forget anything
  // Forget files that are gone, so a reused filename is treated as new.
  std::erase_if(delivered_, [&](const auto& kv) { return !present.contains(kv.first); });
  std::erase_if(pending_, [&](const auto& kv) { return !present.contains(kv.first); });
}

}  // namespace dapple
