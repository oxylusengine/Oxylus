#pragma once

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class Scene;
struct MaterialPreview;

enum class ThumbnailKind : u8 { Texture = 0, Model, Material, Terrain };

struct ThumbnailPrewarmRequest {
  std::filesystem::path path = {};
  ThumbnailKind kind = ThumbnailKind::Texture;
};

struct ThumbnailPrewarmProgress {
  usize completed = 0;
  usize total = 0;
  std::string current = {};
};

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

  // Asks for every thumbnail up front instead of waiting for the content panel to scroll one into
  // view, so a project open can report how much of its preview cache is still cold. Cheap when the
  // cache is warm: each request short-circuits on a cache hit without touching the asset.
  auto begin_prewarm(this ThumbnailManager& self, std::vector<ThumbnailPrewarmRequest>&& requests) -> void;
  auto prewarm_progress(this ThumbnailManager& self) -> ThumbnailPrewarmProgress;
  auto cancel_prewarm(this ThumbnailManager& self) -> void;

private:
  struct PendingRender {
    std::string cache_key = {};
    UUID asset_uuid = UUID(nullptr);
    std::filesystem::path expected_png = {};
  };

  struct PendingUpload {
    std::string cache_key = {};
    std::vector<u8> pixels = {};
  };

  struct PrewarmEntry {
    ThumbnailPrewarmRequest request = {};
    u32 polls = 0;
  };

  // `last_used` is stamped from `find_cached`, which holds only a shared lock, so it is written
  // through a `std::atomic_ref` -- several panels can read the same thumbnail in one frame. It stays
  // a plain `u64` because the map relocates its values and an atomic member cannot move.
  struct ThumbnailEntry {
    Texture texture = {};
    u64 last_used = 0;
  };

  // A thumbnail handed out this frame is in an ImGui draw list the GPU has not finished with, and the
  // `ImageViewID` behind it would be recycled the moment the texture dies. Evicted entries wait here
  // until every frame that could still name them has retired.
  struct RetiringThumbnail {
    Texture texture = {};
    u64 retire_after = 0;
  };

  static constexpr u32 THUMBNAIL_SIZE = 256;

  // Model and material previews are GPU renders that `update` drains one per frame, and each one
  // loads its asset, so the queue is issued in a window rather than all at once.
  static constexpr usize PREWARM_MAX_INFLIGHT = 32;
  // A request that never resolves must not hold the loading panel open forever
  static constexpr u32 PREWARM_MAX_POLLS = 600;

  std::filesystem::path cache_dir = {};

  ankerl::unordered_dense::map<std::string, ThumbnailEntry> thumbnail_cache = {};
  std::vector<RetiringThumbnail> retiring_thumbnails = {};
  std::atomic<u64> frame_counter = 0;
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

  std::vector<PrewarmEntry> prewarm_queue = {};
  std::vector<usize> prewarm_inflight = {};
  usize prewarm_cursor = 0;
  usize prewarm_done = 0;

  auto render_model_thumbnail(this ThumbnailManager& self, const UUID& model_uuid, u32 size) -> option<std::vector<u8>>;
  auto render_material_thumbnail(this ThumbnailManager& self, const UUID& material_uuid, u32 size)
    -> option<std::vector<u8>>;
  auto render_scene(this ThumbnailManager& self, Scene& scene, u32 size) -> option<std::vector<u8>>;
  auto ensure_material_preview(this ThumbnailManager& self) -> bool;

  auto store_thumbnail(this ThumbnailManager& self, const std::string& cache_key, std::span<const u8> pixels) -> void;
  auto drain_pending_upload(this ThumbnailManager& self) -> void;

  auto pump_prewarm(this ThumbnailManager& self) -> void;
  auto request_thumbnail(this ThumbnailManager& self, const ThumbnailPrewarmRequest& request) -> TextureView;

  auto get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path) -> std::string;
  auto material_is_being_edited(this ThumbnailManager& self, const UUID& material_uuid) -> bool;
  auto has_unsaved_edits(this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& meta_path)
    -> bool;

  auto resolve_material_uuid(this ThumbnailManager& self, const std::filesystem::path& path) -> UUID;

  auto material_thumbnail_for(
    this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& asset_path
  ) -> TextureView;

  auto acquire_asset(this ThumbnailManager& self, const UUID& uuid) -> bool;
  auto release_asset(this ThumbnailManager& self, const UUID& uuid) -> void;

  auto find_cached(this ThumbnailManager& self, const std::string& cache_key) -> option<TextureView>;
  auto try_claim_job(this ThumbnailManager& self, const std::string& cache_key) -> bool;
  auto release_job(this ThumbnailManager& self, const std::string& cache_key) -> void;
  auto mark_job_failed(this ThumbnailManager& self, const std::string& cache_key) -> void;
  auto submit_cached_png_load(
    this ThumbnailManager& self, const std::string& cache_key, const std::filesystem::path& png_path
  ) -> void;
  auto submit_pack_load(
    this ThumbnailManager& self, const std::string& cache_key, const std::filesystem::path& pack_path, bool cached
  ) -> void;
  auto evict_stale_thumbnails(this ThumbnailManager& self) -> void;
};
} // namespace ox
