#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <span>
#include <vector>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class Scene;
struct MaterialPreview;

class ThumbnailManager {
public:
  ThumbnailManager();
  ~ThumbnailManager();

  ThumbnailManager(const ThumbnailManager&) = delete;
  auto operator=(const ThumbnailManager&) -> ThumbnailManager& = delete;

  auto init(this ThumbnailManager& self) -> void;
  auto deinit(this ThumbnailManager& self) -> void;
  auto update(this ThumbnailManager& self) -> void;
  auto reset(this ThumbnailManager& self) -> void;

  auto get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;
  auto get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;

  auto get_thumbnail_material(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;
  auto get_thumbnail_material(this ThumbnailManager& self, const UUID& material_uuid) -> TextureView;
  auto invalidate_material(this ThumbnailManager& self, const UUID& material_uuid) -> void;

  auto get_thumbnail_terrain(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> TextureView;

  auto thumbnail_unavailable(this ThumbnailManager& self, const std::filesystem::path& asset_path) -> bool;

private:
  enum class RenderKind : u8 { Model, Material };

  struct PendingRender {
    std::string cache_key = {};
    RenderKind kind = RenderKind::Model;
    UUID asset_uuid = UUID(nullptr);
    std::filesystem::path expected_png = {};
  };

  struct PendingUpload {
    std::string cache_key = {};
    std::vector<u8> pixels = {};
  };

  static constexpr u32 THUMBNAIL_SIZE = 256;

  std::filesystem::path cache_dir = {};

  ankerl::unordered_dense::map<std::string, Texture> thumbnail_cache = {};
  ankerl::unordered_dense::set<std::string> active_jobs = {};
  ankerl::unordered_dense::set<std::string> failed_jobs = {};
  std::shared_mutex thumbnail_mutex = {};

  std::mutex queue_mutex = {};
  std::queue<PendingRender> pending_renders = {};
  std::queue<PendingUpload> pending_uploads = {};

  ankerl::unordered_dense::map<std::filesystem::path, UUID> material_uuids = {};
  std::shared_mutex material_uuids_mutex = {};

  ankerl::unordered_dense::set<UUID> owned_asset_refs = {};

  // Materials edited in memory but not written to disk yet, mapped to the file's mtime at the time
  // they were marked. The on-disk cache key is derived from the file, so persisting a preview of
  // unsaved state would serve it for the saved state on the next launch. Entries retire themselves
  // once the mtime moves on, which is what saving does.
  ankerl::unordered_dense::map<UUID, i64> unsaved_materials = {};

  ankerl::unordered_dense::set<std::string> superseded_jobs = {};
  std::mutex owned_refs_mutex = {};

  std::unique_ptr<MaterialPreview> material_preview = {};

  auto render_model_thumbnail(this ThumbnailManager& self, const UUID& model_uuid, u32 size) -> option<std::vector<u8>>;
  auto render_material_thumbnail(this ThumbnailManager& self, const UUID& material_uuid, u32 size)
    -> option<std::vector<u8>>;
  auto render_scene(this ThumbnailManager& self, Scene& scene, u32 size) -> option<std::vector<u8>>;
  auto ensure_material_preview(this ThumbnailManager& self) -> bool;

  auto store_thumbnail(this ThumbnailManager& self, const std::string& cache_key, std::span<const u8> pixels) -> void;
  auto drain_pending_upload(this ThumbnailManager& self) -> void;

  auto get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path) -> std::string;
  auto has_unsaved_edits(this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& meta_path)
    -> bool;

  auto resolve_material_uuid(this ThumbnailManager& self, const std::filesystem::path& path) -> UUID;

  auto material_thumbnail_for(
    this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& asset_path
  ) -> TextureView;

  auto acquire_asset(this ThumbnailManager& self, const UUID& uuid) -> bool;

  auto find_cached(this ThumbnailManager& self, const std::string& cache_key) -> option<TextureView>;
  auto try_claim_job(this ThumbnailManager& self, const std::string& cache_key) -> bool;
  auto release_job(this ThumbnailManager& self, const std::string& cache_key) -> void;
  auto mark_job_failed(this ThumbnailManager& self, const std::string& cache_key) -> void;
  auto submit_cached_png_load(
    this ThumbnailManager& self, const std::string& cache_key, const std::filesystem::path& png_path
  ) -> void;
};
} // namespace ox
