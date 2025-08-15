#include "Asset/AssetManager.hpp"

#include <meshoptimizer.h>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/ParserGLTF.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "Core/JobManager.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/JsonHelpers.hpp"
#include "Utils/Log.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
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

auto read_material_data(Material* mat, simdjson::ondemand::value& material_obj) -> void {
  auto albedo_color = material_obj["albedo_color"];
  json_to_vec(albedo_color.value_unsafe(), mat->albedo_color);
  auto emissive_color = material_obj["emissive_color"];
  json_to_vec(emissive_color.value_unsafe(), mat->emissive_color);
  mat->roughness_factor = static_cast<f32>(material_obj["roughness_factor"].get_double().value_unsafe());
  mat->metallic_factor = static_cast<f32>(material_obj["metallic_factor"].get_double().value_unsafe());
  mat->alpha_mode = static_cast<AlphaMode>(material_obj["alpha_mode"].get_uint64().value_unsafe());
  mat->alpha_cutoff = static_cast<f32>(material_obj["alpha_cutoff"].get_double().value_unsafe());
  mat->albedo_texture = UUID::from_string(material_obj["albedo_texture"].get_string().value_unsafe())
                            .value_or(UUID(nullptr));
  mat->normal_texture = UUID::from_string(material_obj["normal_texture"].get_string().value_unsafe())
                            .value_or(UUID(nullptr));
  mat->emissive_texture = UUID::from_string(material_obj["emissive_texture"].get_string().value_unsafe())
                              .value_or(UUID(nullptr));
  mat->metallic_roughness_texture = UUID::from_string(
                                        material_obj["metallic_roughness_texture"].get_string().value_unsafe())
                                        .value_or(UUID(nullptr));
  mat->occlusion_texture = UUID::from_string(material_obj["occlusion_texture"].get_string().value_unsafe())
                               .value_or(UUID(nullptr));
}

auto read_material_asset_meta(simdjson::ondemand::value& doc, Material* mat) -> bool {
  ZoneScoped;

  if (!mat)
    return false;

  auto material_obj = doc["material"].value_unsafe();

  read_material_data(mat, material_obj);

  return true;
}

auto write_mesh_asset_meta(JsonWriter& writer,
                           std::span<UUID> embedded_texture_uuids,
                           std::span<UUID> material_uuids,
                           std::span<Material> materials) -> bool {
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

auto end_asset_meta(JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  writer.end_obj();

  auto meta_path = path + ".oxasset";

  std::ofstream filestream(meta_path);
  filestream << writer.stream.rdbuf();

  return true;
}

auto AssetManager::init() -> std::expected<void, std::string> { return {}; }

auto AssetManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : asset_registry) {
    delete_asset(uuid);
  }

  return {};
}

auto AssetManager::registry() const -> const AssetRegistry& { return asset_registry; }

