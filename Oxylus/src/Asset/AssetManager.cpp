#include "Asset/AssetManager.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <queue>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/ParserGLTF.hpp"
#include "Core/App.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/JsonHelpers.hpp"
#include "Utils/Log.hpp"
#include "Utils/Profiler.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};

namespace ox {
auto get_default_gltf_extensions() -> fastgltf::Extensions {
  auto extensions = fastgltf::Extensions::None;
  extensions |= fastgltf::Extensions::KHR_mesh_quantization;
  extensions |= fastgltf::Extensions::KHR_texture_transform;
  extensions |= fastgltf::Extensions::KHR_texture_basisu;
  extensions |= fastgltf::Extensions::KHR_lights_punctual;
  extensions |= fastgltf::Extensions::KHR_materials_specular;
  extensions |= fastgltf::Extensions::KHR_materials_ior;
  extensions |= fastgltf::Extensions::KHR_materials_iridescence;
  extensions |= fastgltf::Extensions::KHR_materials_volume;
  extensions |= fastgltf::Extensions::KHR_materials_transmission;
  extensions |= fastgltf::Extensions::KHR_materials_clearcoat;
  extensions |= fastgltf::Extensions::KHR_materials_emissive_strength;
  extensions |= fastgltf::Extensions::KHR_materials_sheen;
  extensions |= fastgltf::Extensions::KHR_materials_unlit;
  extensions |= fastgltf::Extensions::KHR_materials_anisotropy;
  extensions |= fastgltf::Extensions::EXT_meshopt_compression;
  extensions |= fastgltf::Extensions::EXT_texture_webp;
  extensions |= fastgltf::Extensions::MSFT_texture_dds;

  return extensions;
}

auto get_default_gltf_options() -> fastgltf::Options {
  auto options = fastgltf::Options::None;
  options |= fastgltf::Options::LoadExternalBuffers;
  // options |= fastgltf::Options::DontRequireValidAssetMember;

  return options;
}

auto gltf_mime_type_to_asset_file_type(fastgltf::MimeType mime) -> AssetFileType {
  switch (mime) {
    case fastgltf::MimeType::JPEG: return AssetFileType::JPEG;
    case fastgltf::MimeType::PNG : return AssetFileType::PNG;
    case fastgltf::MimeType::KTX2: return AssetFileType::KTX2;
    case fastgltf::MimeType::DDS : return AssetFileType::DDS;
    default                      : return AssetFileType::None;
  }
}

auto gltf_mime_type_to_texture_mime_type(fastgltf::MimeType mime) -> TextureLoadInfo::MimeType {
  switch (mime) {
    case fastgltf::MimeType::JPEG:
    case fastgltf::MimeType::PNG : return TextureLoadInfo::MimeType::Generic;
    case fastgltf::MimeType::KTX2: return TextureLoadInfo::MimeType::KTX;
    case fastgltf::MimeType::DDS : return TextureLoadInfo::MimeType::DDS;
    default                      : return TextureLoadInfo::MimeType::Generic;
  }
}

auto gltf_sampler_to_sampler(const fastgltf::Sampler& gltf_sampler) -> vuk::SamplerCreateInfo {
  auto get_address_mode = [](fastgltf::Wrap v) -> vuk::SamplerAddressMode {
    switch (v) {
      case fastgltf::Wrap::ClampToEdge   : return vuk::SamplerAddressMode::eClampToEdge;
      case fastgltf::Wrap::MirroredRepeat: return vuk::SamplerAddressMode::eMirroredRepeat;
      case fastgltf::Wrap::Repeat        : return vuk::SamplerAddressMode::eRepeat;
    }
  };

  auto get_filter_mode = [](fastgltf::Filter v) -> vuk::Filter {
    switch (v) {
      case fastgltf::Filter::Nearest:
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::Filter::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::Filter::eLinear;
    }
  };

  auto get_mip_filter_mode = [](fastgltf::Filter v) -> vuk::SamplerMipmapMode {
    switch (v) {
      case fastgltf::Filter::Nearest:
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::SamplerMipmapMode::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::SamplerMipmapMode::eLinear;
    }
  };

  return vuk::SamplerCreateInfo{
    .magFilter = get_filter_mode(gltf_sampler.magFilter.value_or(fastgltf::Filter::Linear)),
    .minFilter = get_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .mipmapMode = get_mip_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .addressModeU = get_address_mode(gltf_sampler.wrapS),
    .addressModeV = get_address_mode(gltf_sampler.wrapT),
  };
}

auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void {
  ZoneScoped;

  writer.begin_obj();

  writer["uuid"] = uuid.str();

  writer["type"] = std::to_underlying(type);
}

auto write_texture_asset_meta(JsonWriter& writer, Texture*) -> bool {
  ZoneScoped;

  return true;
}

auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool {
  ZoneScoped;

  writer.begin_obj();

  writer["uuid"] = uuid.str();
  writer["sampling_mode"] = static_cast<u32>(material.sampling_mode);
  writer["albedo_color"] = material.albedo_color;
  writer["emissive_color"] = material.emissive_color;
  writer["roughness_factor"] = material.roughness_factor;
  writer["metallic_factor"] = material.metallic_factor;
  writer["alpha_mode"] = std::to_underlying(material.alpha_mode);
  writer["alpha_cutoff"] = material.alpha_cutoff;
  writer["albedo_texture"] = material.albedo_texture.str().c_str();
  writer["normal_texture"] = material.normal_texture.str().c_str();
  writer["emissive_texture"] = material.emissive_texture.str().c_str();
  writer["metallic_roughness_texture"] = material.metallic_roughness_texture.str().c_str();
  writer["occlusion_texture"] = material.occlusion_texture.str().c_str();

  writer.end_obj();

  return true;
}

auto read_material_data(Material* mat, simdjson::ondemand::value& material_obj) -> bool {
  ZoneScoped;

  auto sampling_mode = material_obj["sampling_mode"];
  if (sampling_mode.error()) {
    OX_LOG_WARN("Couldn't read sampling_mode field from material!");
  } else {
    mat->sampling_mode = static_cast<SamplingMode>(sampling_mode->get_uint64().value_unsafe());
  }

  auto albedo_color = material_obj["albedo_color"];
  if (albedo_color.error()) {
    OX_LOG_WARN("Couldn't read albedo_color field from material!");
  } else {
    json_to_vec(albedo_color.value_unsafe(), mat->albedo_color);
  }

  auto emissive_color = material_obj["emissive_color"];
  if (emissive_color.error()) {
    OX_LOG_WARN("Couldn't read sampling_mode field from material!");
  } else {
    json_to_vec(emissive_color.value_unsafe(), mat->emissive_color);
  }

  auto roughness_factor = material_obj["roughness_factor"];
  if (roughness_factor.error()) {
    OX_LOG_WARN("Couldn't read roughness_factor field from material!");
  } else {
    mat->roughness_factor = static_cast<f32>(roughness_factor.get_double().value_unsafe());
  }

  auto metallic_factor = material_obj["metallic_factor"];
  if (metallic_factor.error()) {
    OX_LOG_WARN("Couldn't read metallic_factor field from material!");
  } else {
    mat->metallic_factor = static_cast<f32>(metallic_factor.get_double().value_unsafe());
  }

  auto alpha_mode = material_obj["alpha_mode"];
  if (alpha_mode.error()) {
    OX_LOG_WARN("Couldn't read alpha_mode field from material!");
  } else {
    mat->alpha_mode = static_cast<AlphaMode>(alpha_mode.get_uint64().value_unsafe());
  }

  auto alpha_cutoff = material_obj["alpha_cutoff"];
  if (alpha_cutoff.error()) {
    OX_LOG_WARN("Couldn't read alpha_cutoff field from material!");
  } else {
    mat->alpha_cutoff = static_cast<f32>(alpha_cutoff.get_double().value_unsafe());
  }

  auto albedo_texture = material_obj["albedo_texture"];
  if (albedo_texture.error()) {
    OX_LOG_WARN("Couldn't read albedo_texture field from material!");
  } else {
    mat->albedo_texture = UUID::from_string(albedo_texture.get_string().value_unsafe()).value_or(UUID(nullptr));
  }

  auto normal_texture = material_obj["normal_texture"];
  if (normal_texture.error()) {
    OX_LOG_WARN("Couldn't read normal_texture field from material!");
  } else {
    mat->normal_texture = UUID::from_string(normal_texture.get_string().value_unsafe()).value_or(UUID(nullptr));
  }

  auto emissive_texture = material_obj["emissive_texture"];
  if (emissive_texture.error()) {
    OX_LOG_WARN("Couldn't read emissive_texture field from material!");
  } else {
    mat->emissive_texture = UUID::from_string(emissive_texture.get_string().value_unsafe()).value_or(UUID(nullptr));
  }

  auto metallic_roughness_texture = material_obj["metallic_roughness_texture"];
  if (metallic_roughness_texture.error()) {
    OX_LOG_WARN("Couldn't read metallic_roughness_texture field from material!");
  } else {
    mat->metallic_roughness_texture = UUID::from_string(metallic_roughness_texture.get_string().value_unsafe())
                                        .value_or(UUID(nullptr));
  }

  auto occlusion_texture = material_obj["occlusion_texture"];
  if (occlusion_texture.error()) {
    OX_LOG_WARN("Couldn't read occlusion_texture field from material!");
  } else {
    mat->occlusion_texture = UUID::from_string(occlusion_texture.get_string().value_unsafe()).value_or(UUID(nullptr));
  }

  return true;
}

auto write_mesh_asset_meta(
  JsonWriter& writer,
  std::span<UUID> embedded_texture_uuids,
  std::span<UUID> material_uuids,
  std::span<Material> materials
) -> bool {
  ZoneScoped;

  writer["embedded_textures"].begin_array();
  for (const auto& uuid : embedded_texture_uuids) {
    writer << uuid.str();
  }
  writer.end_array();

  writer["embedded_materials"].begin_array();
  for (const auto& [material_uuid, material] : std::views::zip(material_uuids, materials)) {
    write_material_asset_meta(writer, material_uuid, material);
  }
  writer.end_array();

  return true;
}

auto write_scene_asset_meta(JsonWriter& writer, Scene* scene) -> bool {
  ZoneScoped;

  writer["name"] = scene->scene_name;

  return true;
}

auto write_script_asset_meta(JsonWriter&, LuaSystem*) -> bool {
  ZoneScoped;

  return true;
}

auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  writer.end_obj();

