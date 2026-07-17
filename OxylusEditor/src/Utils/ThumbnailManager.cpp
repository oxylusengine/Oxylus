#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "ThumbnailManager.hpp"

#include <imgui.h>
#include <stb_image_write.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "ResourceCompiler.hpp"
#include "Utils/ThumbnailCamera.hpp"

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

  if (!has_render_job) {
    return;
  }

  auto pixels = self.render_thumbnail(render_job.model_uuid, THUMBNAIL_SIZE);

  if (!pixels.has_value() || pixels->empty()) {
    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.active_jobs.erase(render_job.asset_hash);
    return;
  }

  memory::ScopedStack stack;
  auto& job_man = App::get_job_manager();
  job_man.push_job_name("ContentPanelThumbnail_WritePNG");
  job_man.submit(Job::create([expected_png = render_job.expected_png, pixel_bytes = *pixels]() {
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

  auto thumbnail_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Srgb,
    .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  thumbnail_texture.upload(pixels.value(), vuk::eFragmentSampled);

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.insert_or_assign(render_job.asset_hash, std::move(thumbnail_texture));
  self.active_jobs.erase(render_job.asset_hash);
}

auto ThumbnailManager::reset(this ThumbnailManager& self) -> void {
  ZoneScoped;

  if (std::filesystem::exists(self.cache_dir)) {
    std::filesystem::remove_all(self.cache_dir);
  }

  self.init();

  auto lock = std::unique_lock(self.thumbnail_mutex);
  self.thumbnail_cache.clear();
}

auto ThumbnailManager::find_cached(this ThumbnailManager& self, const std::string& asset_hash) -> option<TextureView> {
  auto lock = std::shared_lock(self.thumbnail_mutex);
  auto it = self.thumbnail_cache.find(asset_hash);
  if (it != self.thumbnail_cache.end()) {
    return it->second.view();
  }
  return nullopt;
}

auto ThumbnailManager::try_claim_job(this ThumbnailManager& self, const std::string& asset_hash) -> bool {
  auto lock = std::unique_lock(self.thumbnail_mutex);
  if (self.active_jobs.contains(asset_hash)) {
    return false;
  }
  self.active_jobs.insert(asset_hash);
  return true;
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
    memory::ScopedStack stack;
    auto& rc = App::mod<rc::ResourceCompiler>();
    auto processed_texture = rc.process(
      rc::TextureCompileRequest{
        .path = asset_path,
        .target_width = THUMBNAIL_SIZE,
        .target_height = THUMBNAIL_SIZE,
      }
    );

    auto lock = std::unique_lock(self.thumbnail_mutex);
    if (processed_texture.has_value()) {
      auto thumbnail_texture = Texture::create({
        .format = processed_texture->format,
        .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1},
        .usage = vuk::ImageUsageFlagBits::eSampled,
      });
      auto mips = stack.alloc<std::span<const u8>>(processed_texture->mips.size());
      for (const auto& [mip, processed_mip] : std::views::zip(mips, processed_texture->mips)) {
        mip = std::span(processed_mip.pixels);
      }
      thumbnail_texture.upload_mips(mips, vuk::eFragmentSampled);
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
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("ContentPanelThumbnail_ModelCacheLoad");
    job_man.submit(Job::create([&self, expected_png, asset_hash]() {
      auto& rc = App::mod<rc::ResourceCompiler>();
      auto processed_texture = rc.process(
        rc::TextureCompileRequest{
          .path = expected_png,
          .target_width = THUMBNAIL_SIZE,
          .target_height = THUMBNAIL_SIZE,
        }
      );

      auto lock = std::unique_lock(self.thumbnail_mutex);
      if (processed_texture.has_value()) {
        memory::ScopedStack stack;
        auto thumbnail_texture = Texture::create({
          .format = processed_texture->format,
          .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1},
          .usage = vuk::ImageUsageFlagBits::eSampled,
        });
        auto mips = stack.alloc<std::span<const u8>>(processed_texture->mips.size());
        for (const auto& [mip, processed_mip] : std::views::zip(mips, processed_texture->mips)) {
          mip = std::span(processed_mip.pixels);
        }
        thumbnail_texture.upload_mips(mips, vuk::eFragmentSampled);
        self.thumbnail_cache.insert_or_assign(asset_hash, std::move(thumbnail_texture));
      }
      self.active_jobs.erase(asset_hash);
    }));
    job_man.pop_job_name();

    return {};
  }

  auto& am = App::mod<AssetManager>();
  auto model_uuid = am.import_asset(asset_path);
  if (!model_uuid) {
    auto lock = std::unique_lock(self.thumbnail_mutex);
    self.active_jobs.erase(asset_hash);
    return {};
  }

  std::lock_guard lock(self.queue_mutex);
  self.pending_mesh_renders.push({.asset_hash = asset_hash, .model_uuid = model_uuid, .expected_png = expected_png});

  return {};
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
  model_entity.add<AssetOwner>();

  auto mark_mesh_children_owned = [](this auto&& self_fn, flecs::entity entity) -> void {
    entity.children([&self_fn](flecs::entity child) {
      if (child.has<MeshComponent>()) {
        child.add<AssetOwner>();
      }
      self_fn(child);
    });
  };
  mark_mesh_children_owned(model_entity);

  auto& asset_man = App::mod<AssetManager>();
  auto model_asset = asset_man.get_model(model_uuid);
  if (!model_asset) {
    return nullopt;
  }

  const auto sun = thumbnail_scene.create_entity("sun", true);
  sun
    .set<TransformComponent>({
      .rotation = glm::quat(glm::vec3(glm::radians(45.f), glm::radians(-145.f), 0.f)),
    })
    .set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 20.f})
    .add<AtmosphereComponent>()
    .set<SkyComponent>(SkyComponent{.solid_color = glm::vec4(0.f, 0.f, 0.f, 1.0f), .texture = UUID(nullptr)});

  f32 cam_fov = 40.f;
  auto transform = ThumbnailCamera::calculate_from_model(*model_asset.value, cam_fov, 1.0f);

  const auto camera = thumbnail_scene.create_entity("camera", true);
  camera.set<CameraComponent>({CameraComponent{
    .fov = cam_fov,
    .far_clip = transform.far_clip,
    .near_clip = transform.near_clip,
    .yaw = transform.yaw,
    .pitch = transform.pitch,
    .position = transform.position,
  }});
  camera.set<TransformComponent>(TransformComponent{
    .position = transform.position,
    .rotation = transform.rotation,
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
  {
    std::scoped_lock lock(render_context.queue_mutex);
    readback_buffer.wait(*render_context.frame_allocator, temp_compiler);
  }

  std::vector<u8> pixel_data(buffer_size);
  std::memcpy(pixel_data.data(), readback_buffer->mapped_ptr, buffer_size);

  return pixel_data;
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
