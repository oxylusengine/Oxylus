#include "ThumbnailManager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/quaternion.hpp>
#include <limits>
#include <numbers>
#include <stb_image_write.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Asset/Model.hpp"
#include "Asset/TerrainEdits.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/JobManager.hpp"
#include "Editor.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "ResourceCompiler.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/ThumbnailCamera.hpp"

namespace ox {
constexpr auto PREVIEW_SPHERE_RADIUS = 1.0f;
constexpr auto PREVIEW_SPHERE_RINGS = 48_u32;
constexpr auto PREVIEW_SPHERE_SECTORS = 96_u32;

constexpr auto PREVIEW_FOV = 30.0f;
constexpr auto PREVIEW_FRAMING_PADDING = 1.08f;

constexpr auto PREVIEW_SUN_INTENSITY = 6.0f;
constexpr auto PREVIEW_EXPOSURE = 1.0f;
constexpr auto PREVIEW_AMBIENT = 0.25f;

constexpr auto MODEL_PREVIEW_FOV = 40.0f;

// The terrain asset only holds the brush strokes, not the procedural base they sit on, so the
// preview is a shaded relief of the height deltas tinted by the painted splat rather than a render.
constexpr auto TERRAIN_PREVIEW_RELIEF = 25.0f;
constexpr auto TERRAIN_PREVIEW_AMBIENT = 0.3f;
// Height deltas are normalized heightmap units, so an almost untouched map would otherwise be
// stretched into a full range of relief.
constexpr auto TERRAIN_PREVIEW_MIN_RANGE = 0.05f;

// The splat override tints, it does not replace: a map painted end to end is common, and it would
// otherwise flatten the preview into one colour and hide the sculpting.
constexpr auto TERRAIN_PREVIEW_UNPAINTED = glm::vec3(0.72f, 0.74f, 0.78f);
// Zero delta is untouched ground, so it sits mid tone with cuts below it and raises above.
constexpr auto TERRAIN_PREVIEW_CUT_TONE = 0.55f;
constexpr auto TERRAIN_PREVIEW_RAISE_TONE = 1.2f;

constexpr auto TERRAIN_LAYER_COLORS = std::array{
  glm::vec3(0.45f, 0.62f, 0.32f),
  glm::vec3(0.62f, 0.60f, 0.56f),
  glm::vec3(0.55f, 0.45f, 0.32f),
  glm::vec3(0.95f, 0.96f, 1.00f),
};

struct MaterialPreview {
  std::unique_ptr<Scene> scene = {};
  flecs::entity sphere = {};
  UUID sphere_model_uuid = UUID(nullptr);
};

static auto generate_uv_sphere(f32 radius, u32 rings, u32 sectors) -> rc::ProceduralMeshRequest {
  auto model = rc::ProceduralMeshRequest{.name = "MaterialPreviewSphere"};

  const auto inv_rings = 1.0f / static_cast<f32>(rings);
  const auto inv_sectors = 1.0f / static_cast<f32>(sectors);

  model.vertices.reserve((rings + 1) * (sectors + 1));
  for (u32 ring = 0; ring <= rings; ++ring) {
    const auto v = static_cast<f32>(ring) * inv_rings;
    const auto phi = v * std::numbers::pi_v<f32>;
    const auto cos_phi = std::cos(phi);
    const auto sin_phi = std::sin(phi);

    for (u32 sector = 0; sector <= sectors; ++sector) {
      const auto u = static_cast<f32>(sector) * inv_sectors;
      const auto theta = u * 2.0f * std::numbers::pi_v<f32>;
      const auto normal = glm::vec3(sin_phi * std::cos(theta), cos_phi, sin_phi * std::sin(theta));
      const auto position = normal * radius;

      model.vertices.emplace_back(
        rc::ModelVertex{
          .position = {position.x, position.y, position.z},
          .normal = {normal.x, normal.y, normal.z},
          .uv = {u, v},
        }
      );
    }
  }

  model.indices.reserve(rings * sectors * 6);
  for (u32 ring = 0; ring < rings; ++ring) {
    for (u32 sector = 0; sector < sectors; ++sector) {
      const auto current_row = ring * (sectors + 1);
      const auto next_row = (ring + 1) * (sectors + 1);

      const auto top_left = current_row + sector;
      const auto top_right = current_row + sector + 1;
      const auto bottom_left = next_row + sector;
      const auto bottom_right = next_row + sector + 1;

      model.indices.emplace_back(top_left);
      model.indices.emplace_back(top_right);
      model.indices.emplace_back(bottom_left);

      model.indices.emplace_back(top_right);
      model.indices.emplace_back(bottom_right);
      model.indices.emplace_back(bottom_left);
    }
  }

  return model;
}

struct TerrainEditSample {
  f32 height = 0.f;
  glm::vec4 splat = {};
};

static auto load_terrain_edit(const TerrainEdits& edits, u32 x, u32 y) -> TerrainEditSample {
  const auto index = static_cast<usize>(y) * edits.resolution.x + x;
  const auto* splat = edits.splat.data() + index * 4;

  return {
    .height = reinterpret_cast<const f32*>(edits.height.data())[index],
    .splat = glm::vec4(splat[0], splat[1], splat[2], splat[3]) / 255.f,
  };
}

static auto box_terrain_sample(const TerrainEdits& edits, glm::uvec2 begin, glm::uvec2 end) -> TerrainEditSample {
  auto accumulated = TerrainEditSample{};
  for (auto y = begin.y; y < end.y; ++y) {
    for (auto x = begin.x; x < end.x; ++x) {
      const auto sample = load_terrain_edit(edits, x, y);

      accumulated.height += sample.height;
      accumulated.splat += sample.splat;
    }
  }

  const auto count = static_cast<f32>((end.y - begin.y) * (end.x - begin.x));

  return {.height = accumulated.height / count, .splat = accumulated.splat / count};
}

static auto bilinear_terrain_sample(const TerrainEdits& edits, glm::vec2 coord) -> TerrainEditSample {
  const auto limit = glm::vec2(edits.resolution) - 1.f;
  const auto clamped = glm::clamp(coord, glm::vec2(0.f), limit);
  const auto floored = glm::floor(clamped);
  const auto next = glm::min(floored + 1.f, limit);
  const auto blend = clamped - floored;

  const auto x0 = static_cast<u32>(floored.x);
  const auto y0 = static_cast<u32>(floored.y);
  const auto x1 = static_cast<u32>(next.x);
  const auto y1 = static_cast<u32>(next.y);

  const auto top_left = load_terrain_edit(edits, x0, y0);
  const auto top_right = load_terrain_edit(edits, x1, y0);
  const auto bottom_left = load_terrain_edit(edits, x0, y1);
  const auto bottom_right = load_terrain_edit(edits, x1, y1);

  const auto top = TerrainEditSample{
    .height = glm::mix(top_left.height, top_right.height, blend.x),
    .splat = glm::mix(top_left.splat, top_right.splat, blend.x),
  };
  const auto bottom = TerrainEditSample{
    .height = glm::mix(bottom_left.height, bottom_right.height, blend.x),
    .splat = glm::mix(bottom_left.splat, bottom_right.splat, blend.x),
  };

  return {
    .height = glm::mix(top.height, bottom.height, blend.y),
    .splat = glm::mix(top.splat, bottom.splat, blend.y),
  };
}

static auto resample_terrain_edits(
  const TerrainEdits& edits, u32 size, std::span<f32> out_height, std::span<glm::vec4> out_splat
) -> void {
  const auto resolution = edits.resolution;
  const auto upsampling = resolution.x < size || resolution.y < size;

  for (auto dst_y = 0_u32; dst_y < size; ++dst_y) {
    for (auto dst_x = 0_u32; dst_x < size; ++dst_x) {
      const auto begin = glm::uvec2(dst_x * resolution.x / size, dst_y * resolution.y / size);
      const auto end = glm::uvec2(
        ox::max((dst_x + 1) * resolution.x / size, begin.x + 1),
        ox::max((dst_y + 1) * resolution.y / size, begin.y + 1)
      );
      const auto center = (glm::vec2(dst_x, dst_y) + 0.5f) / static_cast<f32>(size) * glm::vec2(resolution) - 0.5f;

      const auto sample = upsampling ? bilinear_terrain_sample(edits, center) : box_terrain_sample(edits, begin, end);

      const auto dst = static_cast<usize>(dst_y) * size + dst_x;
      out_height[dst] = sample.height;
      out_splat[dst] = sample.splat;
    }
  }
}

static auto render_terrain_preview(const TerrainEdits& edits, u32 size) -> std::vector<u8> {
  ZoneScoped;

  const auto texel_count = static_cast<usize>(edits.resolution.x) * edits.resolution.y;
  if (texel_count == 0 || edits.height.size() < texel_count * sizeof(f32) || edits.splat.size() < texel_count * 4) {
    return {};
  }

  const auto grid_size = static_cast<usize>(size) * size;
  auto height_grid = std::vector<f32>(grid_size);
  auto splat_grid = std::vector<glm::vec4>(grid_size);
  resample_terrain_edits(edits, size, height_grid, splat_grid);

  auto min_height = std::numeric_limits<f32>::max();
  auto max_height = std::numeric_limits<f32>::lowest();
  for (const auto height : height_grid) {
    min_height = ox::min(min_height, height);
    max_height = ox::max(max_height, height);
  }

  const auto height_range = ox::max(ox::max(-min_height, max_height), TERRAIN_PREVIEW_MIN_RANGE);

  const auto sample = [&](i32 x, i32 y) {
    const auto cx = std::clamp(x, 0, static_cast<i32>(size) - 1);
    const auto cy = std::clamp(y, 0, static_cast<i32>(size) - 1);

    return height_grid[static_cast<usize>(cy) * size + cx];
  };

  const auto light_direction = glm::normalize(glm::vec3(-0.45f, 0.82f, -0.45f));

  auto pixels = std::vector<u8>(grid_size * 4);
  for (auto y = 0_u32; y < size; ++y) {
    for (auto x = 0_u32; x < size; ++x) {
      const auto ix = static_cast<i32>(x);
      const auto iy = static_cast<i32>(y);

      const auto dhdx = (sample(ix + 1, iy) - sample(ix - 1, iy)) * 0.5f / height_range * TERRAIN_PREVIEW_RELIEF;
      const auto dhdy = (sample(ix, iy + 1) - sample(ix, iy - 1)) * 0.5f / height_range * TERRAIN_PREVIEW_RELIEF;
      const auto normal = glm::normalize(glm::vec3(-dhdx, 1.f, -dhdy));
      const auto shade = TERRAIN_PREVIEW_AMBIENT +
                         (1.f - TERRAIN_PREVIEW_AMBIENT) * ox::max(glm::dot(normal, light_direction), 0.f);

      const auto index = static_cast<usize>(y) * size + x;
      const auto altitude = std::clamp(0.5f + height_grid[index] * 0.5f / height_range, 0.f, 1.f);

      const auto painted = splat_grid[index];
      const auto weights = glm::vec4(
        std::clamp(1.f - painted.x - painted.y - painted.z, 0.f, 1.f),
        painted.x,
        painted.y,
        painted.z
      );
      const auto layer_color = weights.x * TERRAIN_LAYER_COLORS[0] + weights.y * TERRAIN_LAYER_COLORS[1] +
                               weights.z * TERRAIN_LAYER_COLORS[2] + weights.w * TERRAIN_LAYER_COLORS[3];

      const auto tone = glm::mix(TERRAIN_PREVIEW_CUT_TONE, TERRAIN_PREVIEW_RAISE_TONE, altitude);
      const auto color = glm::mix(TERRAIN_PREVIEW_UNPAINTED, layer_color, painted.w) * tone * shade;

      pixels[index * 4] = static_cast<u8>(std::clamp(color.r, 0.f, 1.f) * 255.f);
      pixels[index * 4 + 1] = static_cast<u8>(std::clamp(color.g, 0.f, 1.f) * 255.f);
      pixels[index * 4 + 2] = static_cast<u8>(std::clamp(color.b, 0.f, 1.f) * 255.f);
      pixels[index * 4 + 3] = 255;
    }
  }

  return pixels;
}

static auto write_thumbnail_png(const std::filesystem::path& path, std::span<const u8> pixels, u32 size) -> void {
  ZoneScoped;

  const auto isize = static_cast<i32>(size);
  stbi_write_png(path.string().c_str(), isize, isize, 4, pixels.data(), isize * 4);
}

// A compiled texture is block compressed, so a preview cannot be resampled out of it. The smallest
// mip that still covers the thumbnail becomes the preview's level 0 instead, which keeps a 4K source
// from costing its full size in VRAM for a content browser icon.
static auto trim_to_thumbnail_mips(const TextureData& data, u32 size) -> TextureData {
  ZoneScoped;

  auto level = usize{0};
  while (level + 1 < data.mips.size() && (data.mips[level].width > size || data.mips[level].height > size)) {
    level += 1;
  }

  auto result = TextureData{
    .name = data.name,
    .vk_format = data.vk_format,
    .width = data.mips[level].width,
    .height = data.mips[level].height,
    .layer_count = data.layer_count,
  };
  result.mips.assign(data.mips.begin() + static_cast<std::ptrdiff_t>(level), data.mips.end());

  return result;
}

// The trim is persisted as a small pack rather than a PNG: the blocks would have to be decoded to
// write one, and a pack reads back through the same path as the importer's own output. It is a few
// kilobytes, so the copy here costs nothing next to unpacking the full-size source again.
static auto write_thumbnail_pack(const std::filesystem::path& path, const TextureData& data) -> bool {
  ZoneScoped;

  auto file = AssetFile{};
  file.add_entry(TextureData(data));

  return file.pack(path);
}

static auto material_meta_path(const std::filesystem::path& asset_path) -> std::filesystem::path {
  if (!owns_meta_file(asset_path)) {
    return {};
  }

  return meta_file_path(asset_path);
}

static auto file_mtime(const std::filesystem::path& path) -> i64 {
  auto error = std::error_code();
  const auto time = std::filesystem::last_write_time(path, error);
  if (error) {
    return 0;
  }

  return static_cast<i64>(time.time_since_epoch().count());
}

static auto live_cache_key(const UUID& material_uuid) -> std::string {
  memory::ScopedStack stack;

  return std::string(stack.format("live_{}", material_uuid.str()));
}

ThumbnailManager::ThumbnailManager() = default;
ThumbnailManager::~ThumbnailManager() = default;

auto ThumbnailManager::init(this ThumbnailManager& self) -> void {
  ZoneScoped;

  self.cache_dir = std::filesystem::current_path() / ".oxeditor/thumbnails";
  if (!std::filesystem::exists(self.cache_dir)) {
    std::filesystem::create_directories(self.cache_dir);
  }
}

auto ThumbnailManager::deinit(this ThumbnailManager& self) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  {
    auto lock = std::unique_lock(self.owned_refs_mutex);
    for (const auto& uuid : self.owned_asset_refs) {
      asset_man.unload_asset(uuid);
    }
    self.owned_asset_refs.clear();
  }