  auto meta_path = path;
  if (path.has_extension() && path.extension() != "oxasset") {
    meta_path += ".oxasset";
  }

  auto file = File(meta_path, FileAccess::Write);
  file.write(writer.stream.view());
  file.close();
  return true;
}

auto AssetManager::init() -> std::expected<void, std::string> { return {}; }

auto AssetManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : asset_registry) {
    // leak check
    if (asset.is_loaded() && asset.ref_count != 0) {
      OX_LOG_WARN(
        "A {} asset ({}, {}) with refcount of {} is still alive!",
        to_asset_type_sv(asset.type),
        uuid.str(),
        asset.path,
        asset.ref_count
      );
    }
  }

  asset_registry.clear();
  dirty_materials.clear();
  model_map.reset();
  texture_map.reset();
  material_map.reset();
  scene_map.reset();
  audio_map.reset();
  script_map.reset();

  return {};
}

auto AssetManager::registry() const -> const AssetRegistry& { return asset_registry; }

auto AssetManager::read_meta_file(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile> {
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

auto AssetManager::to_asset_file_type(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!path.has_extension()) {
    return AssetFileType::None;
  }

  auto extension = stack.to_upper(path.extension().string());
  switch (fnv64_str(extension)) {
    case fnv64_c(".GLB")    : return AssetFileType::GLB;
    case fnv64_c(".GLTF")   : return AssetFileType::GLTF;
    case fnv64_c(".PNG")    : return AssetFileType::PNG;
    case fnv64_c(".JPG")    :
    case fnv64_c(".JPEG")   : return AssetFileType::JPEG;
    case fnv64_c(".DDS")    : return AssetFileType::DDS;
    case fnv64_c(".JSON")   : return AssetFileType::JSON;
    case fnv64_c(".OXASSET"): return AssetFileType::Meta;
    case fnv64_c(".KTX2")   : return AssetFileType::KTX2;
    case fnv64_c(".LUA")    : return AssetFileType::LUA;
    default                 : return AssetFileType::None;
  }
}

auto AssetManager::to_asset_type_sv(AssetType type) -> std::string_view {
  ZoneScoped;

  switch (type) {
    case AssetType::None    : return "None";
    case AssetType::Shader  : return "Shader";
    case AssetType::Model   : return "Model";
    case AssetType::Texture : return "Texture";
    case AssetType::Material: return "Material";
    case AssetType::Font    : return "Font";
    case AssetType::Scene   : return "Scene";
    case AssetType::Audio   : return "Audio";
    case AssetType::Script  : return "Script";
    default                 : return {};
  }
}

auto AssetManager::create_asset(const AssetType type, const std::filesystem::path& path) -> UUID {
  const auto uuid = UUID::generate_random();
  auto [asset_it, inserted] = asset_registry.try_emplace(uuid);
  if (!inserted) {
    OX_LOG_ERROR("Can't create asset {}!", uuid.str());
    return UUID(nullptr);
  }

  auto& asset = asset_it->second;
  asset.uuid = uuid;
  asset.type = type;
  asset.path = path;

  return asset.uuid;
}

auto AssetManager::import_asset(const std::filesystem::path& path) -> UUID {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Trying to import an asset '{}' that doesn't exist.", path);
    return UUID(nullptr);
  }

  auto asset_type = AssetType::None;
  switch (this->to_asset_file_type(path)) {
    case AssetFileType::Meta: {
      return this->register_asset(path);
    }
    case AssetFileType::GLB:
    case AssetFileType::GLTF: {
      asset_type = AssetType::Model;
      break;
    }
    case AssetFileType::PNG:
    case AssetFileType::JPEG:
    case AssetFileType::DDS:
    case AssetFileType::KTX2: {
      asset_type = AssetType::Texture;
      break;
    }
    case ox::AssetFileType::LUA: {
      asset_type = AssetType::Script;
      break;
    }
    default: {
      return UUID(nullptr);
    }
  }

  // Check for meta file before creating new asset
  auto meta_path = stack.format("{}.oxasset", path);
  if (std::filesystem::exists(meta_path)) {
    return this->register_asset(meta_path);
  }

  auto uuid = this->create_asset(asset_type, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset_type);

  switch (asset_type) {
    case AssetType::Model: {
      auto gltf_model = GLTFMeshInfo::parse_info(path);
      if (!gltf_model.has_value()) {
        OX_LOG_ERROR("Couldn't generate metadata for asset: {}", path);
        return UUID(nullptr);
      }
      auto textures = std::vector<UUID>();
      auto embedded_textures = std::vector<UUID>();
      for (auto& v : gltf_model->textures) {
        auto& image = gltf_model->images[v.image_index.value()];
        auto& texture_uuid = textures.emplace_back();

        std::visit(
          ox::match{
            [&](const std::vector<u8>& data) {
              texture_uuid = this->create_asset(AssetType::Texture, {});
              embedded_textures.push_back(texture_uuid);
            },
            [&](const std::filesystem::path& image_path) { //
              texture_uuid = this->import_asset(image_path);
            },
          },
          image.image_data
        );
      }

      auto material_uuids = std::vector<UUID>(gltf_model->materials.size());
      auto materials = std::vector<Material>(gltf_model->materials.size());
      for (const auto& [material_uuid, material, gltf_material] :
           std::views::zip(material_uuids, materials, gltf_model->materials)) {
        material_uuid = this->create_asset(AssetType::Material);
        material.albedo_color = gltf_material.albedo_color;
        material.emissive_color = gltf_material.emissive_color;
        material.roughness_factor = gltf_material.roughness_factor;
        material.metallic_factor = gltf_material.metallic_factor;
        material.alpha_mode = static_cast<AlphaMode>(gltf_material.alpha_mode);
        material.alpha_cutoff = gltf_material.alpha_cutoff;

        if (auto tex_idx = gltf_material.albedo_texture_index; tex_idx.has_value()) {
          material.albedo_texture = textures[tex_idx.value()];
        }

        if (auto tex_idx = gltf_material.normal_texture_index; tex_idx.has_value()) {
          material.normal_texture = textures[tex_idx.value()];
        }

        if (auto tex_idx = gltf_material.emissive_texture_index; tex_idx.has_value()) {
          material.emissive_texture = textures[tex_idx.value()];
        }

        if (auto tex_idx = gltf_material.metallic_roughness_texture_index; tex_idx.has_value()) {
          material.metallic_roughness_texture = textures[tex_idx.value()];
        }

        if (auto tex_idx = gltf_material.occlusion_texture_index; tex_idx.has_value()) {
          material.occlusion_texture = textures[tex_idx.value()];
        }
      }

      write_mesh_asset_meta(writer, embedded_textures, material_uuids, materials);
    } break;
    case AssetType::Texture: {
      Texture texture = {};

      write_texture_asset_meta(writer, &texture);
    } break;
    case ox::AssetType::Script: {
      write_script_asset_meta(writer, nullptr);
      break;
    }
    default:;
  }

  if (!end_asset_meta(writer, path)) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::delete_asset(const UUID& uuid) -> void {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (asset->ref_count > 0) {
    OX_LOG_WARN("Deleting alive asset {} with {} references!", asset->uuid.str(), asset->ref_count);
  }

  if (asset->is_loaded()) {
    asset->ref_count = ox::min(asset->ref_count, 1_u64);
    this->unload_asset(uuid);

    {
      auto write_lock = std::unique_lock(registry_mutex);
      asset_registry.erase(uuid);
    }
  }

  OX_LOG_TRACE("Deleted asset {}.", uuid.str());
}

auto AssetManager::register_asset(const std::filesystem::path& path) -> UUID {
  ZoneScoped;

  memory::ScopedStack stack;

  auto meta_json = read_meta_file(path);
  if (!meta_json) {
    return UUID(nullptr);
  }

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

  auto asset_path = path;
  asset_path.replace_extension("");
  auto uuid = UUID::from_string(uuid_json.value_unsafe()).value();
  auto type = static_cast<AssetType>(type_json.value_unsafe().get_uint64());

  if (!this->register_asset(uuid, type, asset_path)) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::register_asset(const UUID& uuid, AssetType type, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(registry_mutex);

  auto [asset_it, inserted] = asset_registry.try_emplace(uuid);
  if (!inserted) {
    if (asset_it != asset_registry.end()) {
      // Tried a reinsert, asset already exists
      return true;
    }

    return false;
  }

  auto& asset = asset_it->second;
  asset.uuid = uuid;
  asset.path = path;
  asset.type = type;

  OX_LOG_INFO("Registered new asset: {}:{}", to_asset_type_sv(asset.type), uuid.str());

  return true;
}

auto AssetManager::export_asset(const UUID& uuid, const std::filesystem::path& path) -> bool {
  auto* asset = this->get_asset(uuid);

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset->type);

  switch (asset->type) {
    case AssetType::Texture: {
      if (!this->export_texture(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case AssetType::Model: {
      if (!this->export_model(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case AssetType::Scene: {
      if (!this->export_scene(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case ox::AssetType::Material: {
      if (!this->export_material(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case ox::AssetType::Script: {
      if (!this->export_script(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    default: return false;
  }

  return end_asset_meta(writer, path);
}

auto AssetManager::export_texture(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* texture = this->get_texture(uuid);
  OX_CHECK_NULL(texture);
  return write_texture_asset_meta(writer, texture);
}

auto AssetManager::export_model(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* model = this->get_model(uuid);
  OX_CHECK_NULL(model);

  auto materials = std::vector<Material>(model->initial_materials.size());
  for (const auto& [material_uuid, material] : std::views::zip(model->initial_materials, materials)) {
    material = *this->get_material(material_uuid);
  }

  return write_mesh_asset_meta(writer, model->embedded_textures, model->initial_materials, materials);
}

auto AssetManager::export_scene(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* scene = this->get_scene(uuid);
  OX_CHECK_NULL(scene);
  write_scene_asset_meta(writer, scene);

  return scene->save_to_file(path);
}

auto AssetManager::export_material(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* material = this->get_material(uuid);
  OX_CHECK_NULL(material);

  writer.key("material");
  auto result = write_material_asset_meta(writer, uuid, *material);

  return result;
}

auto AssetManager::export_script(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  return write_texture_asset_meta(writer, nullptr);
}

auto AssetManager::load_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  switch (asset->type) {
    case AssetType::Model: {
      return this->load_model(uuid);
    }
    case AssetType::Texture: {
      return this->load_texture(uuid);
    }
    case AssetType::Scene: {
      return this->load_scene(uuid);
    }
    case AssetType::Audio: {
      return this->load_audio(uuid);
    }
    case AssetType::Script: {
      return this->load_script(uuid);
    }
    case AssetType::Material: {
      return this->load_material(uuid, {});
    }
    default:;
  }

  return false;
}

auto AssetManager::unload_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  switch (asset->type) {
    case AssetType::Model: {
      return this->unload_model(uuid);
    } break;
    case AssetType::Texture: {
      return this->unload_texture(uuid);
    } break;
    case AssetType::Scene: {
      return this->unload_scene(uuid);
    } break;
    case AssetType::Audio: {
      return this->unload_audio(uuid);
      break;
    }
    case AssetType::Script: {
      return this->unload_script(uuid);
      break;
    }
    case AssetType::Material: {
      return this->unload_material(uuid);
      break;
    }
    default:;
  }

  return false;
}

auto AssetManager::load_model(const UUID& uuid) -> bool {
  ZoneScoped;

  memory::ScopedStack stack;

  auto asset_path = std::filesystem::path{};
  {
    auto* asset = this->get_asset(uuid);
    if (asset->is_loaded()) {
      // Model is collection of multiple assets and all child
      // assets must be alive to safely process meshes.
      // Don't acquire child refs.
      asset->acquire_ref();

      return true;
    }

    asset_path = asset->path;
    asset->acquire_ref();
  }

  // Initial parsing
  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(asset_path);
  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    OX_LOG_ERROR("GLTF model type is invalid!");
    return false;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), asset_path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(gltf_result.error()));
    return false;
  }

  auto gltf_asset = std::move(gltf_result.get());
  if (gltf_asset.scenes.size() != 1) {
    OX_LOG_ERROR("Error loading {}. The GLTF scene can only contain one scene.", asset_path);
    return false;
  }

  auto meta_path = std::filesystem::path(asset_path.string() + ".oxasset");
  auto meta_json = read_meta_file(meta_path);
  if (!meta_json) {
    return false;
  }

  auto model = Model{};

  // extract UUIDs from meta file
  auto textures = std::vector<UUID>(gltf_asset.textures.size());
  auto embedded_textures_json = meta_json->doc["embedded_textures"];
  for (auto embedded_texture_obj_json : embedded_textures_json.get_array()) {
    for (auto field_json : embedded_texture_obj_json.get_object()) {
      if (field_json.error()) {
        OX_LOG_ERROR("Failed to import model {}! An element of `embedded_textures` is not an object.", asset_path);
        return false;
      }

      auto texture_uuid_str = field_json.unescaped_key();
      if (embedded_texture_obj_json.error()) {
        OX_LOG_ERROR("Failed to import model {}! An element of `embedded_textures` is not a string.", asset_path);
        return false;
      }

      auto texture_uuid = UUID::from_string(embedded_texture_obj_json.value_unsafe());
      if (!texture_uuid.has_value()) {
        OX_LOG_ERROR("Failed to import model {}! An embedded texture with corrupt UUID.", asset_path);
        return false;
      }

      auto field_value_json = field_json.value();
      auto image_index_json = field_value_json["image_index"].get_int64();
      if (!image_index_json.error()) {
        OX_LOG_ERROR(
          "Failed to import model {}! An element of `embedded_textures` contain an invalid `image_index` field.",
          asset_path
        );
        return false;
      }

      auto image_index = static_cast<usize>(image_index_json.value_unsafe());
      if (textures.size() <= image_index) {
        textures.resize(image_index + 1);
      }
      textures[image_index] = texture_uuid.value();
    }
  }

  // determine and initialize texture info
  for (const auto& [texture_uuid, gltf_texture] : std::views::zip(textures, gltf_asset.textures)) {
    if (auto& image_index = gltf_texture.imageIndex; image_index.has_value()) {
      auto& image = gltf_asset.images[image_index.value()];

      auto mapped_file = File{};
      auto texture_load_info = TextureLoadInfo{};
      std::visit(
        ox::match{
          [](const auto&) {},
          [&](fastgltf::sources::BufferView& v) {
            // Embedded buffer
            auto& buffer_view = gltf_asset.bufferViews[v.bufferViewIndex];
            auto& buffer = gltf_asset.buffers[buffer_view.bufferIndex];
            std::visit(
              ox::match{
                [](const auto&) {},
                [&](fastgltf::sources::Array& array) {
                  texture_load_info.bytes = std::span(
                    reinterpret_cast<u8*>(array.bytes.data() + buffer_view.byteOffset),
                    buffer_view.byteLength
                  );
                },
              },
              buffer.data
            );

            OX_ASSERT(register_asset(texture_uuid, AssetType::Texture, asset_path));
            texture_load_info.mime = gltf_mime_type_to_texture_mime_type(v.mimeType);
          },
          [&](fastgltf::sources::Array& v) {
            // Embedded array
            OX_ASSERT(register_asset(texture_uuid, AssetType::Texture, asset_path));
            texture_load_info.mime = gltf_mime_type_to_texture_mime_type(v.mimeType);
          },
          [&](fastgltf::sources::URI& uri) {
            // External file
            const auto& image_path = uri.uri.path();
            mapped_file = File(image_path, FileAccess::Read);
            auto* mapped_data = static_cast<u8*>(mapped_file.map());
            OX_ASSERT(mapped_file.error == FileError::None);
            texture_load_info.bytes = std::span(mapped_data, mapped_file.size);
            texture_load_info.mime = gltf_mime_type_to_texture_mime_type(uri.mimeType);
          },
        },
        image.data
      );

      if (gltf_texture.samplerIndex.has_value()) {
        auto& sampler = gltf_asset.samplers[gltf_texture.samplerIndex.value()];
        texture_load_info.sampler_info = gltf_sampler_to_sampler(sampler);
      }

      load_texture(texture_uuid, std::move(texture_load_info));
    }
  }

  // Load registered UUIDs.
  auto materials_json = meta_json->doc["materials"].get_array();

  auto materials = std::vector<Material>();
  for (auto material_json : materials_json) {
    auto material_uuid_json = material_json["uuid"].get_string().value_unsafe();
    auto material_uuid = UUID::from_string(material_uuid_json);
    if (!material_uuid.has_value()) {
      OX_LOG_ERROR("Failed to import model {}! A material with corrupt UUID.", asset_path);
      return false;
    }

    this->register_asset(material_uuid.value(), AssetType::Material, asset_path);
    model.initial_materials.emplace_back(material_uuid.value());

    auto& material = materials.emplace_back();
    read_material_data(&material, material_json.value_unsafe());
  }

  // for (const auto& [material_uuid, material, gltf_material] :
  //      std::views::zip(model.initial_materials, materials, gltf_asset.materials)) {
  //   if (auto texture_index = gltf_material.pbrData.baseColorTexture; texture_index.has_value()) {
  //     auto& info = texture_info_map[material.albedo_texture];
  //     load_texture_bytes(texture_index.value(), info);
  //   }
  //
  //   if (auto texture_index = gltf_material.normal_texture_index; texture_index.has_value()) {
  //     auto& info = texture_info_map[material.normal_texture];
  //     load_texture_bytes(texture_index.value(), info);
  //   }
  //
  //   if (auto texture_index = gltf_material.emissive_texture_index; texture_index.has_value()) {
  //     auto& info = texture_info_map[material.emissive_texture];
  //     load_texture_bytes(texture_index.value(), info);
  //   }
  //
  //   if (auto texture_index = gltf_material.metallic_roughness_texture_index; texture_index.has_value()) {
  //     auto& info = texture_info_map[material.metallic_roughness_texture];
  //     load_texture_bytes(texture_index.value(), info);
  //   }
  //
  //   if (auto texture_index = gltf_material.occlusion_texture_index; texture_index.has_value()) {
  //     auto& info = texture_info_map[material.occlusion_texture];
  //     load_texture_bytes(texture_index.value(), info);
  //   }
  //
  //   asset_man.load_material(material_uuid, material, texture_info_map);
  // }

  auto& gltf_default_scene = gltf_asset.scenes[gltf_asset.defaultScene.value_or(0_sz)];
  struct ProcessingNode {
    usize gltf_node_index = 0;
    usize parent_mesh_group_index = 0;
  };
  auto processing_gltf_nodes = std::queue<ProcessingNode>();

  auto& root_mesh_group = model.mesh_groups.emplace_back();
  root_mesh_group.name = gltf_default_scene.name;
  for (auto node_index : gltf_default_scene.nodeIndices) {
    processing_gltf_nodes.push({node_index, 0});
  }

  auto& vk_context = App::get()->get_vkcontext();

  while (!processing_gltf_nodes.empty()) {
    auto [gltf_node_index, parent_mesh_group_index] = processing_gltf_nodes.front();
    const auto& node = gltf_asset.nodes[gltf_node_index];
    auto& parent_mesh_group = model.mesh_groups[parent_mesh_group_index];
    processing_gltf_nodes.pop();

    auto mesh_group_index = model.mesh_groups.size();
    parent_mesh_group.child_indices.push_back(mesh_group_index);

    auto& mesh_group = model.mesh_groups.emplace_back();
    mesh_group.name = node.name;

    for (auto child_node_index : node.children) {
      processing_gltf_nodes.push({child_node_index, mesh_group_index});
    }

    // Node translation
    auto translation = glm::vec3{};
    auto rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    auto scale = glm::vec3{};
    if (auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
      translation = glm::make_vec3(trs->translation.data());
      rotation = glm::quat::wxyz(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
      scale = glm::make_vec3(trs->scale.data());
    } else if (auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
      auto transform_mat = glm::make_mat4x4(mat->data());
      auto skew = glm::vec3{};
      auto perspective = glm::vec4{};
      glm::decompose(transform_mat, scale, rotation, translation, skew, perspective);
    }

    mesh_group.translation = translation;
    mesh_group.rotation = rotation;
    mesh_group.scale = scale;

    if (!node.meshIndex.has_value()) {
      continue;
    }

    const auto& gltf_mesh = gltf_asset.meshes[node.meshIndex.value()];
    for (const auto& gltf_primitive : gltf_mesh.primitives) {
      if (!gltf_primitive.indicesAccessor.has_value() || !gltf_primitive.materialIndex.has_value()) {
        continue;
      }

      auto gpu_mesh = GPU::Mesh{};

      auto& index_accessor = gltf_asset.accessors[gltf_primitive.indicesAccessor.value()];
      auto raw_indices = std::vector<u32>(index_accessor.count);
      fastgltf::iterateAccessorWithIndex<u32>(gltf_asset, index_accessor, [&](u32 index, usize i) { //
        raw_indices[i] = index;
      });

      auto vertex_count = 0_u32;
      auto vertex_remap = std::vector<u32>();
      auto positions = std::vector<glm::vec3>();
      if (auto attrib = gltf_primitive.findAttribute("POSITION"); attrib != gltf_primitive.attributes.end()) {
        auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
        auto raw_positions = std::vector<glm::vec3>(accessor.count);
        vertex_remap.resize(accessor.count);

        fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 pos, usize i) { //
          raw_positions[i] = pos;
        });

        vertex_count = meshopt_optimizeVertexFetchRemap(
          vertex_remap.data(),
          raw_indices.data(),
          raw_indices.size(),
          raw_positions.size()
        );

        positions.resize(vertex_count);
        meshopt_remapVertexBuffer(
          positions.data(),
          raw_positions.data(),
          raw_positions.size(),
          sizeof(glm::vec3),
          vertex_remap.data()
        );
      }

      auto normals = std::vector<glm::vec3>();
      if (auto attrib = gltf_primitive.findAttribute("NORMAL"); attrib != gltf_primitive.attributes.end()) {
        auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
        auto raw_normals = std::vector<glm::vec3>(accessor.count);

        fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 normal, usize i) { //
          raw_normals[i] = normal;
        });

        normals.resize(vertex_count);
        meshopt_remapVertexBuffer(
          normals.data(),
          raw_normals.data(),
          raw_normals.size(),
          sizeof(glm::vec3),
          vertex_remap.data()
        );
      }

      auto texcoords = std::vector<glm::vec2>();
      if (auto attrib = gltf_primitive.findAttribute("TEXCOORD_0"); attrib != gltf_primitive.attributes.end()) {
        auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
        auto raw_texcoords = std::vector<glm::vec2>(accessor.count);

        fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf_asset, accessor, [&](glm::vec2 uv, usize i) { //
          raw_texcoords[i] = uv;
        });

        texcoords.resize(vertex_count);
        meshopt_remapVertexBuffer(
          texcoords.data(),
          raw_texcoords.data(),
          raw_texcoords.size(),
          sizeof(glm::vec2),
          vertex_remap.data()
        );
      }

      auto indices = std::vector<u32>(index_accessor.count);
      meshopt_remapIndexBuffer(indices.data(), raw_indices.data(), raw_indices.size(), vertex_remap.data());

      const auto mesh_upload_size = 0                           //
                                    + ox::size_bytes(positions) //
                                    + ox::size_bytes(normals)   //
                                    + ox::size_bytes(texcoords);
      auto upload_size = mesh_upload_size;

      auto lod_cpu_buffers = std::array<std::pair<vuk::Value<vuk::Buffer>, u64>, GPU::Mesh::MAX_LODS>();
      auto last_lod_indices = std::vector<u32>();
      for (auto lod_index = 0_sz; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
        ZoneNamedN(z, "GPU Meshlet Generation", true);

        auto& cur_lod = gpu_mesh.lods[lod_index];
        auto simplified_indices = std::vector<u32>();
        if (lod_index == 0) {
          simplified_indices = std::vector<u32>(indices.begin(), indices.end());
        } else {
          const auto& last_lod = gpu_mesh.lods[lod_index - 1];
          auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
          simplified_indices.resize(last_lod_indices.size(), 0_u32);
          constexpr auto TARGET_ERROR = std::numeric_limits<f32>::max();
          constexpr f32 NORMAL_WEIGHTS[] = {1.0f, 1.0f, 1.0f};

          auto result_error = 0.0f;
          auto result_index_count = meshopt_simplifyWithAttributes(
            simplified_indices.data(),
            last_lod_indices.data(),
            last_lod_indices.size(),
            reinterpret_cast<const f32*>(positions.data()),
            vertex_count,
            sizeof(glm::vec3),
            reinterpret_cast<const f32*>(normals.data()),
            sizeof(glm::vec3),
            NORMAL_WEIGHTS,
            ox::count_of(NORMAL_WEIGHTS),
            nullptr,
            lod_index_count,
            TARGET_ERROR,
            meshopt_SimplifyLockBorder,
            &result_error
          );

          cur_lod.error = last_lod.error + result_error;
          if (result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 ||
              result_index_count < 6) {
            // Error bound
            break;
          }

          simplified_indices.resize(result_index_count);
        }

        gpu_mesh.vertex_count = vertex_count;
        gpu_mesh.lod_count += 1;
        last_lod_indices = simplified_indices;

        meshopt_optimizeVertexCache(
          simplified_indices.data(),
          simplified_indices.data(),
          simplified_indices.size(),
          vertex_count
        );

        // Worst case count
        auto max_meshlet_count = meshopt_buildMeshletsBound(
          simplified_indices.size(),
          Model::MAX_MESHLET_INDICES,
          Model::MAX_MESHLET_PRIMITIVES
        );
        auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
        auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * Model::MAX_MESHLET_INDICES);
        auto local_triangle_indices = std::vector<u8>(max_meshlet_count * Model::MAX_MESHLET_PRIMITIVES * 3);

        auto meshlet_count = meshopt_buildMeshlets(
          raw_meshlets.data(),
          indirect_vertex_indices.data(),
          local_triangle_indices.data(),
          simplified_indices.data(),
          simplified_indices.size(),
          reinterpret_cast<const f32*>(positions.data()),
          vertex_count,
          sizeof(glm::vec3),
          Model::MAX_MESHLET_INDICES,
          Model::MAX_MESHLET_PRIMITIVES,
          0.0
        );

        // Trim meshlets from worst case to current case
        raw_meshlets.resize(meshlet_count);
        auto meshlets = std::vector<GPU::Meshlet>(meshlet_count);
        const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
        indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
        local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32));

        auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
        auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
        auto meshlet_bounds = std::vector<GPU::Bounds>(meshlet_count);
        for (const auto& [raw_meshlet, meshlet, bounds] : std::views::zip(raw_meshlets, meshlets, meshlet_bounds)) {
          // AABB computation
          auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
          auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
          for (u32 i = 0; i < raw_meshlet.triangle_count * 3; i++) {
            auto local_triangle_index_offset = raw_meshlet.triangle_offset + i;
            OX_ASSERT(local_triangle_index_offset < local_triangle_indices.size());
            auto local_triangle_index = local_triangle_indices[local_triangle_index_offset];
            OX_ASSERT(local_triangle_index < raw_meshlet.vertex_count);
            auto indirect_vertex_index_offset = raw_meshlet.vertex_offset + local_triangle_index;
            OX_ASSERT(indirect_vertex_index_offset < indirect_vertex_indices.size());
            auto indirect_vertex_index = indirect_vertex_indices[indirect_vertex_index_offset];
            OX_ASSERT(indirect_vertex_index < vertex_count);

            const auto& tri_pos = positions[indirect_vertex_index];
            meshlet_bb_min = glm::min(meshlet_bb_min, tri_pos);
            meshlet_bb_max = glm::max(meshlet_bb_max, tri_pos);
          }

          // Sphere and Cone computation
          auto sphere_bounds = meshopt_computeMeshletBounds(
            &indirect_vertex_indices[raw_meshlet.vertex_offset],
            &local_triangle_indices[raw_meshlet.triangle_offset],
            raw_meshlet.triangle_count,
            reinterpret_cast<f32*>(positions.data()),
            vertex_count,
            sizeof(glm::vec3)
          );

          meshlet.indirect_vertex_index_offset = raw_meshlet.vertex_offset;
          meshlet.local_triangle_index_offset = raw_meshlet.triangle_offset;
          meshlet.vertex_count = raw_meshlet.vertex_count;
          meshlet.triangle_count = raw_meshlet.triangle_count;

          bounds.aabb_center = (meshlet_bb_max + meshlet_bb_min) * 0.5f;
          bounds.aabb_extent = meshlet_bb_max - meshlet_bb_min;
          bounds.sphere_center = glm::make_vec3(sphere_bounds.center);
          bounds.sphere_radius = sphere_bounds.radius;

          mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
          mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
        }

        gpu_mesh.bounds.aabb_center = (mesh_bb_max + mesh_bb_min) * 0.5f;
        gpu_mesh.bounds.aabb_extent = mesh_bb_max - mesh_bb_min;

        auto lod_upload_size = 0                                        //
                               + ox::size_bytes(simplified_indices)     //
                               + ox::size_bytes(meshlets)               //
                               + ox::size_bytes(meshlet_bounds)         //
                               + ox::size_bytes(local_triangle_indices) //
                               + ox::size_bytes(indirect_vertex_indices);
        auto cpu_lod_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, lod_upload_size);
        auto cpu_lod_ptr = reinterpret_cast<u8*>(cpu_lod_buffer->mapped_ptr);

        auto upload_offset = 0_u64;
        cur_lod.indices = upload_offset;
        std::memcpy(cpu_lod_ptr + upload_offset, simplified_indices.data(), ox::size_bytes(simplified_indices));
        upload_offset += ox::size_bytes(simplified_indices);

        cur_lod.meshlets = upload_offset;
        std::memcpy(cpu_lod_ptr + upload_offset, meshlets.data(), ox::size_bytes(meshlets));
        upload_offset += ox::size_bytes(meshlets);

        cur_lod.meshlet_bounds = upload_offset;
        std::memcpy(cpu_lod_ptr + upload_offset, meshlet_bounds.data(), ox::size_bytes(meshlet_bounds));
        upload_offset += ox::size_bytes(meshlet_bounds);

        cur_lod.local_triangle_indices = upload_offset;
        std::memcpy(cpu_lod_ptr + upload_offset, local_triangle_indices.data(), ox::size_bytes(local_triangle_indices));
        upload_offset += ox::size_bytes(local_triangle_indices);

        cur_lod.indirect_vertex_indices = upload_offset;
        std::memcpy(
          cpu_lod_ptr + upload_offset,
          indirect_vertex_indices.data(),
          ox::size_bytes(indirect_vertex_indices)
        );
        upload_offset += ox::size_bytes(indirect_vertex_indices);

        cur_lod.indices_count = simplified_indices.size();
        cur_lod.meshlet_count = meshlet_count;
        cur_lod.meshlet_bounds_count = meshlet_bounds.size();
        cur_lod.local_triangle_indices_count = local_triangle_indices.size();
        cur_lod.indirect_vertex_indices_count = indirect_vertex_indices.size();

        lod_cpu_buffers[lod_index] = std::pair(cpu_lod_buffer, lod_upload_size);
        upload_size += lod_upload_size;
      }

      auto mesh_upload_offset = 0_u64;

      auto gpu_mesh_buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, upload_size);

      // Mesh first
      auto cpu_mesh_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, mesh_upload_size);
      auto cpu_mesh_ptr = reinterpret_cast<u8*>(cpu_mesh_buffer->mapped_ptr);

      auto gpu_mesh_bda = gpu_mesh_buffer->device_address;
      gpu_mesh.vertex_positions = gpu_mesh_bda + mesh_upload_offset;
      std::memcpy(cpu_mesh_ptr + mesh_upload_offset, positions.data(), ox::size_bytes(positions));
      mesh_upload_offset += ox::size_bytes(positions);

      gpu_mesh.vertex_normals = gpu_mesh_bda + mesh_upload_offset;
      std::memcpy(cpu_mesh_ptr + mesh_upload_offset, normals.data(), ox::size_bytes(normals));
      mesh_upload_offset += ox::size_bytes(normals);

      if (!texcoords.empty()) {
        gpu_mesh.texture_coords = gpu_mesh_bda + mesh_upload_offset;
        std::memcpy(cpu_mesh_ptr + mesh_upload_offset, texcoords.data(), ox::size_bytes(texcoords));
        mesh_upload_offset += ox::size_bytes(texcoords);
      }

      auto gpu_mesh_subrange = vuk::discard_buf("mesh", gpu_mesh_buffer->subrange(0, mesh_upload_size));
      gpu_mesh_subrange = vk_context.upload_staging(std::move(cpu_mesh_buffer), std::move(gpu_mesh_subrange));
      vk_context.wait_on(std::move(gpu_mesh_subrange));

      for (auto lod_index = 0_sz; lod_index < gpu_mesh.lod_count; lod_index++) {
        auto&& [lod_cpu_buffer, lod_upload_size] = lod_cpu_buffers[lod_index];
        auto& lod = gpu_mesh.lods[lod_index];

        lod.indices += gpu_mesh_bda + mesh_upload_offset;
        lod.meshlets += gpu_mesh_bda + mesh_upload_offset;
        lod.meshlet_bounds += gpu_mesh_bda + mesh_upload_offset;
        lod.local_triangle_indices += gpu_mesh_bda + mesh_upload_offset;
        lod.indirect_vertex_indices += gpu_mesh_bda + mesh_upload_offset;

        auto gpu_lod_subrange = vuk::discard_buf(
          "mesh lod subrange",
          gpu_mesh_buffer->subrange(mesh_upload_offset, lod_upload_size)
        );
        gpu_lod_subrange = vk_context.upload_staging(std::move(lod_cpu_buffer), std::move(gpu_lod_subrange));
        vk_context.wait_on(std::move(gpu_lod_subrange));

        mesh_upload_offset += lod_upload_size;
      }

      auto mesh_index = model.gpu_meshes.size();
      const auto& initial_material = model.initial_materials[gltf_primitive.materialIndex.value()];
      mesh_group.mesh_indices.push_back(mesh_index);
      model.initial_materials.push_back(initial_material);
      model.gpu_meshes.push_back(gpu_mesh);
      model.gpu_mesh_buffers.push_back(std::move(gpu_mesh_buffer));
    }
  }

  return true;
}

auto AssetManager::unload_model(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  auto* model = this->get_model(asset->model_id);
  for (auto& v : model->initial_materials) {
    this->unload_material(v);
  }

  model_map.destroy_slot(asset->model_id);
  asset->model_id = ModelID::Invalid;

  OX_LOG_TRACE("Unloaded model {}", uuid.str());

  return true;
}

auto AssetManager::load_texture(const UUID& uuid, TextureLoadInfo info) -> bool {
  ZoneScoped;

  auto read_lock = std::shared_lock(textures_mutex);
  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded()) {
    return true;
  }

  read_lock.unlock();

  {
    Texture texture{};
    texture.create(asset->path, std::move(info));

    auto write_lock = std::unique_lock(textures_mutex);
    asset->texture_id = texture_map.create_slot(std::move(texture));

    OX_LOG_INFO("Loaded texture {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->texture_id).index);
  }

  return true;
}

auto AssetManager::unload_texture(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  texture_map.destroy_slot(asset->texture_id);
  asset->texture_id = TextureID::Invalid;

  OX_LOG_TRACE("Unloaded texture {}", uuid.str());

  return true;
}

auto AssetManager::is_texture_loaded(const UUID& uuid) -> bool {
  ZoneScoped;

  std::shared_lock _(textures_mutex);
  auto* asset = this->get_asset(uuid);
  if (!asset) {
    return false;
  }

  return asset->is_loaded();
}

auto AssetManager::load_material(const UUID& uuid, const Material& info, const MateriaLoadInfo& load_info) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (asset->is_loaded()) {
    asset->acquire_ref();
    return true;
  }

  asset->material_id = material_map.create_slot({
    .albedo_color = info.albedo_color,
    .emissive_color = info.emissive_color,
    .roughness_factor = info.roughness_factor,
    .metallic_factor = info.metallic_factor,
    .alpha_mode = info.alpha_mode,
    .alpha_cutoff = info.alpha_cutoff,
    .albedo_texture = info.albedo_texture,
    .normal_texture = info.normal_texture,
    .emissive_texture = info.emissive_texture,
    .metallic_roughness_texture = info.metallic_roughness_texture,
    .occlusion_texture = info.occlusion_texture,
  });

  auto& job_man = App::get_job_manager();

  if (info.albedo_texture) {
    auto job = Job::create([this,
                            texture_uuid = info.albedo_texture,
                            texture_info = std::move(load_info.albedo_texture)]() { load_texture(texture_uuid); });
    job_man.submit(std::move(job));
  }

  if (info.normal_texture) {
    auto job = Job::create(
      [this, texture_uuid = info.normal_texture, texture_info = std::move(load_info.normal_texture)]() {
        load_texture(texture_uuid, {.format = vuk::Format::eR8G8B8A8Unorm});
      }
    );
    job_man.submit(std::move(job));
  }

  if (info.emissive_texture) {
    auto job = Job::create([this,
                            texture_uuid = info.emissive_texture,
                            texture_info = std::move(load_info.emissive_texture)]() { load_texture(texture_uuid); });
    job_man.submit(std::move(job));
  }

  if (info.metallic_roughness_texture) {
    auto job = Job::create([this,
                            texture_uuid = info.metallic_roughness_texture,
                            texture_info = std::move(load_info.metallic_roughness_texture)]() {
      load_texture(texture_uuid, {.format = vuk::Format::eR8G8B8A8Unorm});
    });
    job_man.submit(std::move(job));
  }

  if (info.occlusion_texture) {
    auto job = Job::create(
      [this, texture_uuid = info.occlusion_texture, texture_info = std::move(load_info.occlusion_texture)]() {
        load_texture(texture_uuid, {.format = vuk::Format::eR8G8B8A8Unorm});
      }
    );
    job_man.submit(std::move(job));
  }

  this->set_material_dirty(asset->material_id);
  asset->acquire_ref();

  return true;
}

