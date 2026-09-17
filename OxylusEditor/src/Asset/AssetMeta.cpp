#include "Asset/AssetMeta.hpp"

#include <array>
#include <cstring>
#include <span>

#include "Asset/AssetManager.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void {
  ZoneScoped;

  writer.begin_obj();
  writer["uuid"] = uuid.str();
  writer["type"] = std::to_underlying(type);
}

auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  writer.end_obj();

  auto file = File(meta_file_path(path), FileAccess::Write);
  file.write(writer.stream.view());
  file.close();
  return true;
}

auto write_texture_asset_meta(JsonWriter&, Texture*) -> bool { return true; }

auto write_script_asset_meta(JsonWriter&, LuaScript*) -> bool { return true; }

auto write_terrain_asset_meta(JsonWriter&, const TerrainEdits*) -> bool { return true; }

auto write_particle_system_asset_meta(JsonWriter&, const ParticleSystem*) -> bool { return true; }

auto write_scene_asset_meta(JsonWriter& writer, const Scene* scene) -> bool {
  ZoneScoped;

  writer["name"] = scene->scene_name;

  return true;
}

auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool {
  ZoneScoped;

  writer.begin_obj();

  writer["uuid"] = uuid.str();
  writer["sampling_mode"] = static_cast<u32>(material.sampling_mode);
  writer["albedo_color"] = material.albedo_color;
  writer["uv_size"] = material.uv_size;
  writer["uv_offset"] = material.uv_offset;
  writer["emissive_color"] = material.emissive_color;
  writer["roughness_factor"] = material.roughness_factor;
  writer["metallic_factor"] = material.metallic_factor;
  writer["normal_scale"] = material.normal_scale;
  writer["occlusion_strength"] = material.occlusion_strength;
  writer["alpha_mode"] = std::to_underlying(material.alpha_mode);
  writer["alpha_cutoff"] = material.alpha_cutoff;
  writer["flip_normal_y"] = material.flip_normal_y;
  writer["albedo_texture"] = material.albedo_texture.str().c_str();
  writer["normal_texture"] = material.normal_texture.str().c_str();
  writer["emissive_texture"] = material.emissive_texture.str().c_str();
  writer["metallic_roughness_texture"] = material.metallic_roughness_texture.str().c_str();
  writer["occlusion_texture"] = material.occlusion_texture.str().c_str();

  writer.end_obj();

  return true;
}

auto read_material_asset_meta(simdjson::ondemand::value json, Material& material) -> bool {
  ZoneScoped;

  const auto read_f32 = [&json](std::string_view key, f32& value) {
    auto result = json[key].get_double();
    if (!result.error()) {
      value = static_cast<f32>(result.value_unsafe());
    }
  };

  const auto read_enum = [&json]<typename T>(std::string_view key, T& value) {
    auto result = json[key].get_uint64();
    if (!result.error()) {
      value = static_cast<T>(result.value_unsafe());
    }
  };

  const auto read_bool = [&json](std::string_view key, bool& value) {
    auto result = json[key].get_bool();
    if (!result.error()) {
      value = result.value_unsafe();
    }
  };

  const auto read_uuid = [&json](std::string_view key, UUID& value) {
    auto result = json[key].get_string();
    if (result.error()) {
      return;
    }

    if (auto uuid = UUID::from_string(result.value_unsafe()); uuid.has_value()) {
      value = uuid.value();
    }
  };

  const auto read_vec = [&json]<glm::length_t N>(std::string_view key, glm::vec<N, f32>& value) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    auto field = json[key];
    if (field.error()) {
      return;
    }

    for (glm::length_t i = 0; i < N; i++) {
      auto result = field[components[i]].get_double();
      if (!result.error()) {
        value[i] = static_cast<f32>(result.value_unsafe());
      }
    }
  };

  read_enum("sampling_mode", material.sampling_mode);
  read_enum("alpha_mode", material.alpha_mode);
  read_vec("albedo_color", material.albedo_color);
  read_vec("uv_size", material.uv_size);
  read_vec("uv_offset", material.uv_offset);
  read_vec("emissive_color", material.emissive_color);
  read_f32("roughness_factor", material.roughness_factor);
  read_f32("metallic_factor", material.metallic_factor);
  read_f32("normal_scale", material.normal_scale);
  read_f32("occlusion_strength", material.occlusion_strength);
  read_f32("alpha_cutoff", material.alpha_cutoff);
  read_bool("flip_normal_y", material.flip_normal_y);
  read_uuid("albedo_texture", material.albedo_texture);
  read_uuid("normal_texture", material.normal_texture);
  read_uuid("emissive_texture", material.emissive_texture);
  read_uuid("metallic_roughness_texture", material.metallic_roughness_texture);
  read_uuid("occlusion_texture", material.occlusion_texture);

  return true;
}