  if (!self.material_preview) {
    return;
  }

  self.material_preview->scene.reset();

  if (self.material_preview->sphere_model_uuid) {
    asset_man.delete_asset(self.material_preview->sphere_model_uuid);
  }

  self.material_preview.reset();
}

auto ThumbnailManager::acquire_asset(this ThumbnailManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto lock = std::unique_lock(self.owned_refs_mutex);
  if (self.owned_asset_refs.contains(uuid)) {
    return asset_man.is_loaded(uuid);
  }

  if (!asset_man.load_asset(uuid)) {
    return false;
  }

  self.owned_asset_refs.insert(uuid);

  return true;
}

auto ThumbnailManager::release_asset(this ThumbnailManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.owned_refs_mutex);
  if (self.owned_asset_refs.erase(uuid) == 0) {
    return;
  }

  App::mod<AssetManager>().unload_asset(uuid);
}

auto ThumbnailManager::update(this ThumbnailManager& self) -> void {
  ZoneScoped;

  if (!App::get_rendercontext().frame_allocator.has_value()) {
    return;
  }

  // `Editor::update` runs this ahead of every panel, so nothing evicted here was handed out earlier
  // in the same frame
  self.frame_counter.fetch_add(1, std::memory_order_relaxed);
  self.evict_stale_thumbnails();

  self.pump_prewarm();
  self.drain_pending_upload();

  auto render_job = option<PendingRender>(nullopt);
  {
    auto lock = std::unique_lock(self.queue_mutex);
    if (!self.pending_renders.empty()) {
      render_job = self.pending_renders.front();
      self.pending_renders.pop();
    }
  }

  if (!render_job) {
    return;
  }

  auto asset_type = AssetType::None;
  if (auto asset = App::mod<AssetManager>().get_asset(render_job->asset_uuid)) {
    asset_type = asset->type;
  }

  auto pixels = option<std::vector<u8>>(nullopt);
  switch (asset_type) {
    case AssetType::Model   : pixels = self.render_model_thumbnail(render_job->asset_uuid, THUMBNAIL_SIZE); break;
    case AssetType::Material: pixels = self.render_material_thumbnail(render_job->asset_uuid, THUMBNAIL_SIZE); break;
    default                 : {
      OX_LOG_ERROR(
        "Can't render a thumbnail for asset {} of type {}.",
        render_job->asset_uuid.str(),
        AssetManager::to_asset_type_sv(asset_type)
      );
      self.mark_job_failed(render_job->cache_key);
      return;
    }
  }

  // The pixels are what the cache keeps; the asset itself was only needed for the render, and holding
  // on to it leaves a project-wide prewarm pinning every model and every material -- and a material
  // refs its five texture slots, so that is most of the project's textures resident for the session.
  // A material being dragged in the inspector is the exception: it is about to be rendered again, so
  // it keeps its ref until the edit is written out and the next render releases it.
  if (asset_type == AssetType::Model || !self.material_is_being_edited(render_job->asset_uuid)) {
    self.release_asset(render_job->asset_uuid);
  }

  if (!pixels.has_value() || pixels->empty()) {
    self.release_job(render_job->cache_key);
    return;
  }

  if (!render_job->expected_png.empty()) {
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("ContentPanelThumbnail_WritePNG");
    job_man.submit(Job::create([expected_png = render_job->expected_png, pixel_bytes = *pixels]() {
      write_thumbnail_png(expected_png, pixel_bytes, THUMBNAIL_SIZE);
    }));
    job_man.pop_job_name();
  }

  self.store_thumbnail(render_job->cache_key, *pixels);
}