auto AssetManager::unload_material(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  const auto* material = this->get_material(asset->material_id);
  if (material->albedo_texture) {
    this->unload_texture(material->albedo_texture);
  }

  if (material->normal_texture) {
    this->unload_texture(material->normal_texture);
  }

  if (material->emissive_texture) {
    this->unload_texture(material->emissive_texture);
  }

  if (material->metallic_roughness_texture) {
    this->unload_texture(material->metallic_roughness_texture);
  }

  if (material->occlusion_texture) {
    this->unload_texture(material->occlusion_texture);
  }

  material_map.destroy_slot(asset->material_id);
  asset->material_id = MaterialID::Invalid;

  OX_LOG_INFO("Unloaded material {}", uuid.str());

  return true;
}

auto AssetManager::load_scene(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  asset->scene_id = this->scene_map.create_slot(std::make_unique<Scene>());
  auto* scene = this->scene_map.slot(asset->scene_id)->get();

  scene->init("unnamed_scene");

  if (!scene->load_from_file(asset->path)) {
    return false;
  }

  asset->acquire_ref();
  return true;
}

auto AssetManager::unload_scene(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  scene_map.destroy_slot(asset->scene_id);
  asset->scene_id = SceneID::Invalid;

  OX_LOG_INFO("Unloaded scene {}", uuid.str());

  return true;
}

