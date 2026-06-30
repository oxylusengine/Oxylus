#include "ThumbnailManager.hpp"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"

namespace ox {

auto ThumbnailManager::init(this ThumbnailManager& self) -> void {
  ZoneScoped;

  self.cache_dir = std::filesystem::current_path() / ".oxeditor/thumbnails";
  if (!std::filesystem::exists(self.cache_dir)) {
    std::filesystem::create_directories(self.cache_dir);
  }
}

auto ThumbnailManager::update(this ThumbnailManager& self) -> void {
  ZoneScoped;

  std::vector<PendingUpload> uploads_to_process;
  {
    std::lock_guard lock(self.queue_mutex);
    uploads_to_process = std::move(self.pending_uploads);
    self.pending_uploads.clear();
  }

  for (const auto& upload : uploads_to_process) {
    auto thumbnail_texture = std::make_shared<Texture>();
    thumbnail_texture->create(
      {},
      TextureLoadInfo{
        .loaded_data = const_cast<u8*>(upload.pixel_data.data()),
        .extent = vuk::Extent3D{upload.size, upload.size, 1}
      }
    );

    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.thumbnail_cache.insert_or_assign(upload.asset_hash, thumbnail_texture);
    self.active_jobs.erase(upload.asset_hash);
  }

  PendingMeshRender render_job;
  bool has_render_job = false;
  {
    std::lock_guard lock(self.queue_mutex);
    if (!self.pending_mesh_renders.empty()) {
      render_job = self.pending_mesh_renders.front();
      self.pending_mesh_renders.pop();
      has_render_job = true;
    }
  }

  if (has_render_job) {
    auto pixels = self.render_thumbnail(render_job.model_uuid, THUMBNAIL_SIZE);

    if (pixels.has_value() && !pixels->empty()) {
      stbi_write_png(
        render_job.expected_png.string().c_str(),
        THUMBNAIL_SIZE,
        THUMBNAIL_SIZE,
        4,
        pixels->data(),
        THUMBNAIL_SIZE * 4
      );

      auto thumbnail_texture = std::make_shared<Texture>();
      thumbnail_texture->create(
        {},
        TextureLoadInfo{.loaded_data = pixels->data(), .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1}}
      );

      auto lock = std::unique_lock(self.thumbnail_mutex);
      self.thumbnail_cache.insert_or_assign(render_job.asset_hash, thumbnail_texture);
      self.active_jobs.erase(render_job.asset_hash);
    } else {
      auto lock = std::unique_lock(self.thumbnail_mutex);
      self.active_jobs.erase(render_job.asset_hash);
    }
  }
}

auto ThumbnailManager::get_thumbnail_texture(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> option<std::shared_ptr<Texture>> {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return nullopt;
  }

  auto asset_hash = self.get_asset_hash(asset_path);

  {
    auto read_lock = std::shared_lock(self.thumbnail_mutex);
    if (self.thumbnail_cache.contains(asset_hash)) {
      return self.thumbnail_cache[asset_hash];
    }
  }

  {
    auto lock = std::unique_lock(self.thumbnail_mutex);
    if (self.active_jobs.contains(asset_hash)) {
      return nullopt;
    }
    self.active_jobs.insert(asset_hash);
  }

  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_TextureLoad");
  job_man.submit(Job::create([&self, asset_path, asset_hash]() {
    i32 width, height, chans;
    u8* raw_pixels = stbi_load(asset_path.string().c_str(), &width, &height, &chans, 4);

    if (raw_pixels) {
      std::vector<u8> pixel_data;
      u32 actual_size = THUMBNAIL_SIZE;

      if (width > (i32)THUMBNAIL_SIZE || height > (i32)THUMBNAIL_SIZE) {
        pixel_data.resize(THUMBNAIL_SIZE * THUMBNAIL_SIZE * 4);
        stbir_resize_uint8_linear(
          raw_pixels,
          width,
          height,
          0,
          pixel_data.data(),
          THUMBNAIL_SIZE,
          THUMBNAIL_SIZE,
          0,
          STBIR_RGBA
        );
        stbi_image_free(raw_pixels);
      } else {
        pixel_data.assign(raw_pixels, raw_pixels + (width * height * 4));
        actual_size = static_cast<u32>(width);
        stbi_image_free(raw_pixels);
      }

      std::lock_guard lock(self.queue_mutex);
      self.pending_uploads.push_back(
        {.asset_hash = asset_hash, .pixel_data = std::move(pixel_data), .size = actual_size}
      );
    } else {
      std::lock_guard lock(self.thumbnail_mutex);
      self.active_jobs.erase(asset_hash);
    }
  }));
  job_man.pop_job_name();

  return nullopt;
}

