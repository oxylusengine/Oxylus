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