auto AssetManager::load_audio(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded()) {
    return true;
  }

  AudioSource audio{};
  audio.load(asset->path);
  asset->audio_id = audio_map.create_slot(std::move(audio));

  OX_LOG_INFO("Loaded audio {}", uuid.str());

  return true;
}

auto AssetManager::unload_audio(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  auto* audio = this->get_audio(asset->audio_id);
  OX_CHECK_NULL(audio);
  audio->unload();

  audio_map.destroy_slot(asset->audio_id);
  asset->audio_id = AudioID::Invalid;

  OX_LOG_INFO("Unloaded audio {}.", uuid.str());

  return true;
}

auto AssetManager::load_script(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded())
    return true;

  asset->script_id = script_map.create_slot(std::make_unique<LuaSystem>());
  auto* system = script_map.slot(asset->script_id);
  system->get()->load(asset->path);

  OX_LOG_INFO("Loaded script {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->script_id).index);

  return true;
}

auto AssetManager::unload_script(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  script_map.destroy_slot(asset->script_id);
  asset->script_id = ScriptID::Invalid;

  OX_LOG_INFO("Unloaded script {}.", uuid.str());

  return true;
}

auto AssetManager::get_asset(const UUID& uuid) -> Asset* {
  ZoneScoped;

  auto read_lock = std::shared_lock(registry_mutex);
  const auto it = asset_registry.find(uuid);
  if (it == asset_registry.end()) {
    return nullptr;
  }

  return &it->second;
}