auto ThumbnailManager::get_thumbnail_model(this ThumbnailManager& self, const std::filesystem::path& asset_path)
  -> option<std::shared_ptr<Texture>> {
  ZoneScoped;

  if (!std::filesystem::exists(asset_path)) {
    return nullopt;
  }

  auto asset_hash = self.get_asset_hash(asset_path);

  {
    auto read_lock = std::shared_lock(self.thumbnail_mutex);
    if (self.thumbnail_cache.contains(asset_hash)) {
      return self.thumbnail_cache[asset_hash];
    }
  }

  {
    auto lock = std::unique_lock(self.thumbnail_mutex);
    if (self.active_jobs.contains(asset_hash)) {
      return nullopt;
    }
    self.active_jobs.insert(asset_hash);
  }

  auto expected_png = self.cache_dir / (asset_hash + ".png");

  if (std::filesystem::exists(expected_png)) {
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("ContentPanelThumbnail_ModelCacheLoad");
    job_man.submit(Job::create([&self, expected_png, asset_hash]() {
      int width, height, chans;
      u8* raw_pixels = stbi_load(expected_png.string().c_str(), &width, &height, &chans, 4);

      if (raw_pixels) {
        std::vector<u8> pixel_data(raw_pixels, raw_pixels + (width * height * 4));
        stbi_image_free(raw_pixels);

        std::lock_guard lock(self.queue_mutex);
        self.pending_uploads.push_back(
          {.asset_hash = asset_hash, .pixel_data = std::move(pixel_data), .size = static_cast<u32>(width)}
        );
      } else {
        std::lock_guard lock(self.thumbnail_mutex);
        self.active_jobs.erase(asset_hash);
      }
    }));
    job_man.pop_job_name();

    return nullopt;
  }

  auto& am = App::mod<AssetManager>();
  auto model_uuid = am.import_asset(asset_path);
  if (!model_uuid) {
    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.active_jobs.erase(asset_hash);
    return nullopt;
  }

  {
    std::lock_guard lock(self.queue_mutex);
    self.pending_mesh_renders.push({.asset_hash = asset_hash, .model_uuid = model_uuid, .expected_png = expected_png});
  }

  return nullopt;
}

auto ThumbnailManager::render_thumbnail(this ThumbnailManager& self, UUID model_uuid, u32 size)
  -> option<std::vector<u8>> {
  ZoneScoped;

  if (!model_uuid) {
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

  auto thumbnail_scene = Scene("ThumbnailScene");
  auto model_entity = thumbnail_scene.create_model_entity(model_uuid);

  auto& asset_man = App::mod<AssetManager>();
  auto model_asset = asset_man.get_model(model_uuid);

  const auto sun = thumbnail_scene.create_entity("sun", true);
  sun.set<TransformComponent>({
    .rotation = glm::quat(glm::vec3(glm::radians(45.f), glm::radians(90.f), 0.f)),
  });
  sun.set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 10.f})
    .add<AtmosphereComponent>();
  sun.set<AutoExposureComponent>({});

  glm::vec3 out_pos;
  glm::quat out_rot;
  f32 out_near, out_far;
  self.calculate_thumbnail_camera(AABB(model_asset->get_base_aabb()), out_pos, out_rot, out_near, out_far);

  const auto camera = thumbnail_scene.create_entity("camera", true);
  camera.set<CameraComponent>({CameraComponent{
    .fov = 40.f,
    .far_clip = out_far,
    .near_clip = out_near,
  }});
  camera.set<TransformComponent>(TransformComponent{
    .position = out_pos,
    .rotation = out_rot,
  });

  auto& ts = App::get_timestep();
  thumbnail_scene.runtime_update(ts);

  const Renderer::RenderInfo render_info = {};
  auto renderer_instance = thumbnail_scene.get_renderer_instance();
  auto scene_view_image = renderer_instance->render(std::move(thumbnail_image), render_info);

  usize buffer_size = size * size * 4; // RGBA8
  auto readback_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, buffer_size);

  readback_buffer = vuk::copy(scene_view_image, readback_buffer);

  auto temp_compiler = vuk::Compiler{};
  readback_buffer.wait(*render_context.frame_allocator, temp_compiler);

  std::vector<u8> pixel_data(buffer_size);
  std::memcpy(pixel_data.data(), readback_buffer->mapped_ptr, buffer_size);

  return pixel_data;
}

auto ThumbnailManager::get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path)
  -> std::string {
  ZoneScoped;

  auto last_write = std::filesystem::last_write_time(path).time_since_epoch().count();
  std::string signature = path.string() + fmt::format("{}", last_write);
  size_t hash_val = std::hash<std::string>{}(signature);
  return fmt::format("{:X}", hash_val);
}

auto ThumbnailManager::calculate_thumbnail_camera(
  this ThumbnailManager& self,
  const ox::AABB& model_aabb,
  glm::vec3& out_position,
  glm::quat& out_rotation,
  f32& out_near,
  f32& out_far
) -> void {
  ZoneScoped;

  glm::vec3 extents = model_aabb.get_extents();
  f32 radius = glm::max(0.1f, glm::length(extents));

  f32 fov_radians = glm::radians(45.0f);
  f32 distance = (radius / glm::sin(fov_radians * 0.5f)) * 1.3f;

  glm::vec3 target = glm::vec3(0.0f);

  f32 pitch = glm::radians(25.0f);
  f32 yaw = glm::radians(35.0f);

  glm::vec3 offset_direction = glm::vec3(
    glm::cos(pitch) * glm::sin(yaw), // X
    glm::sin(pitch),                 // Y
    glm::cos(pitch) * glm::cos(yaw)  // Z
  );

  out_position = target + offset_direction * distance;

  glm::mat4 view_matrix = glm::lookAt(out_position, target, glm::vec3(0.0f, 1.0f, 0.0f));
  out_rotation = glm::quat_cast(glm::inverse(view_matrix));

  out_near = glm::max(0.01f, distance - (radius * 2.0f));
  out_far = distance + (radius * 2.0f);
}

} // namespace ox
