#pragma once

#include <filesystem>
#include <vector>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox {
class ThumbnailManager {
public:
  auto init(this ThumbnailManager& self) -> void;

  auto get_thumbnail(this ThumbnailManager& self, std::filesystem::path asset_path) -> std::shared_ptr<Texture>;

private:
  auto render_thumbnail(this ThumbnailManager& self, const std::filesystem::path& model_path, u32 size)
    -> std::vector<u8>;
  auto calculate_thumbnail_camera(
    this ThumbnailManager& self, const ox::AABB& model_aabb, glm::mat4& out_view, glm::mat4& out_proj
  ) -> void;
};
} // namespace ox
