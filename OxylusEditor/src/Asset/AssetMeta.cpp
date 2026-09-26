#include "Asset/AssetMeta.hpp"

#include "Asset/AssetManager.hpp"
#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto write_texture_asset_meta(JsonWriter&, Texture*) -> bool { return true; }

auto write_script_asset_meta(JsonWriter&, LuaScript*) -> bool { return true; }

auto write_terrain_asset_meta(JsonWriter&, const TerrainEdits*) -> bool { return true; }

auto write_particle_system_asset_meta(JsonWriter&, const ParticleSystem*) -> bool { return true; }

auto write_scene_asset_meta(JsonWriter& writer, const Scene* scene) -> bool {
  ZoneScoped;

  writer["name"] = scene->scene_name;

  return true;
}

auto read_meta_file(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile> {
  ZoneScoped;

  auto content = File::to_string(path);
  if (content.empty()) {
    OX_LOG_ERROR("Failed to read/open file {}!", path);
    return nullptr;
  }

  auto meta_file = std::make_unique<AssetMetaFile>();
  meta_file->contents = simdjson::padded_string(content);
  meta_file->doc = meta_file->parser.iterate(meta_file->contents);

  if (meta_file->doc.error()) {
    OX_LOG_ERROR("Failed to parse meta file! {}", simdjson::error_message(meta_file->doc.error()));
    return nullptr;
  }

  return meta_file;
}

auto read_meta_file_from_asset(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile> {
  ZoneScoped;

  auto meta_path = meta_file_path(path);
  if (meta_path.empty() || !std::filesystem::exists(meta_path)) {
    return nullptr;
  }

  return read_meta_file(meta_path);
}

auto export_scene(AssetManager& asset_man, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path)
  -> bool {
  ZoneScoped;

  auto scene = asset_man.get_scene(uuid);
  write_scene_asset_meta(writer, scene.value);

  return scene->save_to_file(path);
}

auto export_material(AssetManager& asset_man, const UUID& uuid, JsonWriter& writer) -> bool {
  ZoneScoped;

  auto material = asset_man.get_material(uuid);

  writer.key("material");
  return write_material_asset_meta(writer, uuid, *material.value);
}

auto export_terrain_edits(
  AssetManager& asset_man, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  auto edits = asset_man.get_terrain_edits(uuid);
  if (!edits) {
    return false;
  }

  if (!edits->write(path)) {
    return false;
  }

  return write_terrain_asset_meta(writer, edits.value);
}

auto export_particle_system(
  AssetManager& asset_man, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  auto particle_system = asset_man.get_particle_system(uuid);
  if (!particle_system) {
    return false;
  }

  if (!particle_system->write(path)) {
    return false;
  }

  return write_particle_system_asset_meta(writer, particle_system.value);
}

auto export_asset(AssetManager& asset_man, const UUID& uuid, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto asset = asset_man.get_asset(uuid);
  if (!asset)
    return false;

  const auto meta_path = meta_file_path(path);
  if (std::filesystem::exists(meta_path)) {
    if (auto existing_meta = read_meta_file(meta_path)) {
      auto existing_uuid = existing_meta->doc["uuid"].get_string();
      if (!existing_uuid.error() && existing_uuid.value_unsafe() != uuid.str()) {
        OX_LOG_ERROR("Refusing to overwrite {}, which belongs to another asset.", meta_path);
        return false;
      }
    }
  }

  const auto asset_type = asset->type;
  asset.reset();

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset_type);

  switch (asset_type) {
    case AssetType::Texture:
    case AssetType::Model  : {
      OX_LOG_ERROR("Cannot export unsupported asset type {}.", AssetManager::to_asset_type_sv(asset_type));
      return false;
    }
    case AssetType::Scene: {
      if (!export_scene(asset_man, uuid, writer, path))
        return false;
    } break;
    case AssetType::Material: {
      if (!export_material(asset_man, uuid, writer))
        return false;

      // keep the pending copy in step with what just went to disk, so a later lazy load of this
      // material does not resurrect the pre-edit values
      auto material = asset_man.get_material(uuid);
      if (material) {
        asset_man.set_pending_load_info(uuid, *material.value);
      }
    } break;
    case AssetType::Script: {
      if (!write_script_asset_meta(writer, nullptr))
        return false;
    } break;
    case AssetType::Terrain: {
      if (!export_terrain_edits(asset_man, uuid, writer, path))
        return false;
    } break;
    case AssetType::ParticleSystem: {
      if (!export_particle_system(asset_man, uuid, writer, path))
        return false;
    } break;
    default: return false;
  }

  return end_asset_meta(writer, path);
}
} // namespace ox
