#include "ThumbnailManager.hpp"

#include <stb_image_write.h>

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
  job_man.push_job_name("ContentPanelThumbnail");
  job_man.submit(Job::create([&self, file_path = asset_path, asset_hash]() {
    auto thumbnail_texture = std::make_shared<Texture>();
    auto file_extension = file_path.extension();
    TextureLoadInfo::MimeType mime_type = TextureLoadInfo::MimeType::Generic;
    if (file_extension == "ktx" || file_extension == "ktx2") {
      mime_type = TextureLoadInfo::MimeType::KTX;
    }
    thumbnail_texture->create(
      file_path,
      {.preset = Preset::eRTT2DUnmipped, .mime = mime_type, .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1}}
    );
    auto write_lock = std::unique_lock(self.thumbnail_mutex);
    self.thumbnail_cache.insert_or_assign(asset_hash, thumbnail_texture);
    self.active_jobs.erase(asset_hash);
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

  if (self.thumbnail_cache.contains(asset_hash)) {
    return self.thumbnail_cache[asset_hash];
  }

  auto expected_png = self.cache_dir / (asset_hash + ".png");
  if (std::filesystem::exists(expected_png)) {
    auto cached_tex = std::make_shared<Texture>();
    cached_tex->create(expected_png, {});
    self.thumbnail_cache.insert_or_assign(asset_hash, cached_tex);
    return cached_tex;
  }

  // TODO: Do the rest of this function in a async thread
  // auto& job_man = App::get_job_manager();
  // job_man.submit(
  //   Job::create([]() {
  //   })
  // );

  auto& am = App::mod<AssetManager>();
  auto asset = am.import_asset(asset_path);
  if (!asset) {
    return nullopt;
  }

  auto model = am.get_model(asset);
  if (!model) {
    return nullopt;
  }

  auto pixels = self.render_thumbnail(model.value, THUMBNAIL_SIZE);

  if (!pixels.empty()) {
    stbi_write_png(expected_png.string().c_str(), THUMBNAIL_SIZE, THUMBNAIL_SIZE, 4, pixels.data(), THUMBNAIL_SIZE * 4);

    // Create texture resource to push to cache map
    auto new_thumb = std::make_shared<Texture>();
    new_thumb->create(
      {},
      TextureLoadInfo{.loaded_data = pixels.data(), .extent = vuk::Extent3D{THUMBNAIL_SIZE, THUMBNAIL_SIZE, 1}}
    );
    self.thumbnail_cache.insert_or_assign(asset_hash, new_thumb);
    return new_thumb;
  }

  return nullopt;
}

auto ThumbnailManager::render_thumbnail(this ThumbnailManager& self, Model* model, u32 size) -> std::vector<u8> {
  ZoneScoped;

  // Set up vuk offscreen render targets, call calculate_thumbnail_camera with model->get_base_aabb(),
  // execute your render graph commands sequentially, and read back the transient mapping buffer.

  return {};
}

auto ThumbnailManager::get_asset_hash(this const ThumbnailManager& self, const std::filesystem::path& path)
  -> std::string {
  auto last_write = std::filesystem::last_write_time(path).time_since_epoch().count();
  std::string signature = path.string() + std::to_string(last_write);
  size_t hash_val = std::hash<std::string>{}(signature);
  return fmt::format("{:X}", hash_val);
}

auto ThumbnailManager::calculate_thumbnail_camera(
  this ThumbnailManager& self, const ox::AABB& model_aabb, glm::mat4& out_view, glm::mat4& out_proj
) -> void {
  glm::vec3 center = model_aabb.get_center();
  glm::vec3 size = model_aabb.get_size();

  f32 max_dim = glm::max(size.x, glm::max(size.y, size.z));
  f32 fov = glm::radians(45.0f);
  f32 distance = (max_dim * 0.5f) / glm::sin(fov * 0.5f);

  glm::vec3 eye = center + glm::vec3(0.5f, 0.5f, 1.0f) * distance;

  out_view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
  out_proj = glm::perspective(fov, 1.0f, 0.1f, distance * 5.0f);
  out_proj[1][1] *= -1;
}
} // namespace ox
