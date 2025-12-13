#include "Asset/AssetMetadata.hpp"

#include <simdjson.h>

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetMetadata::from_file(std::filesystem::path& path) -> option<AssetMetadata> {
  auto file = File(path, FileAccess::Read);
  if (!file) {
    return nullopt;
  }

  auto buffer_size = file.size + simdjson::SIMDJSON_PADDING;
  auto buffer = std::vector<c8>(buffer_size);
  file.read(buffer.data(), file.size);

  return from_string(std::string_view(buffer.data(), file.size), buffer_size);
}

auto AssetMetadata::from_string(std::string_view str, usize padded_capacity) -> option<AssetMetadata> {
  auto print_error = [](auto v) {
    OX_LOG_ERROR("Failed to parse meta file! {}", simdjson::error_message(v));
  };

  auto contents = simdjson::padded_string_view(str, padded_capacity);
  OX_ASSERT(contents.has_sufficient_padding());
  auto parser = simdjson::ondemand::parser{};
  auto doc = parser.iterate(contents);
  if (doc.error()) {
    print_error(doc.error());
    return nullopt;
  }

  auto uuid_json = doc["uuid"].get_string();
  if (uuid_json.error()) {
    print_error(uuid_json.error());
    return nullopt;
  }

  auto uuid = UUID::from_string(uuid_json.value_unsafe());
  if (!uuid) {
    OX_LOG_ERROR("Failed to read asset meta file. `uuid` is corrupt.");
    return nullopt;
  }

  auto type_json = doc["type"].get_uint64();
  if (type_json.error()) {
    print_error(type_json.error());
    return nullopt;
  }

  auto type = static_cast<AssetType>(type_json.value_unsafe());

  auto variant = AssetVariant{};
  switch (type) {
    case AssetType::Model: {
      auto embedded_textures_json = doc["embedded_textures"].get_array();
      if (embedded_textures_json.error()) {
        print_error(embedded_textures_json.error());
        return nullopt;
      }

      auto embedded_textures = std::vector<UUID>();
      for (auto embedded_texture_json : embedded_textures_json) {
        auto embedded_texture_uuid_str = embedded_texture_json.get_string();
        if (embedded_texture_uuid_str.error()) {
          print_error(embedded_texture_uuid_str.error());
          return nullopt;
        }

        auto embedded_texture_uuid = UUID::from_string(embedded_texture_json);
        if (!embedded_texture_uuid.has_value()) {
          OX_LOG_ERROR("Failed to read asset meta file. An embedded texture with corrupt UUID.");
          return nullopt;
        }

        embedded_textures.push_back(embedded_texture_uuid.value());
      }

      auto embedded_materials_json = doc["embedded_materials"].get_array();
      if (embedded_materials_json.error()) {
        print_error(embedded_materials_json.error());
        return nullopt;
      }

      auto embedded_materials = std::vector<UUID>();
      for (auto embedded_material_json : embedded_materials_json) {
        auto embedded_material_uuid_str = embedded_material_json.get_string();
        if (embedded_material_uuid_str.error()) {
          print_error(embedded_material_uuid_str.error());
          return nullopt;
        }

        auto embedded_material_uuid = UUID::from_string(embedded_material_json);
        if (!embedded_material_uuid.has_value()) {
          OX_LOG_ERROR("Failed to read asset meta file. An embedded texture with corrupt UUID.");
          return nullopt;
        }

        embedded_materials.push_back(embedded_material_uuid.value());
      }

      variant = ModelMetadata{
        .embedded_texture_uuids = std::move(embedded_textures),
        .material_uuids = std::move(embedded_materials)
      };
    } break;
    case AssetType::Material: {
      auto sampling_mode_json = doc["sampling_mode"].get_uint64();
      if (sampling_mode_json.error()) {
        print_error(sampling_mode_json.error());
        return nullopt;
      }

      auto albedo_color_json = doc["albedo_color"].get_array();
      if (albedo_color_json.error()) {
        print_error(albedo_color_json.error());
        return nullopt;
      }

      auto albedo_color = glm::vec4{};
      for (usize i = 0; i < 4 && i < albedo_color_json.count_elements(); ++i) {
        auto color_value = albedo_color_json.at(i).get_double();
        if (color_value.error()) {
          print_error(color_value.error());
          return nullopt;
        }
        albedo_color[i] = static_cast<f32>(color_value.value());
      }

      auto emissive_color_json = doc["emissive_color"].get_array();
      if (emissive_color_json.error()) {
        print_error(emissive_color_json.error());
        return nullopt;
      }

      auto emissive_color = glm::vec3{};
      for (usize i = 0; i < 3 && i < emissive_color_json.count_elements(); ++i) {
        auto color_value = emissive_color_json.at(i).get_double();
        if (color_value.error()) {
          print_error(color_value.error());
          return nullopt;
        }
        emissive_color[i] = static_cast<f32>(color_value.value());
      }

      auto roughness_factor_json = doc["roughness_factor"].get_double();
      if (roughness_factor_json.error()) {
        print_error(roughness_factor_json.error());
        return nullopt;
      }

      auto metallic_factor_json = doc["metallic_factor"].get_double();
      if (metallic_factor_json.error()) {
        print_error(metallic_factor_json.error());
        return nullopt;
      }

      auto alpha_mode_json = doc["alpha_mode"].get_uint64();
      if (alpha_mode_json.error()) {
        print_error(alpha_mode_json.error());
        return nullopt;
      }

      auto alpha_cutoff_json = doc["alpha_cutoff"].get_double();
      if (alpha_cutoff_json.error()) {
        print_error(alpha_cutoff_json.error());
        return nullopt;
      }

      auto albedo_texture_uuid_str = doc["albedo_texture"].get_string();
      if (albedo_texture_uuid_str.error()) {
        print_error(albedo_texture_uuid_str.error());
        return nullopt;
      }
      auto albedo_texture = UUID::from_string(doc["albedo_texture"]);
      if (!albedo_texture.has_value()) {
        OX_LOG_ERROR("Failed to read asset meta file. Albedo texture has corrupt UUID.");
        return nullopt;
      }

      auto normal_texture_uuid_str = doc["normal_texture"].get_string();
      if (normal_texture_uuid_str.error()) {
        print_error(normal_texture_uuid_str.error());
        return nullopt;
      }
      auto normal_texture = UUID::from_string(doc["normal_texture"]);
      if (!normal_texture.has_value()) {
        OX_LOG_ERROR("Failed to read asset meta file. Normal texture has corrupt UUID.");
        return nullopt;
      }

      auto emissive_texture_uuid_str = doc["emissive_texture"].get_string();
      if (emissive_texture_uuid_str.error()) {
        print_error(emissive_texture_uuid_str.error());
        return nullopt;
      }
      auto emissive_texture = UUID::from_string(doc["emissive_texture"]);
      if (!emissive_texture.has_value()) {
        OX_LOG_ERROR("Failed to read asset meta file. Emissive texture has corrupt UUID.");
        return nullopt;
      }

      auto metallic_roughness_texture_uuid_str = doc["metallic_roughness_texture"].get_string();
      if (metallic_roughness_texture_uuid_str.error()) {
        print_error(metallic_roughness_texture_uuid_str.error());
        return nullopt;
      }
      auto metallic_roughness_texture = UUID::from_string(doc["metallic_roughness_texture"]);
      if (!metallic_roughness_texture.has_value()) {
        OX_LOG_ERROR("Failed to read asset meta file. Metallic roughness texture has corrupt UUID.");
        return nullopt;
      }

      auto occlusion_texture_uuid_str = doc["occlusion_texture"].get_string();
      if (occlusion_texture_uuid_str.error()) {
        print_error(occlusion_texture_uuid_str.error());
        return nullopt;
      }
      auto occlusion_texture = UUID::from_string(doc["occlusion_texture"]);
      if (!occlusion_texture.has_value()) {
        OX_LOG_ERROR("Failed to read asset meta file. Occlusion texture has corrupt UUID.");
        return nullopt;
      }

      variant = MaterialMetadata{
        .sampling_mode = static_cast<u32>(sampling_mode_json.value()),
        .albedo_color = albedo_color,
        .emissive_color = emissive_color,
        .roughness_factor = static_cast<f32>(roughness_factor_json.value()),
        .metallic_factor = static_cast<f32>(metallic_factor_json.value()),
        .alpha_mode = static_cast<u32>(alpha_mode_json.value()),
        .alpha_cutoff = static_cast<f32>(alpha_cutoff_json.value()),
        .albedo_texture = albedo_texture.value(),
        .normal_texture = normal_texture.value(),
        .emissive_texture = emissive_texture.value(),
        .metallic_roughness_texture = metallic_roughness_texture.value(),
        .occlusion_texture = occlusion_texture.value()
      };
    } break;
    case AssetType::Texture:
    case AssetType::Font:
    case AssetType::Scene:
    case AssetType::Audio:
    case AssetType::Script:
    case AssetType::Shader:
    case AssetType::None   :;
  }

  return AssetMetadata{
    .uuid = uuid.value(),
    .type = type,
    .variant = variant,
  };
}
} // namespace ox
