#include "Utils/RmlLiveReload.hpp"

#include <FileWatch.hpp>
#include <algorithm>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "UI/RmlUI.hpp"

namespace ox {
// an editor saves through a temporary file and a rename, one reload covers the burst
constexpr auto RELOAD_DEBOUNCE = std::chrono::milliseconds(150);

static auto is_ui_source(std::string_view relative_path) -> bool {
  return relative_path.ends_with(".rml") || relative_path.ends_with(".rcss");
}

RmlLiveReload::RmlLiveReload() = default;

RmlLiveReload::~RmlLiveReload() { this->close(); }

auto RmlLiveReload::open(this RmlLiveReload& self, const std::filesystem::path& asset_dir) -> void {
  ZoneScoped;

  self.close();
  if (!std::filesystem::exists(asset_dir)) {
    return;
  }

  self.watch = std::make_unique<filewatch::FileWatch<std::string>>(
    asset_dir.string(),
    [&self](const std::string& path, filewatch::Event) {
      if (!is_ui_source(path)) {
        return;
      }

      auto lock = std::unique_lock(self.mutex);
      self.changed.push_back(path);
      self.last_change = std::chrono::steady_clock::now();
    }
  );
}

auto RmlLiveReload::close(this RmlLiveReload& self) -> void {
  self.watch.reset();

  auto lock = std::unique_lock(self.mutex);
  self.changed.clear();
}

auto RmlLiveReload::update(this RmlLiveReload& self) -> void {
  ZoneScoped;

  auto paths = std::vector<std::string>{};
  {
    auto lock = std::unique_lock(self.mutex);
    if (self.changed.empty() || std::chrono::steady_clock::now() - self.last_change < RELOAD_DEBOUNCE) {
      return;
    }

    paths.swap(self.changed);
  }

  if (!App::has_mod<RmlUI>()) {
    return;
  }

  // what a document's source url is, since gameplay loads them by virtual path
  for (auto& path : paths) {
    path = (std::filesystem::path(VFS::ASSETS_DIR) / path).generic_string();
  }

  std::ranges::sort(paths);
  paths.erase(std::ranges::unique(paths).begin(), paths.end());
  App::mod<RmlUI>().reload_ui_files(paths);
}
} // namespace ox