auto ThumbnailManager::store_thumbnail(
  this ThumbnailManager& self, const std::string& cache_key, std::span<const u8> pixels
) -> void {
  ZoneScoped;

  auto thumbnail_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Srgb,
    .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  thumbnail_texture.upload(pixels, vuk::eFragmentSampled);

  auto lock = std::unique_lock(self.thumbnail_mutex);
  if (self.superseded_jobs.contains(cache_key)) {
    self.superseded_jobs.erase(cache_key);
    self.active_jobs.erase(cache_key);
    return;
  }

  self.thumbnail_cache.insert_or_assign(
    cache_key,
    ThumbnailEntry{
      .texture = std::move(thumbnail_texture),
      .last_used = self.frame_counter.load(std::memory_order_relaxed)
    }
  );
  self.active_jobs.erase(cache_key);
}

auto ThumbnailManager::drain_pending_upload(this ThumbnailManager& self) -> void {
  ZoneScoped;

  auto upload = option<PendingUpload>(nullopt);
  {
    auto lock = std::unique_lock(self.queue_mutex);
    if (!self.pending_uploads.empty()) {
      upload = std::move(self.pending_uploads.front());
      self.pending_uploads.pop();
    }
  }

  if (!upload) {
    return;
  }

  self.store_thumbnail(upload->cache_key, upload->pixels);
}

