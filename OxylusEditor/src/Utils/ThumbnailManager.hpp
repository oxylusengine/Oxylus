#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <queue>
#include <vector>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class ThumbnailManager {
public:
  auto init(this ThumbnailManager& self) -> void;
  auto update(this ThumbnailManager& self) -> void;
  auto reset(this ThumbnailManager& self) -> void;

  auto get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;
  auto get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;

private:
  struct PendingMeshRender {
    std::string asset_hash;
    UUID model_uuid;
    std::filesystem::path expected_png;
  };

  std::filesystem::path cache_dir = {};

  static constexpr u32 THUMBNAIL_SIZE = 256;

  ankerl::unordered_dense::map<std::string, Texture> thumbnail_cache = {};
  ankerl::unordered_dense::set<std::string> active_jobs = {};
  std::shared_mutex thumbnail_mutex = {};

  std::mutex queue_mutex;
  std::queue<PendingMeshRender> pending_mesh_renders = {};

  auto render_thumbnail(this ThumbnailManager& self, UUID model_uuid, u32 size) -> option<std::vector<u8>>;
  auto get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path) -> std::string;

  auto find_cached(this ThumbnailManager& self, const std::string& asset_hash) -> option<TextureView>;
  auto try_claim_job(this ThumbnailManager& self, const std::string& asset_hash) -> bool;
};
} // namespace ox