auto AssetManager::read_meta_file(const std::string& path) -> std::unique_ptr<AssetMetaFile> {
  auto content = fs::read_file(path);
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

auto AssetManager::load_deferred_assets() -> void {
  ZoneScoped;

  for (auto& task : deferred_load_queue) {
    task();
  }
  deferred_load_queue.clear();
}

auto AssetManager::to_asset_file_type(const std::string& path) -> AssetFileType {
  ZoneScoped;
  memory::ScopedStack stack;

  auto extension = ox::fs::get_file_extension(path);

  if (extension.empty()) {
    return AssetFileType::None;
  }

  extension = stack.to_upper(extension);
  switch (fnv64_str(extension)) {
    case fnv64_c("GLB")    : return AssetFileType::GLB;
    case fnv64_c("GLTF")   : return AssetFileType::GLTF;
    case fnv64_c("PNG")    : return AssetFileType::PNG;
    case fnv64_c("JPG")    :
    case fnv64_c("JPEG")   : return AssetFileType::JPEG;
    case fnv64_c("JSON")   : return AssetFileType::JSON;
    case fnv64_c("OXASSET"): return AssetFileType::Meta;
    case fnv64_c("KTX2")   : return AssetFileType::KTX2;
    case fnv64_c("LUA")    : return AssetFileType::LUA;
    default                : return AssetFileType::None;
  }
}

auto AssetManager::to_asset_type_sv(AssetType type) -> std::string_view {
  ZoneScoped;

  switch (type) {
    case AssetType::None    : return "None";
    case AssetType::Shader  : return "Shader";
    case AssetType::Mesh    : return "Mesh";
    case AssetType::Texture : return "Texture";
    case AssetType::Material: return "Material";
    case AssetType::Font    : return "Font";
    case AssetType::Scene   : return "Scene";
    case AssetType::Audio   : return "Audio";
    case AssetType::Script  : return "Script";
    default                 : return {};
  }
}

auto AssetManager::create_asset(const AssetType type, const std::string& path) -> UUID {
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

auto AssetManager::import_asset(const std::string& path) -> UUID {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!fs::exists(path)) {
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
      asset_type = AssetType::Mesh;
      break;
    }
    case AssetFileType::PNG:
    case AssetFileType::JPEG:
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
  if (fs::exists(meta_path)) {
    return this->register_asset(std::string(meta_path));
  }

  auto uuid = this->create_asset(asset_type, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset_type);

  switch (asset_type) {
    case AssetType::Mesh: {
      auto gltf_model = GLTFMeshInfo::parse_info(path);
      auto textures = std::vector<UUID>();
      auto embedded_textures = std::vector<UUID>();
      for (auto& v : gltf_model->textures) {
        auto& image = gltf_model->images[v.image_index.value()];
        auto& texture_uuid = textures.emplace_back();

        std::visit(ox::match{
                       [&](const std::vector<u8>& data) {
                         texture_uuid = this->create_asset(AssetType::Texture, {});
                         embedded_textures.push_back(texture_uuid);
                       },
                       [&](const ::fs::path& image_path) { //
                         texture_uuid = this->import_asset(image_path.string());
                       },
                   },
                   image.image_data);
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

auto AssetManager::register_asset(const std::string& path) -> UUID {
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

  auto asset_path = ::fs::path(path);
  asset_path.replace_extension("");
  auto uuid = UUID::from_string(uuid_json.value_unsafe()).value();
  auto type = static_cast<AssetType>(type_json.value_unsafe().get_uint64());

  if (!this->register_asset(uuid, type, asset_path.string())) {
    return UUID(nullptr);
  }

  switch (type) {
    case AssetType::Material: {
      Material mat = {};
      auto obj = meta_json->doc["material"].value_unsafe();
      if (read_material_asset_meta(obj, &mat)) {
        /* Since materials could contain textures that may be not yet registered
           we defer them to be loaded at the end of frame */
        deferred_load_queue.emplace_back([this, uuid, mat]() { load_material(uuid, mat); });
      } else {
        OX_LOG_ERROR("Couldn't parse material meta data!");
      }
      break;
    }
    default: break;
  }

  return uuid;
}

auto AssetManager::register_asset(const UUID& uuid, AssetType type, const std::string& path) -> bool {
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

auto AssetManager::export_asset(const UUID& uuid, const std::string& path) -> bool {
  auto* asset = this->get_asset(uuid);

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset->type);

  switch (asset->type) {
    case AssetType::Texture: {
      if (!this->export_texture(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case AssetType::Mesh: {
      if (!this->export_mesh(asset->uuid, writer, path)) {
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

auto AssetManager::export_texture(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  auto* texture = this->get_texture(uuid);
  OX_CHECK_NULL(texture);
  return write_texture_asset_meta(writer, texture);
}

auto AssetManager::export_mesh(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  auto* mesh = this->get_mesh(uuid);
  OX_CHECK_NULL(mesh);

  auto materials = std::vector<Material>(mesh->materials.size());
  for (const auto& [material_uuid, material] : std::views::zip(mesh->materials, materials)) {
    material = *this->get_material(material_uuid);
  }

  return write_mesh_asset_meta(writer, mesh->embedded_textures, mesh->materials, materials);
}

auto AssetManager::export_scene(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  auto* scene = this->get_scene(uuid);
  OX_CHECK_NULL(scene);
  write_scene_asset_meta(writer, scene);

  return scene->save_to_file(path);
}

auto AssetManager::export_material(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  auto* material = this->get_material(uuid);
  OX_CHECK_NULL(material);
  return write_material_asset_meta(writer, uuid, *material);
}

auto AssetManager::export_script(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool {
  ZoneScoped;

  return write_texture_asset_meta(writer, nullptr);
}

auto AssetManager::load_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  switch (asset->type) {
    case AssetType::Mesh: {
      return this->load_mesh(uuid);
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
    default:;
  }

  return false;
}

auto AssetManager::unload_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  switch (asset->type) {
    case AssetType::Mesh: {
      return this->unload_mesh(uuid);
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

auto AssetManager::load_mesh(const UUID& uuid) -> bool {
  ZoneScoped;

  memory::ScopedStack stack;

  auto* asset = this->get_asset(uuid);
  if (asset->is_loaded()) {
    // Model is collection of multiple assets and all child
    // assets must be alive to safely process meshes.
    // Don't acquire child refs.
    asset->acquire_ref();

    return true;
  }

  asset->mesh_id = mesh_map.create_slot();
  auto* model = mesh_map.slot(asset->mesh_id);

  std::string meta_path = asset->path + ".oxasset";
  auto meta_json = read_meta_file(meta_path);
  if (!meta_json) {
    return false;
  }

  auto asset_path = asset->path;
  asset->acquire_ref();

  // Below we register new assets, which causes asset pointer to be invalidated.
  // set this to nullptr so it's obvious when debugging.
  asset = nullptr;

  // Load embedded textures
  ankerl::unordered_dense::map<UUID, TextureLoadInfo> texture_info_map = {};

  auto embedded_textures = std::vector<UUID>();
  auto embedded_texture_uuids_json = meta_json->doc["embedded_textures"].get_array();
  for (auto embedded_texture_uuid_json : embedded_texture_uuids_json) {
    auto embedded_texture_uuid_str = embedded_texture_uuid_json.get_string().value_unsafe();

    auto embedded_texture_uuid = UUID::from_string(embedded_texture_uuid_str);
    if (!embedded_texture_uuid.has_value()) {
      OX_LOG_ERROR("Failed to import model {}! An embedded texture with corrupt UUID.", asset_path);
      return false;
    }

    embedded_textures.push_back(embedded_texture_uuid.value());
    this->register_asset(embedded_texture_uuid.value(), AssetType::Texture, {});

    texture_info_map.emplace(embedded_texture_uuid.value(), TextureLoadInfo{});
  }

  // Load registered UUIDs.
  auto materials_json = meta_json->doc["embedded_materials"].get_array();

  auto materials = std::vector<Material>();
  for (auto material_json : materials_json) {
    auto material_uuid_json = material_json["uuid"].get_string().value_unsafe();
    auto material_uuid = UUID::from_string(material_uuid_json);
    if (!material_uuid.has_value()) {
      OX_LOG_ERROR("Failed to import model {}! A material with corrupt UUID.", asset_path);
      return false;
    }

    this->register_asset(material_uuid.value(), AssetType::Material, asset_path);
    model->materials.emplace_back(material_uuid.value());

    auto& material = materials.emplace_back();
    read_material_data(&material, material_json.value_unsafe());
  }

  struct GLTFCallbacks {
    Mesh* model = nullptr;

    std::vector<glm::vec3> vertex_positions = {};
    std::vector<glm::vec3> vertex_normals = {};
    std::vector<glm::vec2> vertex_texcoords = {};
    std::vector<Mesh::Index> indices = {};
  };
  auto on_new_primitive = [](void* user_data,
                             u32 mesh_index,
                             u32 material_index,
                             u32 vertex_offset,
                             u32 vertex_count,
                             u32 index_offset,
                             u32 index_count) {
    auto* asset_man = App::get_asset_manager();
    auto* info = static_cast<GLTFCallbacks*>(user_data);
    if (info->model->meshes.size() <= mesh_index) {
      info->model->meshes.resize(mesh_index + 1);
    }

    auto& gltf_mesh = info->model->meshes[mesh_index];
    auto primitive_index = info->model->primitives.size();
    auto& primitive = info->model->primitives.emplace_back();
    auto* material_asset = asset_man->get_asset(info->model->materials[material_index]);
    auto global_material_index = SlotMap_decode_id(material_asset->material_id).index;

    info->model->gpu_meshes.emplace_back();
    info->model->gpu_mesh_buffers.emplace_back();

    info->vertex_positions.resize(info->vertex_positions.size() + vertex_count);
    info->vertex_normals.resize(info->vertex_normals.size() + vertex_count);
    info->vertex_texcoords.resize(info->vertex_texcoords.size() + vertex_count);
    info->indices.resize(info->indices.size() + index_count);

    gltf_mesh.primitive_indices.push_back(static_cast<u32>(primitive_index));
    primitive.material_index = global_material_index;
    primitive.vertex_offset = vertex_offset;
    primitive.vertex_count = vertex_count;
    primitive.index_offset = index_offset;
    primitive.index_count = index_count;
  };
  auto on_access_index = [](void* user_data, u32, u64 offset, u32 index) {
    auto* info = static_cast<GLTFCallbacks*>(user_data);
    info->indices[offset] = index;
  };
  auto on_access_position = [](void* user_data, u32, u64 offset, glm::vec3 position) {
    auto* info = static_cast<GLTFCallbacks*>(user_data);
    info->vertex_positions[offset] = position;
  };
  auto on_access_normal = [](void* user_data, u32, u64 offset, glm::vec3 normal) {
    auto* info = static_cast<GLTFCallbacks*>(user_data);
    info->vertex_normals[offset] = normal;
  };
  auto on_access_texcoord = [](void* user_data, u32, u64 offset, glm::vec2 texcoord) {
    auto* info = static_cast<GLTFCallbacks*>(user_data);
    info->vertex_texcoords[offset] = texcoord;
  };

  auto on_materials_load = [model, materials, &texture_info_map](std::vector<GLTFMaterialInfo>& gltf_materials,
                                                                 std::vector<GLTFTextureInfo>& textures,
                                                                 std::vector<GLTFImageInfo>& images) {
    auto load_texture_bytes = [&textures, &images](u32 texture_index, TextureLoadInfo& inf) {
      if (auto& image_index = textures[texture_index].image_index; image_index.has_value()) {
        auto& image = images[image_index.value()];

        switch (image.file_type) {
          case AssetFileType::KTX2: inf.mime = TextureLoadInfo::MimeType::KTX; break;
          default                 : inf.mime = TextureLoadInfo::MimeType::Generic; break;
        }

        std::visit(ox::match{
                       [&](const ::fs::path& p) {
                         auto extension = p.extension();
                         if (extension == ".ktx" || extension == ".ktx2") {
                           inf.mime = TextureLoadInfo::MimeType::KTX;
                         }
                       }, // noop
                       [&](const std::vector<u8>& data) { inf.bytes = data; },
                   },
                   image.image_data);
      }
    };

    auto* app = App::get();
    auto* asset_man = app->get_asset_manager();

    for (const auto& [material_uuid, material, gltf_material] :
         std::views::zip(model->materials, materials, gltf_materials)) {
      if (auto texture_index = gltf_material.albedo_texture_index; texture_index.has_value()) {
        auto& info = texture_info_map[material.albedo_texture];
        load_texture_bytes(texture_index.value(), info);
      }

      if (auto texture_index = gltf_material.normal_texture_index; texture_index.has_value()) {
        auto& info = texture_info_map[material.normal_texture];
        load_texture_bytes(texture_index.value(), info);
      }

      if (auto texture_index = gltf_material.emissive_texture_index; texture_index.has_value()) {
        auto& info = texture_info_map[material.emissive_texture];
        load_texture_bytes(texture_index.value(), info);
      }

      if (auto texture_index = gltf_material.metallic_roughness_texture_index; texture_index.has_value()) {
        auto& info = texture_info_map[material.metallic_roughness_texture];
        load_texture_bytes(texture_index.value(), info);
      }

      if (auto texture_index = gltf_material.occlusion_texture_index; texture_index.has_value()) {
        auto& info = texture_info_map[material.occlusion_texture];
        load_texture_bytes(texture_index.value(), info);
      }

      asset_man->load_material(material_uuid, material, texture_info_map);
    }
  };

  GLTFCallbacks gltf_callbacks = {.model = model};
  auto gltf_model = GLTFMeshInfo::parse(asset_path,
                                        {.user_data = &gltf_callbacks,
                                         .on_new_primitive = on_new_primitive,
                                         .on_access_index = on_access_index,
                                         .on_access_position = on_access_position,
                                         .on_access_normal = on_access_normal,
                                         .on_access_texcoord = on_access_texcoord,
                                         .on_materials_load = on_materials_load});
  if (!gltf_model.has_value()) {
    OX_LOG_ERROR("Failed to parse Model '{}'!", asset_path);
    return false;
  }

  //  ── SCENE HIERARCHY ─────────────────────────────────────────────────
  for (const auto& node : gltf_model->nodes) {
    model->nodes.push_back({.name = node.name,
                            .child_indices = node.children,
                            .mesh_index = node.mesh_index,
                            .translation = node.translation,
                            .rotation = node.rotation,
                            .scale = node.scale});
  }

  model->default_scene_index = gltf_model->defualt_scene_index.value_or(0_sz);
  for (const auto& scene : gltf_model->scenes) {
    model->scenes.push_back({.name = scene.name, .node_indices = scene.node_indices});
  }

  auto& context = app->get_vkcontext();
  //  ── MESH PROCESSING ─────────────────────────────────────────────────
  auto model_indices = std::move(gltf_callbacks.indices);
  auto model_vertices = std::move(gltf_callbacks.vertex_positions);
  auto model_normals = std::move(gltf_callbacks.vertex_normals);
  auto model_texcoords = std::move(gltf_callbacks.vertex_texcoords);

  // for each model (aka gltf scene):
  // - for each mesh:
  // - - for each primitive:
  // - - - for each lod:
  // - - - - generate lods
  // - - - - optimize and remap geometry
  // - - - - calculate meshlets and bounds
  //
  for (const auto& mesh : model->meshes) {
    for (auto primitive_index : mesh.primitive_indices) {
      auto& primitive = model->primitives[primitive_index];
      auto& gpu_mesh = model->gpu_meshes[primitive_index];
      auto& gpu_mesh_buffer = model->gpu_mesh_buffers[primitive_index];

      //  ── Geometry remapping ──────────────────────────────────────────────
      auto primitive_indices = std::span(model_indices.data() + primitive.index_offset, primitive.index_count);
      auto primitive_vertices = std::span(model_vertices.data() + primitive.vertex_offset, primitive.vertex_count);
      auto primitive_normals = std::span(model_normals.data() + primitive.vertex_offset, primitive.vertex_count);
      auto primitive_texcoords = std::span(model_texcoords.data() + primitive.vertex_offset, primitive.vertex_count);

      auto remapped_vertices = std::vector<u32>(primitive_vertices.size());
      auto vertex_count = meshopt_optimizeVertexFetchRemap(
          remapped_vertices.data(), primitive_indices.data(), primitive_indices.size(), primitive.vertex_count);

      auto mesh_vertices = std::vector<glm::vec3>(vertex_count);
      meshopt_remapVertexBuffer(mesh_vertices.data(),
                                primitive_vertices.data(),
                                primitive_vertices.size(),
                                sizeof(glm::vec3),
                                remapped_vertices.data());

      auto mesh_normals = std::vector<glm::vec3>(vertex_count);
      meshopt_remapVertexBuffer(mesh_normals.data(),
                                primitive_normals.data(),
                                primitive_normals.size(),
                                sizeof(glm::vec3),
                                remapped_vertices.data());

      auto mesh_texcoords = std::vector<glm::vec2>();
      if (!primitive_texcoords.empty()) {
        mesh_texcoords.resize(vertex_count);
        meshopt_remapVertexBuffer(mesh_texcoords.data(),
                                  primitive_texcoords.data(),
                                  primitive_texcoords.size(),
                                  sizeof(glm::vec2),
                                  remapped_vertices.data());
      }

      auto mesh_indices = std::vector<u32>(primitive.index_count);
      meshopt_remapIndexBuffer(
          mesh_indices.data(), primitive_indices.data(), primitive_indices.size(), remapped_vertices.data());

      //  ── LOD generation ──────────────────────────────────────────────────

      const auto mesh_upload_size = 0                               //
                                    + ox::size_bytes(mesh_vertices) //
                                    + ox::size_bytes(mesh_normals)  //
                                    + ox::size_bytes(mesh_texcoords);
      auto upload_size = mesh_upload_size;

      std::pair<vuk::Value<vuk::Buffer>, u64> lod_cpu_buffers[GPU::Mesh::MAX_LODS] = {};
      auto last_lod_indices = std::vector<u32>();
      for (auto lod_index = 0_sz; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
        ZoneNamedN(z, "GPU Meshlet Generation", true);

        auto& cur_lod = gpu_mesh.lods[lod_index];

        auto simplified_indices = std::vector<u32>();
        if (lod_index == 0) {
          simplified_indices = std::vector<u32>(mesh_indices.begin(), mesh_indices.end());
        } else {
          const auto& last_lod = gpu_mesh.lods[lod_index - 1];
          auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
          simplified_indices.resize(last_lod_indices.size(), 0_u32);
          constexpr auto TARGET_ERROR = std::numeric_limits<f32>::max();
          constexpr f32 NORMAL_WEIGHTS[] = {1.0f, 1.0f, 1.0f};

          auto result_error = 0.0f;
          auto result_index_count = meshopt_simplifyWithAttributes( //
              simplified_indices.data(),
              last_lod_indices.data(),
              last_lod_indices.size(),
              reinterpret_cast<const f32*>(mesh_vertices.data()),
              mesh_vertices.size(),
              sizeof(glm::vec3),
              reinterpret_cast<const f32*>(mesh_normals.data()),
              sizeof(glm::vec3),
              NORMAL_WEIGHTS,
              ox::count_of(NORMAL_WEIGHTS),
              nullptr,
              lod_index_count,
              TARGET_ERROR,
              meshopt_SimplifyLockBorder,
              &result_error);

          cur_lod.error = last_lod.error + result_error;
          if (result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 ||
              result_index_count < 6) {
            // Error bound
            break;
          }

          simplified_indices.resize(result_index_count);
        }

        gpu_mesh.lod_count += 1;
        last_lod_indices = simplified_indices;

        meshopt_optimizeVertexCache(
            simplified_indices.data(), simplified_indices.data(), simplified_indices.size(), vertex_count);

        // Worst case count
        auto max_meshlet_count = meshopt_buildMeshletsBound(
            simplified_indices.size(), Mesh::MAX_MESHLET_INDICES, Mesh::MAX_MESHLET_PRIMITIVES);
        auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
        auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * Mesh::MAX_MESHLET_INDICES);
        auto local_triangle_indices = std::vector<u8>(max_meshlet_count * Mesh::MAX_MESHLET_PRIMITIVES * 3);

        auto meshlet_count = meshopt_buildMeshlets( //
            raw_meshlets.data(),
            indirect_vertex_indices.data(),
            local_triangle_indices.data(),
            simplified_indices.data(),
            simplified_indices.size(),
            reinterpret_cast<const f32*>(mesh_vertices.data()),
            mesh_vertices.size(),
            sizeof(glm::vec3),
            Mesh::MAX_MESHLET_INDICES,
            Mesh::MAX_MESHLET_PRIMITIVES,
            0.0);

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
            const auto& tri_pos =
                mesh_vertices[indirect_vertex_indices[raw_meshlet.vertex_offset +
                                                      local_triangle_indices[raw_meshlet.triangle_offset + i]]];
            meshlet_bb_min = glm::min(meshlet_bb_min, tri_pos);
            meshlet_bb_max = glm::max(meshlet_bb_max, tri_pos);
          }

          // Sphere and Cone computation
          auto sphere_bounds = meshopt_computeMeshletBounds( //
              &indirect_vertex_indices[raw_meshlet.vertex_offset],
              &local_triangle_indices[raw_meshlet.triangle_offset],
              raw_meshlet.triangle_count,
              reinterpret_cast<f32*>(mesh_vertices.data()),
              vertex_count,
              sizeof(glm::vec3));

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
        auto cpu_lod_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, lod_upload_size);
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
            cpu_lod_ptr + upload_offset, indirect_vertex_indices.data(), ox::size_bytes(indirect_vertex_indices));
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
      gpu_mesh_buffer = context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, upload_size);

      // Mesh first
      auto cpu_mesh_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, mesh_upload_size);
      auto cpu_mesh_ptr = reinterpret_cast<u8*>(cpu_mesh_buffer->mapped_ptr);

      auto gpu_mesh_bda = gpu_mesh_buffer->device_address;
      gpu_mesh.vertex_positions = gpu_mesh_bda + mesh_upload_offset;
      std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_vertices.data(), ox::size_bytes(mesh_vertices));
      mesh_upload_offset += ox::size_bytes(mesh_vertices);

      gpu_mesh.vertex_normals = gpu_mesh_bda + mesh_upload_offset;
      std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_normals.data(), ox::size_bytes(mesh_normals));
      mesh_upload_offset += ox::size_bytes(mesh_normals);

      if (!mesh_texcoords.empty()) {
        gpu_mesh.texture_coords = gpu_mesh_bda + mesh_upload_offset;
        std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_texcoords.data(), ox::size_bytes(mesh_texcoords));
        mesh_upload_offset += ox::size_bytes(mesh_texcoords);
      }

      auto gpu_mesh_subrange = vuk::discard_buf("mesh", gpu_mesh_buffer->subrange(0, mesh_upload_size));
      gpu_mesh_subrange = context.upload_staging(std::move(cpu_mesh_buffer), std::move(gpu_mesh_subrange));
      context.wait_on(std::move(gpu_mesh_subrange));

      for (auto lod_index = 0_sz; lod_index < gpu_mesh.lod_count; lod_index++) {
        auto&& [lod_cpu_buffer, lod_upload_size] = lod_cpu_buffers[lod_index];
        auto& lod = gpu_mesh.lods[lod_index];

        lod.indices += gpu_mesh_bda + mesh_upload_offset;
        lod.meshlets += gpu_mesh_bda + mesh_upload_offset;
        lod.meshlet_bounds += gpu_mesh_bda + mesh_upload_offset;
        lod.local_triangle_indices += gpu_mesh_bda + mesh_upload_offset;
        lod.indirect_vertex_indices += gpu_mesh_bda + mesh_upload_offset;

        auto gpu_lod_subrange = vuk::discard_buf("mesh lod subrange",
                                                 gpu_mesh_buffer->subrange(mesh_upload_offset, lod_upload_size));
        gpu_lod_subrange = context.upload_staging(std::move(lod_cpu_buffer), std::move(gpu_lod_subrange));
        context.wait_on(std::move(gpu_lod_subrange));

        mesh_upload_offset += lod_upload_size;
      }
    }
  }

  return true;
}

auto AssetManager::unload_mesh(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  auto* model = this->get_mesh(asset->mesh_id);
  for (auto& v : model->materials) {
    this->unload_material(v);
  }

  mesh_map.destroy_slot(asset->mesh_id);
  asset->mesh_id = MeshID::Invalid;

  OX_LOG_TRACE("Unloaded mesh {}", uuid.str());

  return true;
}

auto AssetManager::load_texture(const UUID& uuid, const TextureLoadInfo& info) -> bool {
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
    texture.create(asset->path, info);

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

auto AssetManager::load_material(const UUID& uuid,
                                 const Material& material_info,
                                 option<ankerl::unordered_dense::map<UUID, TextureLoadInfo>> texture_info_map) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);

  // Materials don't explicitly load any resources, they need to increase child resources refs.

  if (!asset->is_loaded()) {
    asset->material_id = material_map.create_slot(const_cast<Material&&>(material_info));
  }

  struct LoadInfo {
    UUID texture_uuid = {};
    MaterialID material_id = {};
    TextureLoadInfo texture_load_info = {};
  };
  std::vector<LoadInfo> load_infos = {};

  auto* material = material_map.slot(asset->material_id);

  this->set_material_dirty(asset->material_id);

  const auto get_info = [&texture_info_map](UUID& texture, vuk::Format format) -> TextureLoadInfo {
    TextureLoadInfo info = {.format = format};
    if (texture_info_map.has_value()) {
      auto& map = texture_info_map.value();
      if (map.contains(texture)) {
        info.bytes = map[texture].bytes;
        info.mime = map[texture].mime;
      }
    }
    return info;
  };

  auto* job_man = App::get_job_manager();

  if (material->albedo_texture) {
    auto info = get_info(material->albedo_texture, vuk::Format::eR8G8B8A8Srgb);
    load_infos.emplace_back(LoadInfo{material->albedo_texture, asset->material_id, info});
  }

  if (material->normal_texture) {
    auto info = get_info(material->normal_texture, vuk::Format::eR8G8B8A8Unorm);
    load_infos.emplace_back(LoadInfo{material->normal_texture, asset->material_id, info});
  }

  if (material->emissive_texture) {
    auto info = get_info(material->emissive_texture, vuk::Format::eR8G8B8A8Srgb);
    load_infos.emplace_back(LoadInfo{material->emissive_texture, asset->material_id, info});
  }

  if (material->metallic_roughness_texture) {
    auto info = get_info(material->metallic_roughness_texture, vuk::Format::eR8G8B8A8Unorm);
    load_infos.emplace_back(LoadInfo{material->metallic_roughness_texture, asset->material_id, info});
  }

  if (material->occlusion_texture) {
    auto info = get_info(material->occlusion_texture, vuk::Format::eR8G8B8A8Unorm);
    load_infos.emplace_back(LoadInfo{material->occlusion_texture, asset->material_id, info});
  }

  job_man->push_job_name(fmt::format("Material job: {}", asset->uuid.str()));
  job_man->for_each_async(
      load_infos,
      [](LoadInfo& info, usize index) {
        auto* asset_man = App::get_asset_manager();
        asset_man->load_texture(info.texture_uuid, info.texture_load_info);
      },
      [material_id = asset->material_id]() {
        auto* asset_man = App::get_asset_manager();
        asset_man->set_material_dirty(material_id);
      });
  job_man->pop_job_name();

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

auto AssetManager::get_mesh(const UUID& uuid) -> Mesh* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Mesh);
  if (asset->type != AssetType::Mesh || asset->mesh_id == MeshID::Invalid) {
    return nullptr;
  }

  return mesh_map.slot(asset->mesh_id);
}

auto AssetManager::get_mesh(const MeshID mesh_id) -> Mesh* {
  ZoneScoped;

  if (mesh_id == MeshID::Invalid) {
    return nullptr;
  }

  return mesh_map.slot(mesh_id);
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