auto ThumbnailManager::begin_prewarm(this ThumbnailManager& self, std::vector<ThumbnailPrewarmRequest>&& requests)
  -> void {
  ZoneScoped;

  self.prewarm_queue.clear();
  self.prewarm_queue.reserve(requests.size());
  for (auto& request : requests) {
    self.prewarm_queue.emplace_back(PrewarmEntry{.request = std::move(request)});
  }

  self.prewarm_inflight.clear();
  self.prewarm_cursor = 0;
  self.prewarm_done = 0;
}

auto ThumbnailManager::cancel_prewarm(this ThumbnailManager& self) -> void {
  ZoneScoped;

  self.prewarm_queue.clear();
  self.prewarm_inflight.clear();
  self.prewarm_cursor = 0;
  self.prewarm_done = 0;
}

auto ThumbnailManager::prewarm_progress(this ThumbnailManager& self) -> ThumbnailPrewarmProgress {
  ZoneScoped;

  auto progress = ThumbnailPrewarmProgress{
    .completed = self.prewarm_done,
    .total = self.prewarm_queue.size(),
  };

  if (!self.prewarm_inflight.empty()) {
    progress.current = self.prewarm_queue[self.prewarm_inflight.front()].request.path.filename().string();
  }

  return progress;
}

auto ThumbnailManager::request_thumbnail(this ThumbnailManager& self, const ThumbnailPrewarmRequest& request)
  -> TextureView {
  switch (request.kind) {
    case ThumbnailKind::Texture : return self.get_thumbnail_texture(request.path);
    case ThumbnailKind::Model   : return self.get_thumbnail_model(request.path);
    case ThumbnailKind::Material: return self.get_thumbnail_material(request.path);
    case ThumbnailKind::Terrain : return self.get_thumbnail_terrain(request.path);
  }

  return {};
}

auto ThumbnailManager::pump_prewarm(this ThumbnailManager& self) -> void {
  ZoneScoped;

  if (self.prewarm_done == self.prewarm_queue.size()) {
    return;
  }

  // Re-asking is what polls: a claimed job short-circuits in `try_claim_job` before any import
  // work, so this costs a hash lookup and the mtime stat behind the cache key.
  std::erase_if(self.prewarm_inflight, [&self](const usize index) {
    auto& entry = self.prewarm_queue[index];
    entry.polls += 1;

    const auto resolved = static_cast<bool>(self.request_thumbnail(entry.request)) ||
                          self.thumbnail_unavailable(entry.request.path) || entry.polls >= PREWARM_MAX_POLLS;
    if (resolved) {
      self.prewarm_done += 1;
    }

    return resolved;
  });

  while (self.prewarm_inflight.size() < PREWARM_MAX_INFLIGHT && self.prewarm_cursor < self.prewarm_queue.size()) {
    const auto index = self.prewarm_cursor;
    self.prewarm_cursor += 1;

    auto& entry = self.prewarm_queue[index];
    entry.polls += 1;
    if (self.request_thumbnail(entry.request) || self.thumbnail_unavailable(entry.request.path)) {
      self.prewarm_done += 1;
      continue;
    }

    self.prewarm_inflight.push_back(index);
  }
}

auto ThumbnailManager::reset(this ThumbnailManager& self) -> void {
  ZoneScoped;

  self.cancel_prewarm();

  if (std::filesystem::exists(self.cache_dir)) {
    std::filesystem::remove_all(self.cache_dir);
  }

  self.init();

  {
    auto lock = std::unique_lock(self.material_uuids_mutex);
    self.material_uuids.clear();
  }

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.clear();
  self.unsaved_materials.clear();
  self.superseded_jobs.clear();
}

