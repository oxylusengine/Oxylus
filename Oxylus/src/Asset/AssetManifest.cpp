#include "Asset/AssetManifest.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <zpp_bits.h>

#include "Asset/Material.hpp"
#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
// the manifest is untrusted input: a corrupt length prefix must not turn into a huge allocation
constexpr static auto MAX_MANIFEST_ELEMENTS = 1_u64 << 24;

template <usize N>
static auto to_array(const f32* values) -> std::array<f32, N> {
  auto array = std::array<f32, N>{};
  std::copy_n(values, N, array.begin());
  return array;
}

auto AssetManifest::MaterialEntry::pack(const UUID& uuid, const Material& material) -> MaterialEntry {
  return MaterialEntry{
    .uuid = PackedUUID::pack(uuid),
    .albedo_color = to_array<4>(glm::value_ptr(material.albedo_color)),
    .uv_size = to_array<2>(glm::value_ptr(material.uv_size)),
    .uv_offset = to_array<2>(glm::value_ptr(material.uv_offset)),
    .emissive_color = to_array<3>(glm::value_ptr(material.emissive_color)),
    .roughness_factor = material.roughness_factor,
    .metallic_factor = material.metallic_factor,
    .normal_scale = material.normal_scale,
    .occlusion_strength = material.occlusion_strength,
    .alpha_mode = material.alpha_mode,
    .alpha_cutoff = material.alpha_cutoff,
    .sampling_mode = material.sampling_mode,
    .flip_normal_y = material.flip_normal_y,
    .albedo_texture = PackedUUID::pack(material.albedo_texture),
    .normal_texture = PackedUUID::pack(material.normal_texture),
    .emissive_texture = PackedUUID::pack(material.emissive_texture),
    .metallic_roughness_texture = PackedUUID::pack(material.metallic_roughness_texture),
    .occlusion_texture = PackedUUID::pack(material.occlusion_texture),
  };
}

auto AssetManifest::MaterialEntry::unpack(this const MaterialEntry& self) -> Material {
  return Material{
    .albedo_color = glm::make_vec4(self.albedo_color.data()),
    .uv_size = glm::make_vec2(self.uv_size.data()),
    .uv_offset = glm::make_vec2(self.uv_offset.data()),
    .emissive_color = glm::make_vec3(self.emissive_color.data()),
    .roughness_factor = self.roughness_factor,
    .metallic_factor = self.metallic_factor,
    .normal_scale = self.normal_scale,
    .occlusion_strength = self.occlusion_strength,
    .alpha_mode = self.alpha_mode,
    .alpha_cutoff = self.alpha_cutoff,
    .sampling_mode = self.sampling_mode,
    .flip_normal_y = self.flip_normal_y,
    .albedo_texture = self.albedo_texture.unpack(),
    .normal_texture = self.normal_texture.unpack(),
    .emissive_texture = self.emissive_texture.unpack(),
    .metallic_roughness_texture = self.metallic_roughness_texture.unpack(),
    .occlusion_texture = self.occlusion_texture.unpack(),
  };
}

auto AssetManifest::read(const std::filesystem::path& path) -> option<AssetManifest> {
  ZoneScoped;

  auto file = File(path, FileAccess::Read);
  if (!file || file.size == 0) {
    OX_LOG_ERROR("Couldn't open asset manifest '{}'.", path);
    return nullopt;
  }

  auto* mapped_data = file.map();
  auto bytes = std::span(static_cast<u8*>(mapped_data), file.size);
  auto deser = zpp::bits::in(bytes, zpp::bits::alloc_limit<MAX_MANIFEST_ELEMENTS>{});

  auto header = Header{};
  if (zpp::bits::failure(deser(header)) || header.magic != Header::SIGNATURE) {
    OX_LOG_ERROR("'{}' is not an asset manifest.", path);
    return nullopt;
  }

  if (header.version != Header::VERSION) {
    OX_LOG_ERROR(
      "Asset manifest '{}' has version {}, expected {}. Export the assets again.",
      path,
      header.version,
      Header::VERSION
    );
    return nullopt;
  }

  auto manifest = AssetManifest{};
  if (zpp::bits::failure(deser(manifest.assets, manifest.materials))) {
    OX_LOG_ERROR("Failed to read asset manifest '{}'.", path);
    return nullopt;
  }

  return manifest;
}

auto AssetManifest::write(this const AssetManifest& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto [data, ser] = zpp::bits::data_out();
  if (zpp::bits::failure(ser(Header{}, self.assets, self.materials))) {
    OX_LOG_ERROR("Failed to serialize asset manifest.");
    return false;
  }

  auto file = File(path, FileAccess::Write);
  if (!file) {
    OX_LOG_ERROR("Failed to open '{}' for writing.", path);
    return false;
  }

  file.write(data);

  return true;
}
} // namespace ox
