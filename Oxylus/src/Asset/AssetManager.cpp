#include "Asset/AssetManager.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetFile.hpp"
#include "OS/File.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetManager::read_asset_file(const std::filesystem::path& path) -> std::vector<AssetFileEntryInfo> {
  auto file = File(path, FileAccess::Read);
  if (!file || file.size < sizeof(AssetFileHeader)) {
    OX_LOG_ERROR("Tried to import an invalid asset file. {}", path);
    return {};
  }

  auto header = AssetFileHeader{};
  file.read(&header, sizeof(AssetManager));
  if (header.magic != AssetFileHeader::MAGIC) {
    OX_LOG_ERROR("Tried to import an asset file that does not match our file header. {}", path);
    return {};
  }

#define READ_OR_FAIL(ptr, size, name)                                                                                  \
  if (file.read(ptr, size) < size) {                                                                                   \
    OX_LOG_ERROR("Could not read entry index {}, {} has a corrupt " name " section.", file_entry_index, path);         \
    return {};                                                                                                         \
  }

  auto file_entries = std::vector<AssetFileEntryInfo>{};
  for (auto file_entry_index = 0_sz; file_entry_index < header.file_entry_count; file_entry_index++) {
    auto file_entry = AssetFileEntryInfo{};
    auto uuid_bytes = std::array<u8, 16>{};
    READ_OR_FAIL(uuid_bytes.data(), ox::size_bytes(uuid_bytes), "UUID");
    file_entry.uuid = UUID::from_bytes(std::span(uuid_bytes)).value_or(UUID(nullptr));
    READ_OR_FAIL(&file_entry.data_size, sizeof(u32), "data_size");
    READ_OR_FAIL(&file_entry.data_offset, sizeof(u32), "data_offset");
    READ_OR_FAIL(&file_entry.type, sizeof(AssetType), "type");
    switch (file_entry.type) {
      case AssetType::Model: {
        READ_OR_FAIL(&file_entry.entry.model.nodes, sizeof(AssetDataView<>), "model entry nodes");
        READ_OR_FAIL(&file_entry.entry.model.meshes, sizeof(AssetDataView<>), "model entry meshes");
      } break;
      case AssetType::Shader: {
        READ_OR_FAIL(&file_entry.entry.shader.entry_points, sizeof(AssetDataView<>), "shader entry points");
      } break;
      default: {
        OX_LOG_ERROR("Unhandled meta type {} for {}", std::to_underlying(file_entry.type), path);
        return {};
      }
    }

#undef READ_OR_FAIL

    file_entries.push_back(file_entry);
  }

  return file_entries;
}

auto AssetManager::init(this AssetManager&) -> std::expected<void, std::string> { return {}; }

auto AssetManager::deinit(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : self.asset_registry) {
    // leak check
    if (asset.is_loaded() && asset.ref_count != 0) {
      const auto& extended_asset_it = self.extended_registry.find(uuid);
      const auto& extended_asset = extended_asset_it->second;
      OX_LOG_WARN(
        "A {} asset ({}, {}) with refcount of {} is still alive!",
        AssetMetadata::to_asset_type_sv(asset.type),
        uuid.str(),
        extended_asset.path,
        asset.ref_count
      );
    }
  }

  self.asset_registry.clear();
  self.dirty_materials.clear();
  self.model_map.reset();
  self.texture_map.reset();
  self.material_map.reset();
  self.scene_map.reset();
  self.audio_map.reset();
  self.script_map.reset();

  return {};
}

auto AssetManager::get_registry(this AssetManager& self) -> const AssetRegistry& { return self.asset_registry; }

auto AssetManager::create(this AssetManager& self, AssetType type, const ExtendedAsset& extended_asset) -> UUID {
  ZoneScoped;

  auto uuid = UUID::generate_random();

  auto write_lock = std::unique_lock(self.registry_mutex);
  auto [asset_it, inserted] = self.asset_registry.try_emplace(uuid);
  if (!inserted) {
    OX_LOG_ERROR("Can't create a duplicate asset {}!", uuid.str());
    return UUID(nullptr);
  }

  self.extended_registry.emplace(uuid, extended_asset);

  auto& asset = asset_it->second;
  asset.type = type;

  return uuid;
}

auto AssetManager::import(this AssetManager& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto file_entries = read_asset_file(path);
  for (const auto& file_entry : file_entries) {
    auto write_lock = std::unique_lock(self.registry_mutex);
    auto [asset_it, inserted] = self.asset_registry.try_emplace(file_entry.uuid);
    if (!inserted) {
      OX_LOG_ERROR("Can't create a duplicate asset {}, {}!", file_entry.uuid.str(), path);
      // return UUID(nullptr);
      continue; // it's probably better if we just continue
    }

    auto& asset = asset_it->second;
    asset.type = file_entry.type;

    self.extended_registry.emplace(
      file_entry.uuid,
      ExtendedAsset{
        .path = path,
        .data_size = file_entry.data_size,
        .data_offset = file_entry.data_offset,
        .entry = file_entry.entry
      }
    );
  }

  OX_LOG_TRACE("Imported {} asset(s) from {}", file_entries.size(), path);

  return true;
}

auto AssetManager::delete_asset(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (asset->ref_count > 0) {
    OX_LOG_WARN("Deleting alive asset {} with {} references!", uuid.str(), asset->ref_count);
  }

  if (asset->is_loaded()) {
    asset->ref_count = ox::min(asset->ref_count, 1_u64);
    self.unload_asset(uuid);

    {
      asset.reset();
      auto write_lock = std::unique_lock(self.registry_mutex);
      self.asset_registry.erase(uuid);
    }
  }

  OX_LOG_TRACE("Deleted asset {}.", uuid.str());
}