auto ThumbnailManager::find_cached(this ThumbnailManager& self, const std::string& cache_key) -> option<TextureView> {
  auto lock = std::shared_lock(self.thumbnail_mutex);
  auto it = self.thumbnail_cache.find(cache_key);
  if (it != self.thumbnail_cache.end()) {
    // the only place a thumbnail is handed out, so this is where the LRU learns what is still in use
    std::atomic_ref(it->second.last_used)
      .store(self.frame_counter.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return it->second.texture.view();
  }

  return nullopt;
}

// Thumbnails are only reclaimed once the pool is over its cap, and never while the prewarm is still
// polling for one -- dropping an entry it is waiting on would send it back around to cook the asset
// again.
auto ThumbnailManager::evict_stale_thumbnails(this ThumbnailManager& self) -> void {
  ZoneScoped;

  // A view handed out on an earlier frame can still be in a draw list the GPU is working through, so
  // an evicted texture is only destroyed once every such frame has retired.
  const auto retire_delay = static_cast<u64>(App::get_rendercontext().num_inflight_frames) + 1;
  const auto frame = self.frame_counter.load(std::memory_order_relaxed);

  std::erase_if(self.retiring_thumbnails, [frame](const RetiringThumbnail& retiring) {
    return frame >= retiring.retire_after;
  });

  if (self.prewarm_done != self.prewarm_queue.size()) {
    return;
  }

  const auto cap = static_cast<usize>(ox::max(App::mod<Editor>().editor_cvar.cvar_thumbnail_pool_size.get(), 1));

  auto lock = std::unique_lock(self.thumbnail_mutex);
  if (self.thumbnail_cache.size() <= cap) {
    return;
  }

  auto candidates = std::vector<std::pair<u64, std::string>>();
  candidates.reserve(self.thumbnail_cache.size());
  for (const auto& [key, entry] : self.thumbnail_cache) {
    // a unique lock is held, so no `find_cached` can be stamping this concurrently
    const auto last_used = entry.last_used;
    // anything a live frame could still be naming stays, however cold it looks
    if (frame - last_used < retire_delay) {
      continue;
    }

    candidates.emplace_back(last_used, key);
  }

  const auto excess = self.thumbnail_cache.size() - cap;
  if (candidates.size() > excess) {
    std::ranges::nth_element(candidates, candidates.begin() + static_cast<std::ptrdiff_t>(excess));
    candidates.resize(excess);
  }

  for (const auto& [last_used, key] : candidates) {
    auto it = self.thumbnail_cache.find(key);
    if (it == self.thumbnail_cache.end()) {
      continue;
    }

    self.retiring_thumbnails.emplace_back(
      RetiringThumbnail{.texture = std::move(it->second.texture), .retire_after = frame + retire_delay}
    );
    self.thumbnail_cache.erase(it);
  }
}

auto ThumbnailManager::try_claim_job(this ThumbnailManager& self, const std::string& cache_key) -> bool {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  if (self.active_jobs.contains(cache_key) || self.failed_jobs.contains(cache_key)) {
    return false;
  }
  self.active_jobs.insert(cache_key);

  return true;
}

auto ThumbnailManager::release_job(this ThumbnailManager& self, const std::string& cache_key) -> void {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.active_jobs.erase(cache_key);
}

auto ThumbnailManager::mark_job_failed(this ThumbnailManager& self, const std::string& cache_key) -> void {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.active_jobs.erase(cache_key);
  self.failed_jobs.insert(cache_key);
}

auto ThumbnailManager::submit_cached_png_load(
  this ThumbnailManager& self, const std::string& cache_key, const std::filesystem::path& png_path
) -> void {
  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_CacheLoad");
  job_man.submit(Job::create([&self, png_path, cache_key]() {
    auto thumbnail_texture = Texture::create({
      .source = png_path,
      .target_width = THUMBNAIL_SIZE,
      .target_height = THUMBNAIL_SIZE,
    });
    if (!thumbnail_texture) {
      self.mark_job_failed(cache_key);
      return;
    }

    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.thumbnail_cache.insert_or_assign(
      cache_key,
      ThumbnailEntry{
        .texture = std::move(thumbnail_texture),
        .last_used = self.frame_counter.load(std::memory_order_relaxed)
      }
    );
    self.active_jobs.erase(cache_key);
  }));
  job_man.pop_job_name();
}

// `pack_path` is either the importer's full-size pack, which gets trimmed down and the trim kept for
// next time, or a trim this function wrote on an earlier run. Reading the source pack costs its whole
// size on disk -- tens of megabytes for a 4K texture -- so it is worth doing exactly once per file.
auto ThumbnailManager::submit_pack_load(
  this ThumbnailManager& self, const std::string& cache_key, const std::filesystem::path& pack_path, const bool cached
) -> void {
  auto& job_man = App::get_job_manager();
  job_man.push_job_name(cached ? "ContentPanelThumbnail_CachedPackLoad" : "ContentPanelThumbnail_CompiledTextureLoad");
  job_man.submit(Job::create([&self, pack_path, cache_key, cached]() {
    auto pack = AssetFile::unpack(pack_path);
    if (!pack) {
      self.mark_job_failed(cache_key);
      return;
    }

    const TextureData* texture_data = nullptr;
    for (auto& entry : pack->entries) {
      if (auto* data = std::get_if<TextureData>(&entry.data)) {
        texture_data = data;
        break;
      }
    }

    if (!texture_data || texture_data->mips.empty()) {
      OX_LOG_ERROR("Compiled texture '{}' holds no image data.", pack_path);
      self.mark_job_failed(cache_key);
      return;
    }

    auto trimmed = cached ? TextureData(*texture_data) : trim_to_thumbnail_mips(*texture_data, THUMBNAIL_SIZE);
    if (!cached) {
      write_thumbnail_pack(self.cache_dir / (cache_key + ".oxpack"), trimmed);
    }

    auto thumbnail_texture = Texture::create(std::move(trimmed), TextureLoadInfo{});
    if (!thumbnail_texture) {
      self.mark_job_failed(cache_key);
      return;
    }

    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.thumbnail_cache.insert_or_assign(
      cache_key,
      ThumbnailEntry{
        .texture = std::move(thumbnail_texture),
        .last_used = self.frame_counter.load(std::memory_order_relaxed)
      }
    );
    self.active_jobs.erase(cache_key);
  }));
  job_man.pop_job_name();
}

auto ThumbnailManager::get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> TextureView {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return {};
  }

  auto asset_hash = self.get_asset_hash(asset_path);

  if (auto cached = self.find_cached(asset_hash)) {
    return *cached;
  }

  if (!self.try_claim_job(asset_hash)) {
    return {};
  }

  // KTX2 and DDS are cooked offline now, so their source bytes mean nothing to the engine; the
  // preview comes out of the pack the importer produced instead.
  if (needs_compiling(asset_path)) {
    // the trim kept from an earlier look at this file, which is a few kilobytes against the source
    // pack's tens of megabytes
    auto cached_pack = self.cache_dir / (asset_hash + ".oxpack");
    if (std::filesystem::exists(cached_pack)) {
      self.submit_pack_load(asset_hash, cached_pack, true);
      return {};
    }

    auto& asset_man = App::mod<AssetManager>();
    const auto uuid = import_asset(asset_man, asset_path);

    auto pack_path = std::filesystem::path();
    if (auto asset = asset_man.get_asset(uuid)) {
      pack_path = asset->path;
    }

    if (pack_path.empty()) {
      self.mark_job_failed(asset_hash);
      return {};
    }

    self.submit_pack_load(asset_hash, pack_path, false);

    return {};
  }

  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_TextureLoad");
  job_man.submit(Job::create([&self, asset_path, asset_hash]() {
    auto thumbnail_texture = Texture::create({
      .source = asset_path,
      .target_width = THUMBNAIL_SIZE,
      .target_height = THUMBNAIL_SIZE,
    });
    if (!thumbnail_texture) {
      self.mark_job_failed(asset_hash);
      return;
    }

    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.thumbnail_cache.insert_or_assign(
      asset_hash,
      ThumbnailEntry{
        .texture = std::move(thumbnail_texture),
        .last_used = self.frame_counter.load(std::memory_order_relaxed)
      }
    );
    self.active_jobs.erase(asset_hash);
  }));
  job_man.pop_job_name();

  return {};
}

