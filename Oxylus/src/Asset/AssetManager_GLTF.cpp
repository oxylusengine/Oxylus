#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <queue>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"

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

auto AssetManager::write_gltf_meta(AssetManager& self, const std::filesystem::path& path, JsonWriter& json) -> bool {
  ZoneScoped;

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    OX_LOG_ERROR("GLTF model type is invalid!");
    return false;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(gltf_result.error()));
    return false;
  }

  auto gltf_asset = std::move(gltf_result.get());
  json["embedded_images"].begin_array();
  for (const auto& v : gltf_asset.images) {
    if (std::get_if<fastgltf::sources::URI>(&v.data) != nullptr) {
      json << UUID::generate_random().str();
    }
  }
  json.end_array();

  json["materials"].begin_array();
  for (const auto& v : gltf_asset.materials) {
    json << UUID::generate_random().str();
  }
  json.end_array();

  return true;
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

  // extract UUIDs from meta file
  auto embedded_textures = ankerl::unordered_dense::map<usize, UUID>{};
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
      embedded_textures.emplace(image_index, texture_uuid.value());
    }
  }

  // determine and initialize texture info
  auto textures = std::vector<UUID>{};
  for (const auto& [gltf_texture, texture_index] : std::views::zip(gltf_asset.textures, std::views::iota(0_sz))) {
    if (auto& image_index = gltf_texture.imageIndex; image_index.has_value()) {
      auto& image = gltf_asset.images[image_index.value()];

      auto texture_uuid = UUID{};
      if (!std::get_if<fastgltf::sources::URI>(&image.data)) {
        auto embedded_texture_it = embedded_textures.find(texture_index);
        if (embedded_texture_it != embedded_textures.end()) {
          texture_uuid = embedded_texture_it->second;
        } else {
          texture_uuid = UUID::generate_random();
        }
      }

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
      textures.push_back(texture_uuid);
    }
  }

  auto initial_materials = std::vector<UUID>{};
  auto embedded_materials_json = meta_json->doc["embedded_materials"];
  for (auto embedded_material_json : embedded_materials_json) {
    if (embedded_material_json.error() || !embedded_material_json.is_string()) {
      OX_LOG_ERROR("Failed to import model {}! Bad `embdedded_materials` field.", asset_path);
      return false;
    }

    auto material_uuid = UUID::from_string(embedded_material_json.get_string());
    if (!material_uuid.has_value()) {
      OX_LOG_ERROR("Failed to import model {}! A material with corrupt UUID.", asset_path);
      return false;
    }

    register_asset(material_uuid.value(), AssetType::Material, asset_path);
    initial_materials.emplace_back(material_uuid.value());
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

  auto model = Model{};
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

} // namespace ox