// auto AssetManager::import_asset(this AssetManager& self, const std::filesystem::path& path) -> UUID {
//   ZoneScoped;
//   memory::ScopedStack stack;
//
//   if (!std::filesystem::exists(path)) {
//     OX_LOG_ERROR("Trying to import an asset '{}' that doesn't exist.", path);
//     return UUID(nullptr);
//   }
//
//   auto asset_type = [&]() -> AssetType {
//     switch (self.to_file_format(path)) {
//       case FileFormat::Meta: {
//         return AssetType::Meta;
//       }
//       case FileFormat::GLB:
//       case FileFormat::GLTF: {
//         return AssetType::Model;
//       }
//       case FileFormat::PNG:
//       case FileFormat::JPEG:
//       case FileFormat::DDS:
//       case FileFormat::KTX2: {
//         return AssetType::Texture;
//       }
//       case ox::FileFormat::Lua: {
//         return AssetType::Script;
//       }
//       default: {
//         return AssetType::None;
//       }
//     }
//   }();
//
//   if (asset_type == AssetType::Meta) {
//     return self.register_asset(path);
//   }
//
//   // Check for meta file before creating new asset
//   auto meta_path = stack.format("{}.oxasset", path);
//   if (std::filesystem::exists(meta_path)) {
//     return self.register_asset(meta_path);
//   }
//
//   auto uuid = self.create_asset(asset_type, path);
//   if (!uuid) {
//     return UUID(nullptr);
//   }
//
//   auto variant = [&] -> option<AssetVariant> {
//     switch (asset_type) {
//       case AssetType::Model: {
//         auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
//         auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
//         if (gltf_type == fastgltf::GltfType::Invalid) {
//           OX_LOG_ERROR("GLTF model type is invalid!");
//           return nullopt;
//         }
//
//         auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
//         auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_gltf_options());
//         if (!gltf_result) {
//           OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(gltf_result.error()));
//           return nullopt;
//         }
//
//         // We (usually) import assets once, so it is okay to generate random
//         // UUIDs here, their array indices will map to their contents in GLTF.
//
//         auto gltf_asset = std::move(gltf_result.get());
//         auto embedded_textures = std::vector<UUID>(gltf_asset.textures.size());
//         for (auto& embedded_texture : embedded_textures) {
//           embedded_texture = UUID::generate_random();
//         }
//
//         auto materials = std::vector<UUID>(gltf_asset.materials.size());
//         for (auto& material : materials) {
//           material = UUID::generate_random();
//         }
//
//         return ModelMetadata{
//           .embedded_texture_uuids = std::move(embedded_textures),
//           .material_uuids = std::move(materials),
//         };
//       }
//       default: {
//         return nullopt;
//       }
//     }
//   }();
//
//   if (!variant.has_value()) {
//     return UUID(nullptr);
//   }
//
//   auto asset_metadata = AssetMetadata{
//     .uuid = uuid,
//     .type = asset_type,
//     .variant = variant.value(),
//   };
//   asset_metadata.to_file(meta_path);
//
//   return uuid;
// }

auto AssetManager::load_asset(this AssetManager& self, const UUID& uuid) -> bool {
  auto asset = self.get_asset(uuid);

  return false;
}

auto AssetManager::unload_asset(this AssetManager& self, const UUID& uuid) -> bool {
  auto asset = self.get_asset(uuid);

  return false;
}