static auto header_matches(std::span<const u8> header, std::span<const u8> magic, const usize offset = 0) -> bool {
  return header.size() >= offset + magic.size() && std::memcmp(header.data() + offset, magic.data(), magic.size()) == 0;
}

// What the file says it is, which a rename cannot change. Only formats that carry a signature are
// here: glTF, Lua, JSON and the sidecars are text, and the two `ox` formats are ours to name.
static auto to_asset_file_signature(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;

  auto error = std::error_code{};
  if (!std::filesystem::is_regular_file(path, error)) {
    return AssetFileType::None;
  }

  auto file = File(path, FileAccess::Read);
  if (!file) {
    return AssetFileType::None;
  }

  // as much as the longest signature checked below, KTX2's twelve bytes, plus the tag WAV carries
  // past its RIFF header
  auto bytes = std::array<u8, 16>{};
  const auto read = file.read(bytes.data(), bytes.size());
  const auto header = std::span(bytes).first(ox::min(static_cast<usize>(read), bytes.size()));

  constexpr u8 PNG[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  constexpr u8 JPEG[] = {0xFF, 0xD8, 0xFF};
  constexpr u8 KTX2[] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  constexpr u8 DDS[] = {'D', 'D', 'S', ' '};
  constexpr u8 GLB[] = {'g', 'l', 'T', 'F'};
  constexpr u8 OGG[] = {'O', 'g', 'g', 'S'};
  constexpr u8 FLAC[] = {'f', 'L', 'a', 'C'};
  constexpr u8 RIFF[] = {'R', 'I', 'F', 'F'};
  constexpr u8 WAVE[] = {'W', 'A', 'V', 'E'};
  constexpr u8 ID3[] = {'I', 'D', '3'};

  if (header_matches(header, PNG)) {
    return AssetFileType::PNG;
  }
  if (header_matches(header, JPEG)) {
    return AssetFileType::JPEG;
  }
  if (header_matches(header, KTX2)) {
    return AssetFileType::KTX2;
  }
  if (header_matches(header, DDS)) {
    return AssetFileType::DDS;
  }
  if (header_matches(header, GLB)) {
    return AssetFileType::GLB;
  }
  if (header_matches(header, OGG)) {
    return AssetFileType::OGG;
  }
  if (header_matches(header, FLAC)) {
    return AssetFileType::FLAC;
  }
  // RIFF is a container, so the form it holds is what decides
  if (header_matches(header, RIFF) && header_matches(header, WAVE, 8)) {
    return AssetFileType::WAV;
  }
  // MP3 is the one format with no real magic: an ID3 tag if it has one, otherwise the eleven set
  // bits an mp3 frame starts with. Checked last because that sync pattern is weak evidence next to
  // everything above.
  if (header_matches(header, ID3) || (header.size() >= 2 && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0)) {
    return AssetFileType::MP3;
  }

  return AssetFileType::None;
}

auto to_asset_file_type(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;
  memory::ScopedStack stack;

  // The signature is the stronger evidence, so it settles the question whenever the file has one:
  // a jpeg saved as `.png` reaches a decoder that cannot read it otherwise. The extension answers
  // for text formats, and for a path that is not on disk yet -- a sidecar named for a file that is
  // about to be written.
  if (const auto signature = to_asset_file_signature(path); signature != AssetFileType::None) {
    return signature;
  }

  if (!path.has_extension()) {
    return AssetFileType::None;
  }

  auto extension = stack.to_upper(path.extension().string());
  switch (fnv64_str(extension)) {
    case fnv64_c(".GLB")       : return AssetFileType::GLB;
    case fnv64_c(".GLTF")      : return AssetFileType::GLTF;
    case fnv64_c(".PNG")       : return AssetFileType::PNG;
    case fnv64_c(".JPG")       :
    case fnv64_c(".JPEG")      : return AssetFileType::JPEG;
    case fnv64_c(".DDS")       : return AssetFileType::DDS;
    case fnv64_c(".JSON")      : return AssetFileType::JSON;
    case fnv64_c(".OXASSET")   : return AssetFileType::Meta;
    case fnv64_c(".KTX2")      : return AssetFileType::KTX2;
    case fnv64_c(".LUA")       : return AssetFileType::LUA;
    case fnv64_c(".OXTERRAIN") : return AssetFileType::OXTERRAIN;
    case fnv64_c(".OXPARTICLE"): return AssetFileType::OXPARTICLE;
    case fnv64_c(".WAV")       : return AssetFileType::WAV;
    case fnv64_c(".MP3")       : return AssetFileType::MP3;
    case fnv64_c(".FLAC")      : return AssetFileType::FLAC;
    case fnv64_c(".OGG")       : return AssetFileType::OGG;
    default                    : return AssetFileType::None;
  }
}

auto meta_file_path(const std::filesystem::path& path) -> std::filesystem::path {
  ZoneScoped;

  if (path.empty()) {
    return {};
  }

  if (to_asset_file_type(path) == AssetFileType::Meta) {
    return path;
  }

  auto meta_path = path;
  meta_path += ".oxasset";

  return meta_path;
}

auto owns_meta_file(const std::filesystem::path& path) -> bool {
  ZoneScoped;

  if (path.empty()) {
    return false;
  }

  const auto file_type = to_asset_file_type(path);

  return file_type == AssetFileType::None || file_type == AssetFileType::Meta;
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

auto register_asset_from_meta(AssetManager& asset_man, const std::filesystem::path& meta_path) -> UUID {
  ZoneScoped;

  auto meta_json = read_meta_file(meta_path);
  if (!meta_json) {
    return UUID(nullptr);
  }

  // simdjson ondemand is single pass, so these have to be read in the order `begin_asset_meta` wrote
  // them: uuid, then type, then the per-type payload.
  auto uuid_json = meta_json->doc["uuid"].get_string();
  if (uuid_json.error()) {
    OX_LOG_ERROR("Failed to read asset meta file. `uuid` is missing.");
    return UUID(nullptr);
  }

  auto type_json = meta_json->doc["type"].get_number();
  if (type_json.error()) {
    OX_LOG_ERROR("Failed to read asset meta file. `type` is missing.");
    return UUID(nullptr);
  }

  auto asset_path = meta_path;
  asset_path.replace_extension("");
  auto uuid = UUID::from_string(uuid_json.value_unsafe()).value();
  auto type = static_cast<AssetType>(type_json.value_unsafe().get_uint64());

  if (!asset_man.register_asset(uuid, type, asset_path)) {
    return UUID(nullptr);
  }

  // A material has no payload file: the sidecar is the whole asset. The engine cannot discover that
  // on its own, so hand it over now and let the load pick it up whenever it happens.
  if (type == AssetType::Material) {
    auto material = Material{};
    if (auto material_json = meta_json->doc["material"]; !material_json.error()) {
      read_material_asset_meta(material_json.value_unsafe(), material);
    }

    asset_man.set_pending_load_info(uuid, material);
  }

  return uuid;
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
