#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <queue>
#include <vector>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox {
class ThumbnailManager {
public:
  auto init(this ThumbnailManager& self) -> void;
  auto update(this ThumbnailManager& self) -> void;
  auto reset(this ThumbnailManager& self) -> void;

  auto get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path)
    -> option<std::shared_ptr<Texture>>;
  auto get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path)
    -> option<std::shared_ptr<Texture>>;

private:
  struct PendingUpload {
    std::string asset_hash;
    std::vector<u8> pixel_data;
    u32 size = 256;
  };

  struct PendingMeshRender {
    std::string asset_hash;
    UUID model_uuid;
    std::filesystem::path expected_png;
  };

  std::filesystem::path cache_dir = {};

  static constexpr u32 THUMBNAIL_SIZE = 256;

  ankerl::unordered_dense::map<std::string, std::shared_ptr<Texture>> thumbnail_cache = {};
  ankerl::unordered_dense::set<std::string> active_jobs = {};
  std::shared_mutex thumbnail_mutex = {};

  std::mutex queue_mutex;
  std::vector<PendingUpload> pending_uploads = {};
  std::queue<PendingMeshRender> pending_mesh_renders = {};

  auto render_thumbnail(this ThumbnailManager& self, UUID model_uuid, u32 size) -> option<std::vector<u8>>;
  auto get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path) -> std::string;

  auto calculate_thumbnail_camera(
    this ThumbnailManager& self,
    const ox::AABB& model_aabb,
    glm::vec3& out_position,
    glm::quat& out_rotation,
    f32& out_near,
    f32& out_far
  ) -> void;
};
} // namespace ox