// auto AssetManager::load_model(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   memory::ScopedStack stack;
//
//   auto asset = this->get_asset(uuid);
//   if (asset->is_loaded()) {
//     // Model is collection of multiple assets and all child
//     // assets must be alive to safely process meshes.
//     // Don't acquire child refs.
//     asset->acquire_ref();
//
//     return true;
//   }
//
//   asset->model_id = model_map.create_slot();
//   auto* model = model_map.slot(asset->model_id);
//
//   auto meta_path = std::filesystem::path(asset->path.string() + ".oxasset");
//   auto meta_json = read_meta_file(meta_path);
//   if (!meta_json) {
//     return false;
//   }
//
//   auto asset_path = asset->path;
//   asset->acquire_ref();
//   asset.reset();
//
//   // Load embedded textures
//   ankerl::unordered_dense::map<UUID, TextureLoadInfo> texture_info_map = {};
//
//   auto embedded_textures = std::vector<UUID>();
//   auto embedded_texture_uuids_json = meta_json->doc["embedded_textures"].get_array();
//   for (auto embedded_texture_uuid_json : embedded_texture_uuids_json) {
//     auto embedded_texture_uuid_str = embedded_texture_uuid_json.get_string().value_unsafe();
//
//     auto embedded_texture_uuid = UUID::from_string(embedded_texture_uuid_str);
//     if (!embedded_texture_uuid.has_value()) {
//       OX_LOG_ERROR("Failed to import model {}! An embedded texture with corrupt UUID.", asset_path);
//       return false;
//     }
//
//     embedded_textures.push_back(embedded_texture_uuid.value());
//     this->register_asset(embedded_texture_uuid.value(), AssetType::Texture, {});
//
//     texture_info_map.emplace(embedded_texture_uuid.value(), TextureLoadInfo{});
//   }
//
//   // Load registered UUIDs.
//   auto materials_json = meta_json->doc["embedded_materials"].get_array();
//
//   auto materials = std::vector<Material>();
//   for (auto material_json : materials_json) {
//     auto material_uuid_json = material_json["uuid"].get_string().value_unsafe();
//     auto material_uuid = UUID::from_string(material_uuid_json);
//     if (!material_uuid.has_value()) {
//       OX_LOG_ERROR("Failed to import model {}! A material with corrupt UUID.", asset_path);
//       return false;
//     }
//
//     this->register_asset(material_uuid.value(), AssetType::Material, asset_path);
//     model->materials.emplace_back(material_uuid.value());
//
//     auto& material = materials.emplace_back();
//     read_material_data(&material, material_json.value_unsafe());
//   }
//
//   struct GLTFCallbacks {
//     Model* model = nullptr;
//
//     std::vector<glm::vec3> vertex_positions = {};
//     std::vector<glm::vec3> vertex_normals = {};
//     std::vector<glm::vec2> vertex_texcoords = {};
//     std::vector<Model::Index> indices = {};
//   };
//   auto on_new_primitive = [](
//                             void* user_data,
//                             u32 mesh_index,
//                             u32 material_index,
//                             u32 vertex_offset,
//                             u32 vertex_count,
//                             u32 index_offset,
//                             u32 index_count
//                           ) {
//     auto& asset_man = App::mod<AssetManager>();
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//     if (info->model->meshes.size() <= mesh_index) {
//       info->model->meshes.resize(mesh_index + 1);
//     }
//
//     auto& gltf_mesh = info->model->meshes[mesh_index];
//     auto primitive_index = info->model->primitives.size();
//     auto& primitive = info->model->primitives.emplace_back();
//     auto material_asset = asset_man.get_asset(info->model->materials[material_index]);
//     auto global_material_index = SlotMap_decode_id(material_asset->material_id).index;
//
//     info->model->gpu_meshes.emplace_back();
//     info->model->gpu_mesh_buffers.emplace_back();
//
//     info->vertex_positions.resize(info->vertex_positions.size() + vertex_count);
//     info->vertex_normals.resize(info->vertex_normals.size() + vertex_count);
//     info->vertex_texcoords.resize(info->vertex_texcoords.size() + vertex_count);
//     info->indices.resize(info->indices.size() + index_count);
//
//     gltf_mesh.primitive_indices.push_back(static_cast<u32>(primitive_index));
//     primitive.material_index = global_material_index;
//     primitive.vertex_offset = vertex_offset;
//     primitive.vertex_count = vertex_count;
//     primitive.index_offset = index_offset;
//     primitive.index_count = index_count;
//   };
//   auto on_access_index = [](void* user_data, u32, u64 offset, u32 index) {
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//     info->indices[offset] = index;
//   };
//   auto on_access_position = [](void* user_data, u32, u64 offset, glm::vec3 position) {
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//     info->vertex_positions[offset] = position;
//   };
//   auto on_access_normal = [](void* user_data, u32, u64 offset, glm::vec3 normal) {
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//     info->vertex_normals[offset] = normal;
//   };
//   auto on_access_texcoord = [](void* user_data, u32, u64 offset, glm::vec2 texcoord) {
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//     info->vertex_texcoords[offset] = texcoord;
//   };
//
//   auto on_materials_load = [model, materials, &texture_info_map](
//                              std::vector<GLTFMaterialInfo>& gltf_materials,
//                              std::vector<GLTFTextureInfo>& textures,
//                              std::vector<GLTFImageInfo>& images,
//                              std::vector<GLTFSamplerInfo>& samplers
//                            ) {
//     auto load_texture_bytes = [&textures, &images, &samplers](u32 texture_index, TextureLoadInfo& inf) {
//       auto& texture = textures[texture_index];
//       if (auto& image_index = texture.image_index; image_index.has_value()) {
//         auto& image = images[image_index.value()];
//
//         switch (image.file_type) {
//           case FileFormat::KTX2: inf.mime = TextureLoadInfo::MimeType::KTX; break;
//           case FileFormat::DDS : inf.mime = TextureLoadInfo::MimeType::DDS; break;
//           default              : inf.mime = TextureLoadInfo::MimeType::Generic; break;
//         }
//
//         if (texture.sampler_index.has_value()) {
//           auto& sampler = samplers[texture.sampler_index.value()];
//           inf.sampler_info.minFilter = sampler.min_filter;
//           inf.sampler_info.magFilter = sampler.mag_filter;
//           inf.sampler_info.addressModeU = sampler.address_u;
//           inf.sampler_info.addressModeV = sampler.address_v;
//         }
//
//         std::visit(
//           ox::match{
//             [&](const std::filesystem::path& p) {
//               auto extension = p.extension();
//
//               memory::ScopedStack s;
//               auto extension_upped = s.to_upper(extension.string());
//
//               if (extension_upped == ".KTX" || extension_upped == ".KTX2") {
//                 inf.mime = TextureLoadInfo::MimeType::KTX;
//               } else if (extension_upped == ".DDS") {
//                 inf.mime = TextureLoadInfo::MimeType::DDS;
//               }
//             },
//             [&](const std::vector<u8>& data) { inf.bytes = data; },
//           },
//           image.image_data
//         );
//       }
//     };
//
//     auto* app = App::get();
//     auto& asset_man = app->mod<AssetManager>();
//
//     for (const auto& [material_uuid, material, gltf_material] :
//          std::views::zip(model->materials, materials, gltf_materials)) {
//       if (auto texture_index = gltf_material.albedo_texture_index; texture_index.has_value()) {
//         auto& info = texture_info_map[material.albedo_texture];
//         load_texture_bytes(texture_index.value(), info);
//       }
//
//       if (auto texture_index = gltf_material.normal_texture_index; texture_index.has_value()) {
//         auto& info = texture_info_map[material.normal_texture];
//         load_texture_bytes(texture_index.value(), info);
//       }
//
//       if (auto texture_index = gltf_material.emissive_texture_index; texture_index.has_value()) {
//         auto& info = texture_info_map[material.emissive_texture];
//         load_texture_bytes(texture_index.value(), info);
//       }
//
//       if (auto texture_index = gltf_material.metallic_roughness_texture_index; texture_index.has_value()) {
//         auto& info = texture_info_map[material.metallic_roughness_texture];
//         load_texture_bytes(texture_index.value(), info);
//       }
//
//       if (auto texture_index = gltf_material.occlusion_texture_index; texture_index.has_value()) {
//         auto& info = texture_info_map[material.occlusion_texture];
//         load_texture_bytes(texture_index.value(), info);
//       }
//
//       asset_man.load_material(material_uuid, material, texture_info_map);
//     }
//   };
//
//   auto on_new_light = [](void* user_data, usize light_index, const GLTFLightInfo& light) {
//     auto* info = static_cast<GLTFCallbacks*>(user_data);
//
//     info->model->lights.emplace_back(
//       Model::Light{
//         .name = light.name,
//         .type = static_cast<Model::LightType>(light.type),
//         .color = light.color,
//         .intensity = light.intensity,
//         .range = light.range,
//         .inner_cone_angle = light.inner_cone_angle,
//         .outer_cone_angle = light.outer_cone_angle,
//       }
//     );
//   };
//
//   GLTFCallbacks gltf_callbacks = {.model = model};
//   auto gltf_model = GLTFMeshInfo::parse(
//     asset_path,
//     {.user_data = &gltf_callbacks,
//      .on_new_primitive = on_new_primitive,
//      .on_new_light = on_new_light,
//      .on_materials_load = on_materials_load,
//      .on_access_index = on_access_index,
//      .on_access_position = on_access_position,
//      .on_access_normal = on_access_normal,
//      .on_access_texcoord = on_access_texcoord}
//   );
//   if (!gltf_model.has_value()) {
//     OX_LOG_ERROR("Failed to parse Model '{}'!", asset_path);
//     return false;
//   }
//
//   //  ── SCENE HIERARCHY ─────────────────────────────────────────────────
//   for (const auto& node : gltf_model->nodes) {
//     model->nodes.push_back(
//       {.name = node.name,
//        .child_indices = node.children,
//        .mesh_index = node.mesh_index,
//        .light_index = node.light_index,
//        .translation = node.translation,
//        .rotation = node.rotation,
//        .scale = node.scale}
//     );
//   }
//
//   model->default_scene_index = gltf_model->defualt_scene_index.value_or(0_sz);
//   for (const auto& scene : gltf_model->scenes) {
//     model->scenes.push_back({.name = scene.name, .node_indices = scene.node_indices});
//   }
//
//   auto& context = App::get()->get_vkcontext();
//   //  ── MESH PROCESSING ─────────────────────────────────────────────────
//   auto model_indices = std::move(gltf_callbacks.indices);
//   auto model_vertices = std::move(gltf_callbacks.vertex_positions);
//   auto model_normals = std::move(gltf_callbacks.vertex_normals);
//   auto model_texcoords = std::move(gltf_callbacks.vertex_texcoords);
//
//   // for each model (aka gltf scene):
//   // - for each mesh:
//   // - - for each primitive:
//   // - - - for each lod:
//   // - - - - generate lods
//   // - - - - optimize and remap geometry
//   // - - - - calculate meshlets and bounds
//   //
//   for (const auto& mesh : model->meshes) {
//     for (auto primitive_index : mesh.primitive_indices) {
//       auto& primitive = model->primitives[primitive_index];
//       auto& gpu_mesh = model->gpu_meshes[primitive_index];
//       auto& gpu_mesh_buffer = model->gpu_mesh_buffers[primitive_index];
//
//       //  ── Geometry remapping ──────────────────────────────────────────────
//       auto primitive_indices = std::span(model_indices.data() + primitive.index_offset, primitive.index_count);
//       auto primitive_vertices = std::span(model_vertices.data() + primitive.vertex_offset, primitive.vertex_count);
//       auto primitive_normals = std::span(model_normals.data() + primitive.vertex_offset, primitive.vertex_count);
//       auto primitive_texcoords = std::span(model_texcoords.data() + primitive.vertex_offset, primitive.vertex_count);
//
//       auto remapped_vertices = std::vector<u32>(primitive_vertices.size());
//       auto vertex_count = meshopt_optimizeVertexFetchRemap(
//         remapped_vertices.data(),
//         primitive_indices.data(),
//         primitive_indices.size(),
//         primitive.vertex_count
//       );
//
//       auto mesh_vertices = std::vector<glm::vec3>(vertex_count);
//       meshopt_remapVertexBuffer(
//         mesh_vertices.data(),
//         primitive_vertices.data(),
//         primitive_vertices.size(),
//         sizeof(glm::vec3),
//         remapped_vertices.data()
//       );
//
//       auto mesh_normals = std::vector<glm::vec3>(vertex_count);
//       meshopt_remapVertexBuffer(
//         mesh_normals.data(),
//         primitive_normals.data(),
//         primitive_normals.size(),
//         sizeof(glm::vec3),
//         remapped_vertices.data()
//       );
//
//       auto mesh_texcoords = std::vector<glm::vec2>();
//       if (!primitive_texcoords.empty()) {
//         mesh_texcoords.resize(vertex_count);
//         meshopt_remapVertexBuffer(
//           mesh_texcoords.data(),
//           primitive_texcoords.data(),
//           primitive_texcoords.size(),
//           sizeof(glm::vec2),
//           remapped_vertices.data()
//         );
//       }
//
//       auto mesh_indices = std::vector<u32>(primitive.index_count);
//       meshopt_remapIndexBuffer(
//         mesh_indices.data(),
//         primitive_indices.data(),
//         primitive_indices.size(),
//         remapped_vertices.data()
//       );
//
//       //  ── LOD generation ──────────────────────────────────────────────────
//
//       const auto mesh_upload_size = 0                               //
//                                     + ox::size_bytes(mesh_vertices) //
//                                     + ox::size_bytes(mesh_normals)  //
//                                     + ox::size_bytes(mesh_texcoords);
//       auto upload_size = mesh_upload_size;
//
//       std::pair<vuk::Value<vuk::Buffer>, u64> lod_cpu_buffers[GPU::Mesh::MAX_LODS] = {};
//       auto last_lod_indices = std::vector<u32>();
//       for (auto lod_index = 0_sz; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
//         ZoneNamedN(z, "GPU Meshlet Generation", true);
//
//         auto& cur_lod = gpu_mesh.lods[lod_index];
//
//         auto simplified_indices = std::vector<u32>();
//         if (lod_index == 0) {
//           simplified_indices = std::vector<u32>(mesh_indices.begin(), mesh_indices.end());
//         } else {
//           const auto& last_lod = gpu_mesh.lods[lod_index - 1];
//           auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
//           simplified_indices.resize(last_lod_indices.size(), 0_u32);
//           constexpr auto TARGET_ERROR = std::numeric_limits<f32>::max();
//           constexpr f32 NORMAL_WEIGHTS[] = {1.0f, 1.0f, 1.0f};
//
//           auto result_error = 0.0f;
//           auto result_index_count = meshopt_simplifyWithAttributes( //
//               simplified_indices.data(),
//               last_lod_indices.data(),
//               last_lod_indices.size(),
//               reinterpret_cast<const f32*>(mesh_vertices.data()),
//               mesh_vertices.size(),
//               sizeof(glm::vec3),
//               reinterpret_cast<const f32*>(mesh_normals.data()),
//               sizeof(glm::vec3),
//               NORMAL_WEIGHTS,
//               ox::count_of(NORMAL_WEIGHTS),
//               nullptr,
//               lod_index_count,
//               TARGET_ERROR,
//               meshopt_SimplifyLockBorder,
//               &result_error);
//
//           cur_lod.error = last_lod.error + result_error;
//           if (result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 ||
//               result_index_count < 6) {
//             // Error bound
//             break;
//           }
//
//           simplified_indices.resize(result_index_count);
//         }
//
//         gpu_mesh.vertex_count = mesh_vertices.size();
//         gpu_mesh.lod_count += 1;
//         last_lod_indices = simplified_indices;
//
//         meshopt_optimizeVertexCache(
//           simplified_indices.data(),
//           simplified_indices.data(),
//           simplified_indices.size(),
//           vertex_count
//         );
//
//         // Worst case count
//         auto max_meshlet_count = meshopt_buildMeshletsBound(
//           simplified_indices.size(),
//           Model::MAX_MESHLET_INDICES,
//           Model::MAX_MESHLET_PRIMITIVES
//         );
//         auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
//         auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * Model::MAX_MESHLET_INDICES);
//         auto local_triangle_indices = std::vector<u8>(max_meshlet_count * Model::MAX_MESHLET_PRIMITIVES * 3);
//
//         auto meshlet_count = meshopt_buildMeshlets( //
//             raw_meshlets.data(),
//             indirect_vertex_indices.data(),
//             local_triangle_indices.data(),
//             simplified_indices.data(),
//             simplified_indices.size(),
//             reinterpret_cast<const f32*>(mesh_vertices.data()),
//             mesh_vertices.size(),
//             sizeof(glm::vec3),
//             Model::MAX_MESHLET_INDICES,
//             Model::MAX_MESHLET_PRIMITIVES,
//             0.0);
//
//         // Trim meshlets from worst case to current case
//         raw_meshlets.resize(meshlet_count);
//         auto meshlets = std::vector<GPU::Meshlet>(meshlet_count);
//         const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
//         indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
//         local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) &
//         ~3_u32));
//
//         auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
//         auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
//         auto meshlet_bounds = std::vector<GPU::Bounds>(meshlet_count);
//         for (const auto& [raw_meshlet, meshlet, bounds] : std::views::zip(raw_meshlets, meshlets, meshlet_bounds)) {
//           // AABB computation
//           auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
//           auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
//           for (u32 i = 0; i < raw_meshlet.triangle_count * 3; i++) {
//             const auto& tri_pos =
//               mesh_vertices[indirect_vertex_indices
//                               [raw_meshlet.vertex_offset + local_triangle_indices[raw_meshlet.triangle_offset + i]]];
//             meshlet_bb_min = glm::min(meshlet_bb_min, tri_pos);
//             meshlet_bb_max = glm::max(meshlet_bb_max, tri_pos);
//           }
//
//           // Sphere and Cone computation
//           auto sphere_bounds = meshopt_computeMeshletBounds( //
//               &indirect_vertex_indices[raw_meshlet.vertex_offset],
//               &local_triangle_indices[raw_meshlet.triangle_offset],
//               raw_meshlet.triangle_count,
//               reinterpret_cast<f32*>(mesh_vertices.data()),
//               vertex_count,
//               sizeof(glm::vec3));
//
//           meshlet.indirect_vertex_index_offset = raw_meshlet.vertex_offset;
//           meshlet.local_triangle_index_offset = raw_meshlet.triangle_offset;
//           meshlet.vertex_count = raw_meshlet.vertex_count;
//           meshlet.triangle_count = raw_meshlet.triangle_count;
//
//           bounds.aabb_center = (meshlet_bb_max + meshlet_bb_min) * 0.5f;
//           bounds.aabb_extent = meshlet_bb_max - meshlet_bb_min;
//           bounds.sphere_center = glm::make_vec3(sphere_bounds.center);
//           bounds.sphere_radius = sphere_bounds.radius;
//
//           mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
//           mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
//         }
//
//         gpu_mesh.bounds.aabb_center = (mesh_bb_max + mesh_bb_min) * 0.5f;
//         gpu_mesh.bounds.aabb_extent = mesh_bb_max - mesh_bb_min;
//
//         auto lod_upload_size = 0                                        //
//                                + ox::size_bytes(simplified_indices)     //
//                                + ox::size_bytes(meshlets)               //
//                                + ox::size_bytes(meshlet_bounds)         //
//                                + ox::size_bytes(local_triangle_indices) //
//                                + ox::size_bytes(indirect_vertex_indices);
//         auto cpu_lod_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, lod_upload_size);
//         auto cpu_lod_ptr = reinterpret_cast<u8*>(cpu_lod_buffer->mapped_ptr);
//
//         auto upload_offset = 0_u64;
//         cur_lod.indices = upload_offset;
//         std::memcpy(cpu_lod_ptr + upload_offset, simplified_indices.data(), ox::size_bytes(simplified_indices));
//         upload_offset += ox::size_bytes(simplified_indices);
//
//         cur_lod.meshlets = upload_offset;
//         std::memcpy(cpu_lod_ptr + upload_offset, meshlets.data(), ox::size_bytes(meshlets));
//         upload_offset += ox::size_bytes(meshlets);
//
//         cur_lod.meshlet_bounds = upload_offset;
//         std::memcpy(cpu_lod_ptr + upload_offset, meshlet_bounds.data(), ox::size_bytes(meshlet_bounds));
//         upload_offset += ox::size_bytes(meshlet_bounds);
//
//         cur_lod.local_triangle_indices = upload_offset;
//         std::memcpy(cpu_lod_ptr + upload_offset, local_triangle_indices.data(),
//         ox::size_bytes(local_triangle_indices)); upload_offset += ox::size_bytes(local_triangle_indices);
//
//         cur_lod.indirect_vertex_indices = upload_offset;
//         std::memcpy(
//           cpu_lod_ptr + upload_offset,
//           indirect_vertex_indices.data(),
//           ox::size_bytes(indirect_vertex_indices)
//         );
//         upload_offset += ox::size_bytes(indirect_vertex_indices);
//
//         cur_lod.indices_count = simplified_indices.size();
//         cur_lod.meshlet_count = meshlet_count;
//         cur_lod.meshlet_bounds_count = meshlet_bounds.size();
//         cur_lod.local_triangle_indices_count = local_triangle_indices.size();
//         cur_lod.indirect_vertex_indices_count = indirect_vertex_indices.size();
//
//         lod_cpu_buffers[lod_index] = std::pair(cpu_lod_buffer, lod_upload_size);
//         upload_size += lod_upload_size;
//       }
//
//       auto mesh_upload_offset = 0_u64;
//       gpu_mesh_buffer = context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, upload_size);
//
//       // Mesh first
//       auto cpu_mesh_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, mesh_upload_size);
//       auto cpu_mesh_ptr = reinterpret_cast<u8*>(cpu_mesh_buffer->mapped_ptr);
//
//       auto gpu_mesh_bda = gpu_mesh_buffer->device_address;
//       gpu_mesh.vertex_positions = gpu_mesh_bda + mesh_upload_offset;
//       std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_vertices.data(), ox::size_bytes(mesh_vertices));
//       mesh_upload_offset += ox::size_bytes(mesh_vertices);
//
//       gpu_mesh.vertex_normals = gpu_mesh_bda + mesh_upload_offset;
//       std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_normals.data(), ox::size_bytes(mesh_normals));
//       mesh_upload_offset += ox::size_bytes(mesh_normals);
//
//       if (!mesh_texcoords.empty()) {
//         gpu_mesh.texture_coords = gpu_mesh_bda + mesh_upload_offset;
//         std::memcpy(cpu_mesh_ptr + mesh_upload_offset, mesh_texcoords.data(), ox::size_bytes(mesh_texcoords));
//         mesh_upload_offset += ox::size_bytes(mesh_texcoords);
//       }
//
//       auto gpu_mesh_subrange = vuk::discard_buf("mesh", gpu_mesh_buffer->subrange(0, mesh_upload_size));
//       gpu_mesh_subrange = context.upload_staging(std::move(cpu_mesh_buffer), std::move(gpu_mesh_subrange));
//       context.wait_on(std::move(gpu_mesh_subrange));
//
//       for (auto lod_index = 0_sz; lod_index < gpu_mesh.lod_count; lod_index++) {
//         auto&& [lod_cpu_buffer, lod_upload_size] = lod_cpu_buffers[lod_index];
//         auto& lod = gpu_mesh.lods[lod_index];
//
//         lod.indices += gpu_mesh_bda + mesh_upload_offset;
//         lod.meshlets += gpu_mesh_bda + mesh_upload_offset;
//         lod.meshlet_bounds += gpu_mesh_bda + mesh_upload_offset;
//         lod.local_triangle_indices += gpu_mesh_bda + mesh_upload_offset;
//         lod.indirect_vertex_indices += gpu_mesh_bda + mesh_upload_offset;
//
//         auto gpu_lod_subrange = vuk::discard_buf(
//           "mesh lod subrange",
//           gpu_mesh_buffer->subrange(mesh_upload_offset, lod_upload_size)
//         );
//         gpu_lod_subrange = context.upload_staging(std::move(lod_cpu_buffer), std::move(gpu_lod_subrange));
//         context.wait_on(std::move(gpu_lod_subrange));
//
//         mesh_upload_offset += lod_upload_size;
//       }
//     }
//   }
//
//   return true;
// }
//
// auto AssetManager::unload_model(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   auto* model = this->get_model(asset->model_id);
//   for (auto& v : model->materials) {
//     this->unload_material(v);
//   }
//
//   model_map.destroy_slot(asset->model_id);
//   asset->model_id = ModelID::Invalid;
//
//   OX_LOG_TRACE("Unloaded model {}", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::load_texture(const UUID& uuid, const TextureLoadInfo& info) -> bool {
//   ZoneScoped;
//
//   auto read_lock = std::shared_lock(textures_mutex);
//   auto asset = this->get_asset(uuid);
//   asset->acquire_ref();
//
//   if (asset->is_loaded()) {
//     return true;
//   }
//
//   read_lock.unlock();
//
//   {
//     Texture texture{};
//     texture.create(asset->path, info);
//
//     auto write_lock = std::unique_lock(textures_mutex);
//     asset->texture_id = texture_map.create_slot(std::move(texture));
//
//     OX_LOG_INFO("Loaded texture {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->texture_id).index);
//   }
//
//   return true;
// }
//
// auto AssetManager::unload_texture(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!asset || !(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   texture_map.destroy_slot(asset->texture_id);
//   asset->texture_id = TextureID::Invalid;
//
//   OX_LOG_TRACE("Unloaded texture {}", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::load_material(
//   const UUID& uuid,
//   const Material& material_info,
//   option<ankerl::unordered_dense::map<UUID, TextureLoadInfo>> texture_info_map
// ) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//
//   // Materials don't explicitly load any resources, they need to increase child resources refs.
//
//   if (!asset->is_loaded()) {
//     auto write_lock = std::unique_lock(materials_mutex);
//     asset->material_id = material_map.create_slot(const_cast<Material&&>(material_info));
//   }
//
//   struct LoadInfo {
//     UUID texture_uuid = {};
//     MaterialID material_id = {};
//     TextureLoadInfo texture_load_info = {};
//   };
//   std::vector<LoadInfo> load_infos = {};
//
//   this->set_material_dirty(asset->material_id);
//   auto material = this->get_material(asset->material_id);
//
//   const auto get_info = [&texture_info_map](UUID& texture, vuk::Format format) -> TextureLoadInfo {
//     TextureLoadInfo info = {.format = format};
//     if (texture_info_map.has_value()) {
//       auto& map = texture_info_map.value();
//       if (map.contains(texture)) {
//         info = map[texture];
//         info.format = format;
//       }
//     }
//     return info;
//   };
//
//   auto& job_man = App::get_job_manager();
//
//   if (material->albedo_texture) {
//     auto info = get_info(material->albedo_texture, vuk::Format::eR8G8B8A8Srgb);
//     load_infos.emplace_back(LoadInfo{material->albedo_texture, asset->material_id, info});
//   }
//
//   if (material->normal_texture) {
//     auto info = get_info(material->normal_texture, vuk::Format::eR8G8B8A8Unorm);
//     load_infos.emplace_back(LoadInfo{material->normal_texture, asset->material_id, info});
//   }
//
//   if (material->emissive_texture) {
//     auto info = get_info(material->emissive_texture, vuk::Format::eR8G8B8A8Srgb);
//     load_infos.emplace_back(LoadInfo{material->emissive_texture, asset->material_id, info});
//   }
//
//   if (material->metallic_roughness_texture) {
//     auto info = get_info(material->metallic_roughness_texture, vuk::Format::eR8G8B8A8Unorm);
//     load_infos.emplace_back(LoadInfo{material->metallic_roughness_texture, asset->material_id, info});
//   }
//
//   if (material->occlusion_texture) {
//     auto info = get_info(material->occlusion_texture, vuk::Format::eR8G8B8A8Unorm);
//     load_infos.emplace_back(LoadInfo{material->occlusion_texture, asset->material_id, info});
//   }
//
//   job_man.push_job_name(fmt::format("Material job: {}", asset->uuid.str()));
//   job_man.for_each_async(
//     load_infos,
//     [](LoadInfo& info, usize index) {
//       auto& asset_man = App::mod<AssetManager>();
//       asset_man.load_texture(info.texture_uuid, info.texture_load_info);
//     },
//     [material_id = asset->material_id]() {
//       auto& asset_man = App::mod<AssetManager>();
//       asset_man.set_material_dirty(material_id);
//     }
//   );
//   job_man.pop_job_name();
//
//   asset->acquire_ref();
//   return true;
// }
//
// auto AssetManager::unload_material(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   auto material = this->get_material(asset->material_id);
//   if (material->albedo_texture) {
//     this->unload_texture(material->albedo_texture);
//   }
//
//   if (material->normal_texture) {
//     this->unload_texture(material->normal_texture);
//   }
//
//   if (material->emissive_texture) {
//     this->unload_texture(material->emissive_texture);
//   }
//
//   if (material->metallic_roughness_texture) {
//     this->unload_texture(material->metallic_roughness_texture);
//   }
//
//   if (material->occlusion_texture) {
//     this->unload_texture(material->occlusion_texture);
//   }
//
//   material_map.destroy_slot(asset->material_id);
//   asset->material_id = MaterialID::Invalid;
//
//   OX_LOG_INFO("Unloaded material {}", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::load_scene(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   asset->scene_id = this->scene_map.create_slot(std::make_unique<Scene>());
//   auto* scene = this->scene_map.slot(asset->scene_id)->get();
//
//   scene->init("unnamed_scene");
//
//   if (!scene->load_from_file(asset->path)) {
//     return false;
//   }
//
//   asset->acquire_ref();
//   return true;
// }
//
// auto AssetManager::unload_scene(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   scene_map.destroy_slot(asset->scene_id);
//   asset->scene_id = SceneID::Invalid;
//
//   OX_LOG_INFO("Unloaded scene {}", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::load_audio(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   asset->acquire_ref();
//
//   if (asset->is_loaded()) {
//     return true;
//   }
//
//   AudioSource audio{};
//   audio.load(asset->path);
//   asset->audio_id = audio_map.create_slot(std::move(audio));
//
//   OX_LOG_INFO("Loaded audio {}", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::unload_audio(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!asset || !(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   auto* audio = this->get_audio(asset->audio_id);
//   OX_CHECK_NULL(audio);
//   audio->unload();
//
//   audio_map.destroy_slot(asset->audio_id);
//   asset->audio_id = AudioID::Invalid;
//
//   OX_LOG_INFO("Unloaded audio {}.", uuid.str());
//
//   return true;
// }
//
// auto AssetManager::load_script(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   asset->acquire_ref();
//
//   if (asset->is_loaded())
//     return true;
//
//   asset->script_id = script_map.create_slot(std::make_unique<LuaSystem>());
//   auto* system = script_map.slot(asset->script_id);
//   system->get()->load(asset->path);
//
//   OX_LOG_INFO("Loaded script {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->script_id).index);
//
//   return true;
// }
//
// auto AssetManager::unload_script(const UUID& uuid) -> bool {
//   ZoneScoped;
//
//   auto asset = this->get_asset(uuid);
//   if (!asset || !(asset->is_loaded() && asset->release_ref())) {
//     return false;
//   }
//
//   script_map.destroy_slot(asset->script_id);
//   asset->script_id = ScriptID::Invalid;
//
//   OX_LOG_INFO("Unloaded script {}.", uuid.str());
//
//   return true;
// }

