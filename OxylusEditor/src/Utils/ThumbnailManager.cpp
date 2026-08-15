#include "ThumbnailManager.hpp"

#include <glm/gtx/quaternion.hpp>
#include <numbers>
#include <stb_image_write.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/JobManager.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
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

struct MaterialPreview {
  std::unique_ptr<Scene> scene = {};
  flecs::entity sphere = {};
  UUID sphere_model_uuid = UUID(nullptr);
};

static auto generate_uv_sphere(f32 radius, u32 rings, u32 sectors) -> ModelLoadInfo {
  auto model = ModelLoadInfo{};

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

      model.vertices.emplace_back(
        ModelLoadInfo::Vertex{
          .position = normal * radius,
          .normal = normal,
          .uv = glm::vec2(u, v),
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

static auto material_meta_path(const std::filesystem::path& asset_path) -> std::filesystem::path {
  if (!AssetManager::owns_meta_file(asset_path)) {
    return {};
  }

  return AssetManager::meta_file_path(asset_path);
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

auto ThumbnailManager::update(this ThumbnailManager& self) -> void {
  ZoneScoped;

  if (!App::get_rendercontext().frame_allocator.has_value()) {
    return;
  }

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

  auto pixels = render_job->kind == RenderKind::Material
                  ? self.render_material_thumbnail(render_job->asset_uuid, THUMBNAIL_SIZE)
                  : self.render_model_thumbnail(render_job->asset_uuid, THUMBNAIL_SIZE);

  if (!pixels.has_value() || pixels->empty()) {
    self.release_job(render_job->cache_key);
    return;
  }

  if (!render_job->expected_png.empty()) {
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("ContentPanelThumbnail_WritePNG");
    job_man.submit(Job::create([expected_png = render_job->expected_png, pixel_bytes = *pixels]() {
      stbi_write_png(
        expected_png.string().c_str(),
        THUMBNAIL_SIZE,
        THUMBNAIL_SIZE,
        4,
        pixel_bytes.data(),
        THUMBNAIL_SIZE * 4
      );
    }));
    job_man.pop_job_name();
  }

  auto thumbnail_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Srgb,
    .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  thumbnail_texture.upload(pixels.value(), vuk::eFragmentSampled);

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.insert_or_assign(render_job->cache_key, std::move(thumbnail_texture));
  self.active_jobs.erase(render_job->cache_key);
}

auto ThumbnailManager::reset(this ThumbnailManager& self) -> void {
  ZoneScoped;

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
}

auto ThumbnailManager::find_cached(this ThumbnailManager& self, const std::string& cache_key) -> option<TextureView> {
  auto lock = std::shared_lock(self.thumbnail_mutex);
  auto it = self.thumbnail_cache.find(cache_key);
  if (it != self.thumbnail_cache.end()) {
    return it->second.view();
  }

  return nullopt;
}

auto ThumbnailManager::try_claim_job(this ThumbnailManager& self, const std::string& cache_key) -> bool {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  if (self.active_jobs.contains(cache_key)) {
    return false;
  }
  self.active_jobs.insert(cache_key);

  return true;
}

auto ThumbnailManager::release_job(this ThumbnailManager& self, const std::string& cache_key) -> void {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.active_jobs.erase(cache_key);
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

    auto lock = std::unique_lock(self.thumbnail_mutex);
    if (thumbnail_texture) {
      self.thumbnail_cache.insert_or_assign(cache_key, std::move(thumbnail_texture));
    }
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

  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_TextureLoad");
  job_man.submit(Job::create([&self, asset_path, asset_hash]() {
    auto thumbnail_texture = Texture::create({
      .source = asset_path,
      .target_width = THUMBNAIL_SIZE,
      .target_height = THUMBNAIL_SIZE,
    });

    auto lock = std::unique_lock(self.thumbnail_mutex);
    if (thumbnail_texture) {
      self.thumbnail_cache.insert_or_assign(asset_hash, std::move(thumbnail_texture));
    }
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
  auto model_uuid = asset_man.import_asset(asset_path);
  if (!model_uuid) {
    self.release_job(asset_hash);
    return {};
  }

  auto lock = std::unique_lock(self.queue_mutex);
  self.pending_renders.push({
    .cache_key = asset_hash,
    .kind = RenderKind::Model,
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
  const auto cache_key = has_file ? self.get_asset_hash(meta_path) : live_cache_key(material_uuid);

  if (auto cached = self.find_cached(cache_key)) {
    return *cached;
  }

  if (!self.try_claim_job(cache_key)) {
    return {};
  }

  auto expected_png = has_file ? self.cache_dir / (cache_key + ".png") : std::filesystem::path{};
  if (has_file && std::filesystem::exists(expected_png)) {
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
    .kind = RenderKind::Material,
    .asset_uuid = material_uuid,
    .expected_png = expected_png,
  });

  return {};
}

auto ThumbnailManager::invalidate_material(this ThumbnailManager& self, const UUID& material_uuid) -> void {
  ZoneScoped;

  if (!material_uuid) {
    return;
  }

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.erase(live_cache_key(material_uuid));
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

  auto sphere_data = generate_uv_sphere(PREVIEW_SPHERE_RADIUS, PREVIEW_SPHERE_RINGS, PREVIEW_SPHERE_SECTORS);
  if (!asset_man.load_asset(sphere_model_uuid, std::move(sphere_data))) {
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

  // Builds this tick's uploads; `render` below submits them.
  scene.runtime_update(App::get_timestep());

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

  const auto uuid = App::mod<AssetManager>().import_asset(path);

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
