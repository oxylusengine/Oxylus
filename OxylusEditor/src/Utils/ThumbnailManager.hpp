#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <vector>

#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox {
class ThumbnailManager {
public:
  auto init(this ThumbnailManager& self) -> void;

  auto get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path)
    -> option<std::shared_ptr<Texture>>;
  auto get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path)
    -> option<std::shared_ptr<Texture>>;

private:
  static constexpr u32 THUMBNAIL_SIZE = 256;
  ankerl::unordered_dense::map<std::string, std::shared_ptr<Texture>> thumbnail_cache = {};
  ankerl::unordered_dense::set<std::string> active_jobs = {};
  std::shared_mutex thumbnail_mutex = {};
  std::filesystem::path cache_dir = {};

  auto render_thumbnail(this ThumbnailManager& self, Model* model, u32 size) -> std::vector<u8>;
  auto get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path) -> std::string;
  auto calculate_thumbnail_camera(
    this ThumbnailManager& self, const ox::AABB& model_aabb, glm::mat4& out_view, glm::mat4& out_proj
  ) -> void;
};
} // namespace ox