auto AssetManager::is_valid(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  return uuid && self.get_asset(uuid);
}

auto AssetManager::is_loaded(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);

  return asset->is_loaded();
}

auto AssetManager::get_asset(this AssetManager& self, const UUID& uuid) -> Borrowed<Asset> {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.registry_mutex);
  const auto it = self.asset_registry.find(uuid);
  if (it == self.asset_registry.end()) {
    return {};
  }

  return Borrowed(self.registry_mutex, &it->second);
}

auto AssetManager::get_asset_info(this AssetManager& self, const UUID& uuid) -> Borrowed<ExtendedAsset> {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.registry_mutex);
  const auto it = self.extended_registry.find(uuid);
  if (it == self.extended_registry.end()) {
    return {};
  }

  return Borrowed(self.registry_mutex, &it->second);
}

auto AssetManager::get_model(this AssetManager& self, const UUID& uuid) -> Model* {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Model);
  return self.get_model(asset->model_id);
}

auto AssetManager::get_model(this AssetManager& self, const ModelID model_id) -> Model* {
  ZoneScoped;

  if (model_id == ModelID::Invalid) {
    return nullptr;
  }

  return self.model_map.slot(model_id);
}

auto AssetManager::get_texture(this AssetManager& self, const UUID& uuid) -> Borrowed<Texture> {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset) {
    return {};
  }

  OX_CHECK_EQ(asset->type, AssetType::Texture);
  return self.get_texture(asset->texture_id);
}

