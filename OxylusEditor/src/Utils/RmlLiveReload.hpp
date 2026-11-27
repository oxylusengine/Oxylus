#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace filewatch {
template <class StringType>
class FileWatch;
}

namespace ox {
// edits to a project's .rml and .rcss files reach the documents on screen, playing or not
class RmlLiveReload {
public:
  RmlLiveReload();
  ~RmlLiveReload();

  RmlLiveReload(const RmlLiveReload&) = delete;
  auto operator=(const RmlLiveReload&) -> RmlLiveReload& = delete;

  auto open(this RmlLiveReload& self, const std::filesystem::path& asset_dir) -> void;
  auto close(this RmlLiveReload& self) -> void;
  // main thread, once a frame
  auto update(this RmlLiveReload& self) -> void;

private:
  std::unique_ptr<filewatch::FileWatch<std::string>> watch = {};
  // written by the watcher thread
  std::mutex mutex = {};
  // relative to the asset directory
  std::vector<std::string> changed = {};
  std::chrono::steady_clock::time_point last_change = {};
};
} // namespace ox