auto AssetManager::get_model(const UUID& uuid) -> Model* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Model);
  if (asset->type != AssetType::Model || asset->model_id == ModelID::Invalid) {
    return nullptr;
  }

  return model_map.slot(asset->model_id);
}

auto AssetManager::get_model(const ModelID model_id) -> Model* {
  ZoneScoped;

  if (model_id == ModelID::Invalid) {
    return nullptr;
  }

  return model_map.slot(model_id);
}

auto AssetManager::get_texture(const UUID& uuid) -> Texture* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Texture);
  if (asset->type != AssetType::Texture || asset->texture_id == TextureID::Invalid) {
    return nullptr;
  }

  return texture_map.slot(asset->texture_id);
}

auto AssetManager::get_texture(const TextureID texture_id) -> Texture* {
  ZoneScoped;

  if (texture_id == TextureID::Invalid) {
    return nullptr;
  }

  return texture_map.slot(texture_id);
}

auto AssetManager::get_material(const UUID& uuid) -> Material* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Material);
  if (asset->type != AssetType::Material || asset->material_id == MaterialID::Invalid) {
    return nullptr;
  }

  return material_map.slot(asset->material_id);
}

auto AssetManager::get_material(const MaterialID material_id) -> Material* {
  ZoneScoped;

  if (material_id == MaterialID::Invalid) {
    return nullptr;
  }

  return material_map.slot(material_id);
}