auto ThumbnailManager::get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> TextureView {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return {};
  }

  auto asset_hash = self.get_asset_hash(asset_path);

  if (auto cached = self.find_cached(asset_hash)) {
    return *cached;
  }

  if (!self.try_claim_job(asset_hash)) {
    return {};
  }

  auto expected_png = self.cache_dir / (asset_hash + ".png");
  if (std::filesystem::exists(expected_png)) {
    self.submit_cached_png_load(asset_hash, expected_png);
    return {};
  }

  auto& asset_man = App::mod<AssetManager>();
  auto model_uuid = import_asset(asset_man, asset_path);
  if (!model_uuid) {
    self.release_job(asset_hash);
    return {};
  }

  auto lock = std::unique_lock(self.queue_mutex);
  self.pending_renders.push({
    .cache_key = asset_hash,
    .asset_uuid = model_uuid,
    .expected_png = expected_png,
  });

  return {};
}

auto ThumbnailManager::get_thumbnail_material(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> TextureView {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return {};
  }

  return self.material_thumbnail_for(self.resolve_material_uuid(asset_path), asset_path);
}

auto ThumbnailManager::get_thumbnail_material(this ThumbnailManager& self, const UUID& material_uuid) -> TextureView {
  ZoneScoped;

  auto asset_path = std::filesystem::path{};
  if (auto asset = App::mod<AssetManager>().get_asset(material_uuid)) {
    asset_path = asset->path;
  }

  return self.material_thumbnail_for(material_uuid, asset_path);
}

auto ThumbnailManager::material_thumbnail_for(
  this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& asset_path
) -> TextureView {
  ZoneScoped;

  if (!material_uuid) {
    return {};
  }

  const auto meta_path = material_meta_path(asset_path);
  const auto has_file = !meta_path.empty() && std::filesystem::exists(meta_path);
  // While there are unsaved edits the file cache is not usable in either direction: its key describes
  // the file on disk, which no longer matches what we would render.
  const auto use_file_cache = has_file && !self.has_unsaved_edits(material_uuid, meta_path);
  const auto cache_key = use_file_cache ? self.get_asset_hash(meta_path) : live_cache_key(material_uuid);

  if (auto cached = self.find_cached(cache_key)) {
    return *cached;
  }

  if (!self.try_claim_job(cache_key)) {
    return {};
  }

  // Left empty for live previews, which is what stops update() from persisting them.
  auto expected_png = use_file_cache ? self.cache_dir / (cache_key + ".png") : std::filesystem::path{};
  if (use_file_cache && std::filesystem::exists(expected_png)) {
    self.submit_cached_png_load(cache_key, expected_png);
    return {};
  }

  if (!self.acquire_asset(material_uuid)) {
    self.release_job(cache_key);
    return {};
  }

  auto lock = std::unique_lock(self.queue_mutex);
  self.pending_renders.push({
    .cache_key = cache_key,
    .asset_uuid = material_uuid,
    .expected_png = expected_png,
  });

  return {};
}

auto ThumbnailManager::get_thumbnail_terrain(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> TextureView {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return {};
  }

  auto asset_hash = self.get_asset_hash(asset_path);

  if (auto cached = self.find_cached(asset_hash)) {
    return *cached;
  }

  if (!self.try_claim_job(asset_hash)) {
    return {};
  }

  auto expected_png = self.cache_dir / (asset_hash + ".png");
  if (std::filesystem::exists(expected_png)) {
    self.submit_cached_png_load(asset_hash, expected_png);
    return {};
  }

  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_TerrainRelief");
  job_man.submit(Job::create([&self, asset_path, asset_hash, expected_png]() {
    auto edits = TerrainEdits::read(asset_path);
    if (!edits) {
      self.mark_job_failed(asset_hash);
      return;
    }

    auto pixels = render_terrain_preview(*edits, THUMBNAIL_SIZE);
    if (pixels.empty()) {
      self.mark_job_failed(asset_hash);
      return;
    }

    write_thumbnail_png(expected_png, pixels, THUMBNAIL_SIZE);

    auto lock = std::unique_lock(self.queue_mutex);
    self.pending_uploads.push({.cache_key = asset_hash, .pixels = std::move(pixels)});
  }));
  job_man.pop_job_name();

  return {};
}