auto AssetManager::get_texture(this AssetManager& self, const TextureID texture_id) -> Borrowed<Texture> {
  ZoneScoped;

  if (texture_id == TextureID::Invalid) {
    return {};
  }

  return Borrowed(self.textures_mutex, self.texture_map.slot(texture_id));
}

auto AssetManager::get_material(this AssetManager& self, const UUID& uuid) -> Borrowed<Material> {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset) {
    return {};
  }

  OX_CHECK_EQ(asset->type, AssetType::Material);
  return self.get_material(asset->material_id);
}

auto AssetManager::get_material(this AssetManager& self, const MaterialID material_id) -> Borrowed<Material> {
  ZoneScoped;

  if (material_id == MaterialID::Invalid) {
    return {};
  }

  return Borrowed(self.materials_mutex, self.material_map.slot(material_id));
}

auto AssetManager::set_material_dirty(this AssetManager& self, MaterialID material_id) -> void {
  ZoneScoped;

  std::shared_lock shared_lock(self.materials_mutex);
  if (std::ranges::find(self.dirty_materials, material_id) != self.dirty_materials.end()) {
    return;
  }

  shared_lock.unlock();
  self.materials_mutex.lock();
  self.dirty_materials.emplace_back(material_id);
  self.materials_mutex.unlock();
}