auto AssetManager::set_material_dirty(MaterialID material_id) -> void {
  ZoneScoped;

  std::shared_lock shared_lock(materials_mutex);
  if (std::ranges::find(dirty_materials, material_id) != dirty_materials.end()) {
    return;
  }

  shared_lock.unlock();
  materials_mutex.lock();
  dirty_materials.emplace_back(material_id);
  materials_mutex.unlock();
}

auto AssetManager::set_material_dirty(const UUID& uuid) -> void {
  ZoneScoped;

  auto material = get_asset(uuid);
  set_material_dirty(material->material_id);
}

auto AssetManager::set_all_materials_dirty(this AssetManager& self) -> void {
  ZoneScoped;

  for (auto& [uuid, asset] : self.asset_registry) {
    if (asset.type == AssetType::Material) {
      self.set_material_dirty(asset.material_id);
    }
  }
}

auto AssetManager::get_dirty_material_ids(this AssetManager& self) -> std::vector<MaterialID> {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.materials_mutex);
  auto dirty_materials = std::vector(self.dirty_materials);

  read_lock.unlock();
  auto write_lock = std::unique_lock(self.materials_mutex);
  self.dirty_materials.clear();

  return dirty_materials;
}

auto AssetManager::get_scene(const UUID& uuid) -> Scene* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Scene);
  if (asset->type != AssetType::Scene || asset->scene_id == SceneID::Invalid) {
    return nullptr;
  }

  return scene_map.slot(asset->scene_id)->get();
}

auto AssetManager::get_scene(const SceneID scene_id) -> Scene* {
  ZoneScoped;

  if (scene_id == SceneID::Invalid) {
    return nullptr;
  }

  return scene_map.slot(scene_id)->get();
}

auto AssetManager::get_audio(const UUID& uuid) -> AudioSource* {
  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Audio);
  if (asset->type != AssetType::Audio || asset->audio_id == AudioID::Invalid) {
    return nullptr;
  }

  return audio_map.slot(asset->audio_id);
}

auto AssetManager::get_audio(const AudioID audio_id) -> AudioSource* {
  ZoneScoped;

  if (audio_id == AudioID::Invalid) {
    return nullptr;
  }

  return audio_map.slot(audio_id);
}

auto AssetManager::get_script(const UUID& uuid) -> LuaSystem* {
  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Script);
  if (asset->type != AssetType::Script || asset->script_id == ScriptID::Invalid) {
    return nullptr;
  }

  return script_map.slot(asset->script_id)->get();
}

auto AssetManager::get_script(ScriptID script_id) -> LuaSystem* {
  ZoneScoped;

  if (script_id == ScriptID::Invalid) {
    return nullptr;
  }

  return script_map.slot(script_id)->get();
}
} // namespace ox