auto ThumbnailManager::thumbnail_unavailable(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> bool {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return true;
  }

  auto lock = std::shared_lock(self.thumbnail_mutex);

  return self.failed_jobs.contains(self.get_asset_hash(asset_path));
}

auto ThumbnailManager::invalidate_material(this ThumbnailManager& self, const UUID& material_uuid) -> void {
  ZoneScoped;

  if (!material_uuid) {
    return;
  }

  auto meta_path = std::filesystem::path{};
  if (auto asset = App::mod<AssetManager>().get_asset(material_uuid)) {
    meta_path = material_meta_path(asset->path);
  }

  const auto live_key = live_cache_key(material_uuid);

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.erase(live_key);

  // An edit can land while a render for this material is already running, which is normal when
  // dragging a slider. That render's result is already out of date, so mark it to be thrown away.
  if (self.active_jobs.contains(live_key)) {
    self.superseded_jobs.insert(live_key);
  }

  // Drop the saved-state preview from memory as well, so the UI switches to the live one straight
  // away, and remember the file's current mtime so we can tell when the edits get written out.
  if (!meta_path.empty() && std::filesystem::exists(meta_path)) {
    self.thumbnail_cache.erase(self.get_asset_hash(meta_path));
    self.unsaved_materials.insert_or_assign(material_uuid, file_mtime(meta_path));
  } else {
    self.unsaved_materials.insert_or_assign(material_uuid, 0);
  }
}

auto ThumbnailManager::material_is_being_edited(this ThumbnailManager& self, const UUID& material_uuid) -> bool {
  auto lock = std::shared_lock(self.thumbnail_mutex);
  return self.unsaved_materials.contains(material_uuid);
}

auto ThumbnailManager::has_unsaved_edits(
  this ThumbnailManager& self, const UUID& material_uuid, const std::filesystem::path& meta_path
) -> bool {
  ZoneScoped;

  auto lock = std::unique_lock(self.thumbnail_mutex);
  const auto it = self.unsaved_materials.find(material_uuid);
  if (it == self.unsaved_materials.end()) {
    return false;
  }

  // A material with no file of its own can only ever be previewed live.
  if (meta_path.empty()) {
    return true;
  }

  // The file moved on, so the edits were saved and the on-disk cache is authoritative again.
  if (file_mtime(meta_path) != it->second) {
    self.unsaved_materials.erase(it);
    return false;
  }

  return true;
}

auto ThumbnailManager::render_model_thumbnail(this ThumbnailManager& self, const UUID& model_uuid, u32 size)
  -> option<std::vector<u8>> {
  ZoneScoped;

  if (!model_uuid) {
    return nullopt;
  }

  if (!self.acquire_asset(model_uuid)) {
    return nullopt;
  }

  auto thumbnail_scene = Scene("ThumbnailScene");
  thumbnail_scene.create_model_entity(model_uuid);

  App::mod<Renderer>().sync_materials();

  auto& asset_man = App::mod<AssetManager>();
  auto model_asset = asset_man.get_model(model_uuid);
  if (!model_asset) {
    return nullopt;
  }

  const auto camera_transform = ThumbnailCamera::calculate_from_model(*model_asset.value, MODEL_PREVIEW_FOV, 1.0f);
  model_asset.reset();

  const auto sun = thumbnail_scene.create_entity("sun", true);
  sun
    .set<TransformComponent>({
      .rotation = glm::quat(glm::vec3(glm::radians(45.f), glm::radians(-145.f), 0.f)),
    })
    .set<LightComponent>({
      .type = LightComponent::LightType::Directional,
      .intensity = 20.f,
      .cast_shadows = false,
    })
    .set<SkyComponent>(SkyComponent{
      .solid_color = glm::vec4(0.f, 0.f, 0.f, 1.0f),
      .ambient_color = glm::vec3(0.25f),
      .texture = UUID(nullptr),
    })
    .set<AutoExposureComponent>({.adaptation_speed = 1.0e6f});

  const auto camera = thumbnail_scene.create_entity("camera", true);
  camera.set<CameraComponent>({
    .fov = MODEL_PREVIEW_FOV,
    .far_clip = camera_transform.far_clip,
    .near_clip = camera_transform.near_clip,
    .position = camera_transform.position,
  });
  camera.set<TransformComponent>({
    .position = camera_transform.position,
    .rotation = camera_transform.rotation,
  });

  thumbnail_scene.renderer_cvar.cvar_enable_debug_renderer.set(false);

  return self.render_scene(thumbnail_scene, size);
}

auto ThumbnailManager::render_material_thumbnail(this ThumbnailManager& self, const UUID& material_uuid, u32 size)
  -> option<std::vector<u8>> {
  ZoneScoped;

  if (!material_uuid) {
    return nullopt;
  }

  if (!self.ensure_material_preview()) {
    return nullopt;
  }

  auto& sphere = self.material_preview->sphere;
  auto& mesh_component = sphere.ensure<MeshComponent>();
  mesh_component.material_uuid = material_uuid;
  sphere.modified<MeshComponent>();

  App::mod<Renderer>().sync_materials();

  return self.render_scene(*self.material_preview->scene, size);
}

