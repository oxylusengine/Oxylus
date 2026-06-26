#include "ThumbnailManager.hpp"

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"

namespace ox {
auto ThumbnailManager::init(this ThumbnailManager& self) -> void {
  ZoneScoped;

  // TODO: Check for the thumbnail cache directory and load all the cached thumbnails.
}

auto ThumbnailManager::get_thumbnail(this ThumbnailManager& self, std::filesystem::path asset_path)
  -> std::shared_ptr<Texture> {
  ZoneScoped;

  // TODO: Check the loaded cache map in init() if not found or is outdated
  //       load the asset into a temporary isolated scene and render it offscreen then save it to a png file.
  // A directory such as: .oxassets/thumbnails/
  // Generate a unique hash name based on the asset path string + its filesystem edit time (last_write_time).
  // Could just use stb_image_write or a more compressed format like ktx/dds.

  return nullptr;
}

auto ThumbnailManager::render_thumbnail(this ThumbnailManager& self, const std::filesystem::path& model_path, u32 size)
  -> std::vector<u8> {
  ZoneScoped;

  // TODO: Render the asset using a basic forward pipeline into a fixed size image (256x256)?
  // read the image and return it using a gpu to cpu buffer.

  return {};
}

auto ThumbnailManager::calculate_thumbnail_camera(
  this ThumbnailManager& self, const ox::AABB& model_aabb, glm::mat4& out_view, glm::mat4& out_proj
) -> void {
  // TODO: Position the camera using model's global AABB so its visible no matter what it's size is.
}
} // namespace ox
