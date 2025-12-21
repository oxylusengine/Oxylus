#include <Asset/AssetFile.hpp>
#include <Asset/AssetMetadata.hpp>
#include <Asset/Texture.hpp>
#include <Core/Option.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <meshoptimizer.h>
#include <queue>
#include <ranges>

#include "AssetData.hpp"
#include "ResourceCompiler.hpp"
#include "Session.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};

namespace ox::rc {
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

auto gltf_mime_type_to_file_format(fastgltf::MimeType mime) -> FileFormat {
  switch (mime) {
    case fastgltf::MimeType::JPEG: return FileFormat::JPEG;
    case fastgltf::MimeType::PNG : return FileFormat::PNG;
    case fastgltf::MimeType::KTX2: return FileFormat::KTX2;
    default                      : return FileFormat::Unknown;
  }
}

auto gltf_sampler_to_sampler(const fastgltf::Sampler& gltf_sampler) -> SamplerInfo {
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

  return SamplerInfo{
    .min_filter = get_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .mag_filter = get_filter_mode(gltf_sampler.magFilter.value_or(fastgltf::Filter::Linear)),
    .mipmap_mode = get_mip_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .addr_u = get_address_mode(gltf_sampler.wrapS),
    .addr_v = get_address_mode(gltf_sampler.wrapT),
  };
}

struct ProcessedNode {
  std::string name = {};
  std::vector<u32> child_indices = {};
  std::vector<u32> mesh_indices = {};
  glm::vec3 translation = {};
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 scale = {};
};

struct ProcessingNode {
  usize gltf_node_index = 0;
  usize parent_node_index = 0;
};

constexpr static auto MAX_LODS = 8_sz;
constexpr static auto MAX_MESHLET_INDICES = 64_sz;
constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

auto process_model(Session self, const ModelProcessRequest& request) -> AssetID {
  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(request.path);
  if (!gltf_buffer) {
    self.push_error(fmt::format("GLTF model does not exist in path {}!", request.path));
    return AssetID::Invalid;
  }

  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    self.push_error(fmt::format("GLTF model {} type is invalid!", request.path));
    return AssetID::Invalid;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), request.path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    self.push_error(
      fmt::format("Failed to load GLTF! {} {}", request.path, fastgltf::getErrorMessage(gltf_result.error()))
    );
    return AssetID::Invalid;
  }

  auto gltf_asset = std::move(gltf_result.get());
  auto& gltf_default_scene = gltf_asset.scenes[gltf_asset.defaultScene.value_or(0_sz)];
  auto asset_data = std::vector<u8>();
  auto processed_nodes = std::vector<ProcessedNode>{};
  auto processing_gltf_nodes = std::queue<ProcessingNode>{};

  auto& root_node = processed_nodes.emplace_back();
  root_node.name = gltf_default_scene.name;
  for (auto node_index : gltf_default_scene.nodeIndices) {
    processing_gltf_nodes.push({node_index, 0});
  }

  auto asset_nodes = std::vector<ModelAssetEntry::Node>{};
  auto asset_meshes = std::vector<ModelAssetEntry::Mesh>{};
  while (!processing_gltf_nodes.empty()) {
    auto [gltf_node_index, parent_node_index] = processing_gltf_nodes.front();
    const auto& gltf_node = gltf_asset.nodes[gltf_node_index];
    auto& parent_node = processed_nodes[parent_node_index];
    processing_gltf_nodes.pop();

    auto node_index = processed_nodes.size();
    parent_node.child_indices.push_back(node_index);

    auto& node = processed_nodes.emplace_back();
    node.name = gltf_node.name;

    for (auto child_node_index : gltf_node.children) {
      processing_gltf_nodes.push({child_node_index, node_index});
    }

    // Node translation
    auto translation = glm::vec3{};
    auto rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    auto scale = glm::vec3{};
    if (auto* trs = std::get_if<fastgltf::TRS>(&gltf_node.transform)) {
      translation = glm::make_vec3(trs->translation.data());
      rotation = glm::quat::wxyz(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
      scale = glm::make_vec3(trs->scale.data());
    } else if (auto* model_matrix = std::get_if<fastgltf::math::fmat4x4>(&gltf_node.transform)) {
      auto transform = glm::make_mat4x4(model_matrix->data());
      auto skew = glm::vec3{};
      auto perspective = glm::vec4{};
      glm::decompose(transform, scale, rotation, translation, skew, perspective);
    }

    node.translation = translation;
    node.rotation = rotation;
    node.scale = scale;

    if (gltf_node.meshIndex.has_value()) {
      const auto& gltf_mesh = gltf_asset.meshes[gltf_node.meshIndex.value()];
      for (const auto& gltf_primitive : gltf_mesh.primitives) {
        if (!gltf_primitive.indicesAccessor.has_value() || !gltf_primitive.materialIndex.has_value()) {
          continue;
        }

        auto mesh = ModelAssetEntry::Mesh{};
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

        auto normals = std::vector<glm::vec3>(vertex_count);
        if (auto attrib = gltf_primitive.findAttribute("NORMAL"); attrib != gltf_primitive.attributes.end()) {
          auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
          auto raw_normals = std::vector<glm::vec3>(accessor.count);

          fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 normal, usize i) { //
            raw_normals[i] = normal;
          });

          meshopt_remapVertexBuffer(
            normals.data(),
            raw_normals.data(),
            raw_normals.size(),
            sizeof(glm::vec3),
            vertex_remap.data()
          );
        }

        auto texcoords = std::vector<glm::vec2>(vertex_count);
        if (auto attrib = gltf_primitive.findAttribute("TEXCOORD_0"); attrib != gltf_primitive.attributes.end()) {
          auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
          auto raw_texcoords = std::vector<glm::vec2>(accessor.count);

          fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf_asset, accessor, [&](glm::vec2 uv, usize i) { //
            raw_texcoords[i] = uv;
          });

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

        auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
        auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
        auto last_lod_indices = std::vector<u32>();
        auto lods = std::array<ModelAssetEntry::Lod, MAX_LODS>{};
        auto lod_index = 0_sz;
        for (; lod_index < MAX_LODS; lod_index++) {
          auto& cur_lod = lods[lod_index];
          auto simplified_indices = std::vector<u32>();
          if (lod_index == 0) {
            simplified_indices.resize(indices.size());
            std::memcpy(simplified_indices.data(), indices.data(), ox::size_bytes(indices));
          } else {
            const auto& last_lod = lods[lod_index - 1];
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

          meshopt_optimizeVertexCache(
            simplified_indices.data(),
            simplified_indices.data(),
            simplified_indices.size(),
            vertex_count
          );

          auto max_meshlet_count = meshopt_buildMeshletsBound(
            simplified_indices.size(),
            MAX_MESHLET_INDICES,
            MAX_MESHLET_PRIMITIVES
          );
          auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
          auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * MAX_MESHLET_INDICES);
          auto local_triangle_indices = std::vector<u8>(max_meshlet_count * MAX_MESHLET_PRIMITIVES * 3);

          auto meshlet_count = meshopt_buildMeshlets(
            raw_meshlets.data(),
            indirect_vertex_indices.data(),
            local_triangle_indices.data(),
            simplified_indices.data(),
            simplified_indices.size(),
            reinterpret_cast<const f32*>(positions.data()),
            vertex_count,
            sizeof(glm::vec3),
            MAX_MESHLET_INDICES,
            MAX_MESHLET_PRIMITIVES,
            0.0
          );

          // Trim meshlets from worst case to current case
          raw_meshlets.resize(meshlet_count);
          auto meshlets = std::vector<ModelAssetEntry::Meshlet>(meshlet_count);
          const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
          indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
          local_triangle_indices.resize(
            last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32)
          );

          auto meshlet_bounds = std::vector<ModelAssetEntry::Bounds>(meshlet_count);
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

          cur_lod.indices = push_span(asset_data, std::span<const u32>(simplified_indices));
          cur_lod.meshlets = push_span(asset_data, std::span<const ModelAssetEntry::Meshlet>(meshlets));
          cur_lod.meshlet_bounds = push_span(asset_data, std::span<const ModelAssetEntry::Bounds>(meshlet_bounds));
          cur_lod.local_triangle_indices = push_span(asset_data, std::span<const u8>(local_triangle_indices));
          cur_lod.indirect_vertex_indices = push_span(asset_data, std::span<const u32>(indirect_vertex_indices));

          last_lod_indices = std::move(simplified_indices);
        }

        mesh.vertex_positions = push_span(asset_data, std::span<const glm::vec3>(positions));
        mesh.vertex_normals = push_span(asset_data, std::span<const glm::vec3>(normals));
        mesh.vertex_texcoords = push_span(asset_data, std::span<const glm::vec2>(texcoords));
        mesh.lods = push_span(asset_data, std::span<const ModelAssetEntry::Lod>{lods.data(), lod_index});
        mesh.bounds.aabb_center = (mesh_bb_max + mesh_bb_min) * 0.5f;
        mesh.bounds.aabb_extent = mesh_bb_max - mesh_bb_min;

        node.mesh_indices.push_back(asset_meshes.size());
        asset_meshes.push_back(mesh);
      }
    }
  }

  for (const auto& node : processed_nodes) {
    auto name = push_str(asset_data, node.name);
    auto child_indices = push_span(asset_data, std::span(node.child_indices));
    auto mesh_indices = push_span(asset_data, std::span(node.mesh_indices));

    asset_nodes.emplace_back(name, child_indices, mesh_indices, node.translation, node.rotation, node.scale);
  }

  auto asset_id = self.create_asset(UUID::generate_random(), AssetType::Model);
  auto asset = ModelAssetEntry{
    .nodes = push_span(asset_data, std::span<const ModelAssetEntry::Node>(asset_nodes)),
    .meshes = push_span(asset_data, std::span<const ModelAssetEntry::Mesh>(asset_meshes)),
  };
  self.set_asset_info(asset_id, asset);
  self.set_asset_data(asset_id, asset_data);

  self.push_message(fmt::format("Processed model {}", request.path));

  return asset_id;
}

} // namespace ox::rc