auto ThumbnailManager::ensure_material_preview(this ThumbnailManager& self) -> bool {
  ZoneScoped;

  if (self.material_preview) {
    return true;
  }

  auto& asset_man = App::mod<AssetManager>();

  auto sphere_model_uuid = asset_man.create_asset(AssetType::Model);
  if (!sphere_model_uuid) {
    OX_LOG_ERROR("Couldn't create the material preview sphere asset!");
    return false;
  }

  auto sphere_request = generate_uv_sphere(PREVIEW_SPHERE_RADIUS, PREVIEW_SPHERE_RINGS, PREVIEW_SPHERE_SECTORS);
  auto sphere_data = App::mod<rc::ResourceCompiler>().process(sphere_request);
  if (!sphere_data || !asset_man.load_asset(sphere_model_uuid, std::move(sphere_data.value()))) {
    OX_LOG_ERROR("Couldn't build the material preview sphere mesh!");
    asset_man.delete_asset(sphere_model_uuid);
    return false;
  }

  auto preview = std::make_unique<MaterialPreview>();
  preview->sphere_model_uuid = sphere_model_uuid;
  preview->scene = std::make_unique<Scene>("MaterialPreviewScene");

  auto& scene = *preview->scene;

  const auto sphere_aabb = AABB(glm::vec3(-PREVIEW_SPHERE_RADIUS), glm::vec3(PREVIEW_SPHERE_RADIUS));
  preview->sphere = scene.create_entity("preview_sphere", true);
  preview->sphere.set<TransformComponent>({});
  preview->sphere.set<MeshComponent>({
    .model_uuid = sphere_model_uuid,
    .mesh_index = 0,
    .material_uuid = UUID(nullptr),
    .cast_shadows = false,
    .baked_aabb = sphere_aabb,
  });

  const auto sun_direction = glm::normalize(glm::vec3(-0.45f, 0.78f, -0.44f));
  const auto sun = scene.create_entity("preview_sun", true);
  sun
    .set<TransformComponent>({
      .rotation = glm::quatLookAt(sun_direction, glm::vec3(0.f, 1.f, 0.f)),
    })
    .set<LightComponent>({
      .type = LightComponent::LightType::Directional,
      .intensity = PREVIEW_SUN_INTENSITY,
      .cast_shadows = false,
    })
    .set<SkyComponent>({
      .solid_color = glm::vec4(0.f),
      .ambient_color = glm::vec3(PREVIEW_AMBIENT),
      .texture = UUID(nullptr),
    });

  const auto camera_distance = PREVIEW_SPHERE_RADIUS / std::sin(glm::radians(PREVIEW_FOV * 0.5f)) *
                               PREVIEW_FRAMING_PADDING;
  const auto camera_position = glm::vec3(-camera_distance, 0.f, 0.f);
  const auto camera_direction = glm::normalize(-camera_position);
  const auto camera = scene.create_entity("preview_camera", true);
  camera.set<CameraComponent>({
    .fov = PREVIEW_FOV,
    .far_clip = camera_distance + PREVIEW_SPHERE_RADIUS * 4.f,
    .near_clip = 0.01f,
    .position = camera_position,
  });
  camera.set<TransformComponent>({
    .position = camera_position,
    .rotation = glm::quatLookAt(camera_direction, glm::vec3(0.f, 1.f, 0.f)),
  });

  scene.renderer_cvar.cvar_enable_debug_renderer.set(false);
  scene.renderer_cvar.cvar_draw_bounding_boxes.set(false);
  scene.renderer_cvar.cvar_bloom_enable.set(false);
  scene.renderer_cvar.cvar_vbgtao_enable.set(false);
  scene.renderer_cvar.cvar_contact_shadows_enabled.set(false);
  scene.renderer_cvar.cvar_transparent_background.set(true);
  scene.renderer_cvar.cvar_tonemapper.set(static_cast<i32>(GPU::TonemapType::AgX));
  scene.renderer_cvar.cvar_exposure.set(PREVIEW_EXPOSURE);

  self.material_preview = std::move(preview);

  return true;
}

auto ThumbnailManager::render_scene(this ThumbnailManager& self, Scene& scene, u32 size) -> option<std::vector<u8>> {
  ZoneScoped;

  auto* renderer_instance = scene.get_renderer_instance();
  if (!renderer_instance) {
    return nullopt;
  }

  auto& render_context = App::get_rendercontext();

  auto thumbnail_image = vuk::declare_ia(
    "thumbnail_image",
    {
      .image_type = vuk::ImageType::e2D,
      .extent = vuk::Extent3D{size, size, 1u},
      .format = vuk::Format::eR8G8B8A8Srgb,
      .sample_count = vuk::Samples::e1,
      .base_level = 0,
      .level_count = 1,
      .base_layer = 0,
      .layer_count = 1,
    }
  );
  thumbnail_image = vuk::clear_image(std::move(thumbnail_image), vuk::Transparent<f32>);

  scene.runtime_update(App::get_timestep());

  // Builds this tick's uploads; `render` below submits them. `Scene::render` would do this itself,
  // but this path drives the renderer instance directly.
  scene.prepare_render();

  auto scene_view_image = renderer_instance->render(
    std::move(thumbnail_image),
    {},
    glm::ivec2(size),
    glm::ivec2(size),
    scene.renderer_cvar
  );

  const auto buffer_size = static_cast<usize>(size) * size * 4; // RGBA8
  auto readback_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, buffer_size);
  readback_buffer = vuk::copy(scene_view_image, readback_buffer);

  {
    auto lock = std::unique_lock(render_context.queue_mutex);
    readback_buffer.wait(*render_context.frame_allocator, render_context.get_compiler());
  }

  auto pixel_data = std::vector<u8>(buffer_size);
  std::memcpy(pixel_data.data(), readback_buffer->mapped_ptr, buffer_size);

  return pixel_data;
}

auto ThumbnailManager::resolve_material_uuid(this ThumbnailManager& self, const std::filesystem::path& path) -> UUID {
  ZoneScoped;

  {
    auto lock = std::shared_lock(self.material_uuids_mutex);
    const auto it = self.material_uuids.find(path);
    if (it != self.material_uuids.end()) {
      return it->second;
    }
  }

  const auto uuid = import_asset(App::mod<AssetManager>(), path);

  auto lock = std::unique_lock(self.material_uuids_mutex);
  self.material_uuids.insert_or_assign(path, uuid);

  return uuid;
}

auto ThumbnailManager::get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path)
  -> std::string {
  ZoneScoped;
  memory::ScopedStack stack;

  auto last_write = std::filesystem::last_write_time(path).time_since_epoch().count();
  auto signature = stack.format("{}{}", path.string(), last_write);

  return fmt::format("{:016X}", fnv64_str(signature));
}
} // namespace ox