auto AssetManager::set_material_dirty(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto material = self.get_asset(uuid);
  self.set_material_dirty(material->material_id);
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

auto AssetManager::get_scene(this AssetManager& self, const UUID& uuid) -> Scene* {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Scene);
  return self.get_scene(asset->scene_id);
}

auto AssetManager::get_scene(this AssetManager& self, const SceneID scene_id) -> Scene* {
  ZoneScoped;

  if (scene_id == SceneID::Invalid) {
    return nullptr;
  }

  return self.scene_map.slot(scene_id)->get();
}

auto AssetManager::get_audio(this AssetManager& self, const UUID& uuid) -> AudioSource* {
  auto asset = self.get_asset(uuid);
  if (!asset) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Audio);
  return self.get_audio(asset->audio_id);
}

auto AssetManager::get_audio(this AssetManager& self, const AudioID audio_id) -> AudioSource* {
  ZoneScoped;

  if (audio_id == AudioID::Invalid) {
    return nullptr;
  }

  return self.audio_map.slot(audio_id);
}

auto AssetManager::get_script(this AssetManager& self, const UUID& uuid) -> LuaSystem* {
  auto asset = self.get_asset(uuid);
  if (!asset) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Script);
  return self.get_script(asset->script_id);
}

auto AssetManager::get_script(this AssetManager& self, ScriptID script_id) -> LuaSystem* {
  ZoneScoped;

  if (script_id == ScriptID::Invalid) {
    return nullptr;
  }

  return self.script_map.slot(script_id)->get();
}
} // namespace ox
