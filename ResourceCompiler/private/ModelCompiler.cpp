#include "ModelCompiler.hpp"

#include <ankerl/unordered_dense.h>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fmt/format.h>
#include <fmt/std.h>
#include <glm/ext/vector_uint2_sized.hpp>
#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <limits>
#include <meshoptimizer.h>
#include <numeric>
#include <queue>
#include <ranges>

#include "AnimationCompiler.hpp"
#include "GltfElementTraits.hpp"
#include "Parallel.hpp"
#include "Session.hpp"
#include "TextureCompiler.hpp"

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

  return options;
}

// Priority: DDS > KTX2/basisu > WebP > base imageIndex
auto get_effective_image_index(const fastgltf::Texture& texture) -> option<usize> {
  if (texture.ddsImageIndex.has_value())
    return texture.ddsImageIndex.value();
  if (texture.basisuImageIndex.has_value())
    return texture.basisuImageIndex.value();
  if (texture.webpImageIndex.has_value())
    return texture.webpImageIndex.value();
  if (texture.imageIndex.has_value())
    return texture.imageIndex.value();
  return nullopt;
}

auto gltf_alpha_mode_to_alpha_mode(fastgltf::AlphaMode mode) -> AlphaMode {
  switch (mode) {
    case fastgltf::AlphaMode::Opaque: return AlphaMode::Opaque;
    case fastgltf::AlphaMode::Mask  : return AlphaMode::Mask;
    case fastgltf::AlphaMode::Blend : return AlphaMode::Blend;
  }

  return AlphaMode::Opaque;
}

auto texture_index_of(const fastgltf::TextureInfo& info, usize texture_count) -> u32 {
  if (info.textureIndex >= texture_count) {
    return ModelData::INVALID_INDEX;
  }

  return static_cast<u32>(info.textureIndex);
}

auto gltf_material_to_material(const fastgltf::Material& gltf_material, usize texture_count) -> ModelData::Material {
  auto material = ModelData::Material{};
  material.name = std::string(gltf_material.name);

  const auto& pbr = gltf_material.pbrData;
  material.albedo_color[0] = pbr.baseColorFactor.x();
  material.albedo_color[1] = pbr.baseColorFactor.y();
  material.albedo_color[2] = pbr.baseColorFactor.z();
  material.albedo_color[3] = pbr.baseColorFactor.w();
  material.roughness_factor = pbr.roughnessFactor;
  material.metallic_factor = pbr.metallicFactor;

  material.alpha_mode = gltf_alpha_mode_to_alpha_mode(gltf_material.alphaMode);
  material.alpha_cutoff = gltf_material.alphaCutoff;

  material.emissive_color[0] = gltf_material.emissiveFactor.x() * gltf_material.emissiveStrength;
  material.emissive_color[1] = gltf_material.emissiveFactor.y() * gltf_material.emissiveStrength;
  material.emissive_color[2] = gltf_material.emissiveFactor.z() * gltf_material.emissiveStrength;

  // Material carries one uv_offset/uv_size for every slot, so only base colour's KHR_texture_transform
  // can be honoured. A transform on any other slot is dropped rather than fought over.
  auto resolve_uv_transform = [&](const fastgltf::TextureInfo& info) {
    if (info.transform) {
      material.uv_offset[0] = info.transform->uvOffset[0];
      material.uv_offset[1] = info.transform->uvOffset[1];
      material.uv_size[0] = info.transform->uvScale[0];
      material.uv_size[1] = info.transform->uvScale[1];
    }
  };

  if (pbr.baseColorTexture.has_value()) {
    material.albedo_texture_index = texture_index_of(pbr.baseColorTexture.value(), texture_count);
    resolve_uv_transform(pbr.baseColorTexture.value());
  }

  if (pbr.metallicRoughnessTexture.has_value()) {
    material.metallic_roughness_texture_index = texture_index_of(pbr.metallicRoughnessTexture.value(), texture_count);
  }

  if (gltf_material.normalTexture.has_value()) {
    material.normal_texture_index = texture_index_of(gltf_material.normalTexture.value(), texture_count);
    material.normal_scale = gltf_material.normalTexture->scale;
  }

  if (gltf_material.occlusionTexture.has_value()) {
    material.occlusion_texture_index = texture_index_of(gltf_material.occlusionTexture.value(), texture_count);
    material.occlusion_strength = gltf_material.occlusionTexture->strength;
  }

  if (gltf_material.emissiveTexture.has_value()) {
    material.emissive_texture_index = texture_index_of(gltf_material.emissiveTexture.value(), texture_count);
  }

  return material;
}

auto extract_linear_texture_indices(const fastgltf::Asset& asset) -> ankerl::unordered_dense::set<usize> {
  ZoneScoped;

  auto result = ankerl::unordered_dense::set<usize>{};
  for (const auto& material : asset.materials) {
    if (material.normalTexture.has_value())
      result.insert(material.normalTexture->textureIndex);
    if (material.pbrData.metallicRoughnessTexture.has_value())
      result.insert(material.pbrData.metallicRoughnessTexture->textureIndex);
    if (material.occlusionTexture.has_value())
      result.insert(material.occlusionTexture->textureIndex);

    if (material.clearcoat) {
      if (material.clearcoat->clearcoatRoughnessTexture.has_value())
        result.insert(material.clearcoat->clearcoatRoughnessTexture->textureIndex);
      if (material.clearcoat->clearcoatNormalTexture.has_value())
        result.insert(material.clearcoat->clearcoatNormalTexture->textureIndex);
    }
    if (material.sheen) {
      if (material.sheen->sheenRoughnessTexture.has_value())
        result.insert(material.sheen->sheenRoughnessTexture->textureIndex);
    }
    if (material.specular) {
      if (material.specular->specularTexture.has_value())
        result.insert(material.specular->specularTexture->textureIndex);
    }
    if (material.transmission) {
      if (material.transmission->transmissionTexture.has_value())
        result.insert(material.transmission->transmissionTexture->textureIndex);
    }
    if (material.anisotropy) {
      if (material.anisotropy->anisotropyTexture.has_value())
        result.insert(material.anisotropy->anisotropyTexture->textureIndex);
    }
  }

  return result;
}

auto blob_append(std::vector<u8>& blob, const void* data, usize size, usize alignment) -> u64 {
  const auto offset = ox::align_up(blob.size(), alignment);
  blob.resize(offset + size);
  if (size != 0) {
    std::memcpy(blob.data() + offset, data, size);
  }

  return offset;
}

template <typename T>
auto blob_append(std::vector<u8>& blob, const std::vector<T>& data, usize alignment) -> u64 {
  return blob_append(blob, data.data(), ox::size_bytes(data), alignment);
}

auto build_mesh(const MeshSource& source, const SkinBinding& skin) -> option<ModelData::Mesh> {
  ZoneScoped;

  if (source.positions.empty() || source.indices.empty()) {
    return nullopt;
  }

  // The project scan compiles every model it finds, so a malformed primitive has to be rejected
  // here rather than reaching meshoptimizer, which only asserts about it in debug builds.
  if (source.indices.size() % 3 != 0) {
    return nullopt;
  }

  if (std::ranges::any_of(source.indices, [&](u32 index) { return index >= source.positions.size(); })) {
    return nullopt;
  }

  auto vertex_remap = std::vector<u32>(source.positions.size());
  const auto vertex_count = static_cast<u32>(meshopt_optimizeVertexFetchRemap(
    vertex_remap.data(),
    source.indices.data(),
    source.indices.size(),
    source.positions.size()
  ));

  auto positions = std::vector<glm::vec3>(vertex_count);
  meshopt_remapVertexBuffer(
    positions.data(),
    source.positions.data(),
    source.positions.size(),
    sizeof(glm::vec3),
    vertex_remap.data()
  );

  auto normals = std::vector<glm::vec3>();
  if (source.normals.size() == source.positions.size()) {
    normals.resize(vertex_count);
    meshopt_remapVertexBuffer(
      normals.data(),
      source.normals.data(),
      source.normals.size(),
      sizeof(glm::vec3),
      vertex_remap.data()
    );
  }

  auto texcoords = std::vector<glm::vec2>();
  if (source.texcoords.size() == source.positions.size()) {
    texcoords.resize(vertex_count);
    meshopt_remapVertexBuffer(
      texcoords.data(),
      source.texcoords.data(),
      source.texcoords.size(),
      sizeof(glm::vec2),
      vertex_remap.data()
    );
  }

  auto joints = std::vector<glm::u16vec4>();
  auto weights = std::vector<glm::vec4>();
  if (
    !skin.joint_to_bone.empty() && source.joints.size() == source.positions.size() &&
    source.weights.size() == source.positions.size()
  ) {
    joints.resize(vertex_count);
    meshopt_remapVertexBuffer(
      joints.data(),
      source.joints.data(),
      source.joints.size(),
      sizeof(glm::u16vec4),
      vertex_remap.data()
    );

    weights.resize(vertex_count);
    meshopt_remapVertexBuffer(
      weights.data(),
      source.weights.data(),
      source.weights.size(),
      sizeof(glm::vec4),
      vertex_remap.data()
    );
  }

  auto indices = std::vector<u32>(source.indices.size());
  meshopt_remapIndexBuffer(indices.data(), source.indices.data(), source.indices.size(), vertex_remap.data());

  auto quantized_positions = std::vector<glm::u16vec4>(vertex_count);
  for (const auto& [position, quantized_position] : std::views::zip(positions, quantized_positions)) {
    quantized_position.x = meshopt_quantizeHalf(position.x);
    quantized_position.y = meshopt_quantizeHalf(position.y);
    quantized_position.z = meshopt_quantizeHalf(position.z);
  }

  auto quantized_normals = std::vector<u32>(vertex_count);
  for (const auto& [normal, quantized_normal] : std::views::zip(normals, quantized_normals)) {
    quantized_normal = ((meshopt_quantizeSnorm(normal.x, 10) + 511) << 20) |
                       ((meshopt_quantizeSnorm(normal.y, 10) + 511) << 10) |
                       (meshopt_quantizeSnorm(normal.z, 10) + 511);
  }

  auto quantized_texcoords = std::vector<glm::u16vec2>(vertex_count);
  for (const auto& [texcoord, quantized_texcoord] : std::views::zip(texcoords, quantized_texcoords)) {
    quantized_texcoord.x = meshopt_quantizeHalf(texcoord.x);
    quantized_texcoord.y = meshopt_quantizeHalf(texcoord.y);
  }

  auto quantized_joints = std::vector<glm::u16vec4>();
  auto quantized_weights = std::vector<glm::u16vec4>();
  auto max_bone_influence_radius = 0.0f;
  if (!joints.empty()) {
    quantized_joints.resize(vertex_count);
    quantized_weights.resize(vertex_count);

    for (auto i = 0_u32; i < vertex_count; ++i) {
      // exporters routinely emit weights that sum to slightly off 1.0, and the shader does a plain
      // weighted sum with no correction of its own
      auto weight = weights[i];
      const auto sum = weight.x + weight.y + weight.z + weight.w;
      weight = sum > glm::epsilon<f32>() ? weight / sum : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

      for (auto influence = 0_u32; influence < MAX_BONE_INFLUENCES; ++influence) {
        const auto component = static_cast<int>(influence);
        const auto joint = joints[i][component];
        const auto bone = joint < skin.joint_to_bone.size() ? skin.joint_to_bone[joint] : 0_u32;
        quantized_joints[i][component] = static_cast<u16>(bone);
        quantized_weights[i][component] = static_cast<u16>(glm::clamp(weight[component], 0.0f, 1.0f) * 65535.0f + 0.5f);

        // widest bind-pose reach of any bone, which is what inflates the animated culling bounds
        if (weight[component] > 0.0f && bone < skin.inverse_bind_pose.size()) {
          const auto bone_local = skin.inverse_bind_pose[bone].transform_point(positions[i]);
          max_bone_influence_radius = glm::max(max_bone_influence_radius, glm::length(bone_local));
        }
      }
    }
  }

  auto mesh = ModelData::Mesh{};
  mesh.name = source.name;
  mesh.vertex_count = vertex_count;
  mesh.max_bone_influence_radius = max_bone_influence_radius;

  mesh.collision_positions.reserve(positions.size() * 3);
  for (const auto& position : positions) {
    mesh.collision_positions.push_back(position.x);
    mesh.collision_positions.push_back(position.y);
    mesh.collision_positions.push_back(position.z);
  }
  mesh.collision_indices = indices;

  mesh.vertex_positions_offset = blob_append(mesh.blob, quantized_positions, 8);
  mesh.vertex_normals_offset = blob_append(mesh.blob, quantized_normals, 4);
  if (!texcoords.empty()) {
    mesh.has_texture_coords = true;
    mesh.texture_coords_offset = blob_append(mesh.blob, quantized_texcoords, 4);
  }
  if (!quantized_joints.empty()) {
    mesh.has_skin = true;
    mesh.skin_joint_indices_offset = blob_append(mesh.blob, quantized_joints, 8);
    mesh.skin_weights_offset = blob_append(mesh.blob, quantized_weights, 8);
  }

  auto last_lod_indices = std::vector<u32>();
  for (auto lod_index = 0_sz; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
    ZoneNamedN(z, "GPU Meshlet Generation", true);

    auto& cur_lod = mesh.lods[lod_index];
    auto simplified_indices = std::vector<u32>();
    if (lod_index == 0) {
      simplified_indices = indices;
    } else {
      const auto& last_lod = mesh.lods[lod_index - 1];
      auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
      simplified_indices.resize(last_lod_indices.size(), 0_u32);
      constexpr auto TARGET_ERROR = std::numeric_limits<f32>::max();
      constexpr f32 NORMAL_WEIGHTS[] = {1.0f, 1.0f, 1.0f};

      auto result_error = 0.0f;
      // A primitive without NORMAL leaves nothing to weight the attribute error by, and handing
      // meshoptimizer a null attribute buffer dereferences it.
      auto result_index_count = normals.empty() ? meshopt_simplify(
                                                    simplified_indices.data(),
                                                    last_lod_indices.data(),
                                                    last_lod_indices.size(),
                                                    reinterpret_cast<const f32*>(positions.data()),
                                                    vertex_count,
                                                    sizeof(glm::vec3),
                                                    lod_index_count,
                                                    TARGET_ERROR,
                                                    meshopt_SimplifyLockBorder,
                                                    &result_error
                                                  )
                                                : meshopt_simplifyWithAttributes(
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
      if (
        result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 || result_index_count < 6
      ) {
        break;
      }

      simplified_indices.resize(result_index_count);
    }

    if (simplified_indices.size() < 3) {
      break;
    }

    last_lod_indices = simplified_indices;

    meshopt_optimizeVertexCache(
      simplified_indices.data(),
      simplified_indices.data(),
      simplified_indices.size(),
      vertex_count
    );

    auto max_meshlet_count = meshopt_buildMeshletsBound(
      simplified_indices.size(),
      GPU::Mesh::MAX_MESHLET_INDICES,
      GPU::Mesh::MAX_MESHLET_PRIMITIVES
    );
    auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
    auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * GPU::Mesh::MAX_MESHLET_INDICES);
    auto local_triangle_indices = std::vector<u8>(max_meshlet_count * GPU::Mesh::MAX_MESHLET_PRIMITIVES * 3);

    auto meshlet_count = meshopt_buildMeshlets(
      raw_meshlets.data(),
      indirect_vertex_indices.data(),
      local_triangle_indices.data(),
      simplified_indices.data(),
      simplified_indices.size(),
      reinterpret_cast<const f32*>(positions.data()),
      vertex_count,
      sizeof(glm::vec3),
      GPU::Mesh::MAX_MESHLET_INDICES,
      GPU::Mesh::MAX_MESHLET_PRIMITIVES,
      0.0
    );

    if (meshlet_count == 0) {
      break;
    }

    raw_meshlets.resize(meshlet_count);
    auto meshlets = std::vector<GPU::Meshlet>(meshlet_count);
    const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
    indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
    local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32));

    auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
    auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
    auto gpu_meshlet_bounds = std::vector<GPU::MeshletBounds>(meshlet_count);
    for (const auto& [raw_meshlet, meshlet, bounds] : std::views::zip(raw_meshlets, meshlets, gpu_meshlet_bounds)) {
      auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
      auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
      for (u32 i = 0; i < raw_meshlet.triangle_count * 3; i++) {
        auto local_triangle_index_offset = raw_meshlet.triangle_offset + i;
        auto local_triangle_index = local_triangle_indices[local_triangle_index_offset];
        auto indirect_vertex_index_offset = raw_meshlet.vertex_offset + local_triangle_index;
        auto indirect_vertex_index = indirect_vertex_indices[indirect_vertex_index_offset];

        const auto& tri_pos = positions[indirect_vertex_index];
        meshlet_bb_min = glm::min(meshlet_bb_min, tri_pos);
        meshlet_bb_max = glm::max(meshlet_bb_max, tri_pos);
      }

      auto meshlet_bounds = meshopt_computeMeshletBounds(
        &indirect_vertex_indices[raw_meshlet.vertex_offset],
        &local_triangle_indices[raw_meshlet.triangle_offset],
        raw_meshlet.triangle_count,
        reinterpret_cast<const f32*>(positions.data()),
        vertex_count,
        sizeof(glm::vec3)
      );

      auto meshlet_aabb_center = (meshlet_bb_max + meshlet_bb_min) * 0.5f;
      auto meshlet_aabb_extent = meshlet_bb_max - meshlet_bb_min;

      meshlet.indirect_vertex_index_offset = raw_meshlet.vertex_offset;
      meshlet.local_triangle_index_offset = raw_meshlet.triangle_offset;
      meshlet.vertex_count = raw_meshlet.vertex_count;
      meshlet.triangle_count = raw_meshlet.triangle_count;

      bounds.aabb_center.x = meshopt_quantizeHalf(meshlet_aabb_center.x);
      bounds.aabb_center.y = meshopt_quantizeHalf(meshlet_aabb_center.y);
      bounds.aabb_center.z = meshopt_quantizeHalf(meshlet_aabb_center.z);

      bounds.aabb_extent.x = meshopt_quantizeHalf(meshlet_aabb_extent.x);
      bounds.aabb_extent.y = meshopt_quantizeHalf(meshlet_aabb_extent.y);
      bounds.aabb_extent.z = meshopt_quantizeHalf(meshlet_aabb_extent.z);

      bounds.cone_axis_xy = {meshlet_bounds.cone_axis_s8[0], meshlet_bounds.cone_axis_s8[1]};
      bounds.cone_axis_z = meshlet_bounds.cone_axis_s8[2];
      bounds.cone_cutoff = meshlet_bounds.cone_cutoff_s8;

      mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
      mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
    }

    if (lod_index == 0) {
      const auto center = (mesh_bb_max + mesh_bb_min) * 0.5f;
      const auto extent = mesh_bb_max - mesh_bb_min;
      mesh.bounds_center[0] = center.x;
      mesh.bounds_center[1] = center.y;
      mesh.bounds_center[2] = center.z;
      mesh.bounds_extent[0] = extent.x;
      mesh.bounds_extent[1] = extent.y;
      mesh.bounds_extent[2] = extent.z;
    }

    cur_lod.indices = blob_append(mesh.blob, simplified_indices, 8);
    cur_lod.meshlets = blob_append(mesh.blob, meshlets, 8);
    cur_lod.meshlet_bounds = blob_append(mesh.blob, gpu_meshlet_bounds, 8);
    cur_lod.local_triangle_indices = blob_append(mesh.blob, local_triangle_indices, 8);
    cur_lod.indirect_vertex_indices = blob_append(mesh.blob, indirect_vertex_indices, 4);

    cur_lod.indices_count = static_cast<u32>(simplified_indices.size());
    cur_lod.meshlet_count = static_cast<u32>(meshlet_count);
    cur_lod.meshlet_bounds_count = static_cast<u32>(gpu_meshlet_bounds.size());
    cur_lod.local_triangle_indices_count = static_cast<u32>(local_triangle_indices.size());
    cur_lod.indirect_vertex_indices_count = static_cast<u32>(indirect_vertex_indices.size());

    mesh.lod_count += 1;
  }

  if (mesh.lod_count == 0) {
    return nullopt;
  }

  mesh.lod_metadata_offset = ox::align_up(mesh.blob.size(), 8);
  mesh.blob.resize(mesh.lod_metadata_offset + mesh.lod_count * sizeof(GPU::MeshLOD));

  return mesh;
}

auto read_gltf(Session& session, const std::filesystem::path& path) -> option<fastgltf::Asset> {
  ZoneScoped;

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  if (fastgltf::determineGltfFileType(gltf_buffer.get()) == fastgltf::GltfType::Invalid) {
    session.push_error(fmt::format("'{}' is not a glTF file.", path));
    return nullopt;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    session.push_error(fmt::format("Failed to parse '{}': {}", path, fastgltf::getErrorMessage(gltf_result.error())));
    return nullopt;
  }

  return std::move(gltf_result.get());
}

// Pulls the encoded bytes of an embedded image out of whichever glTF source it lives in.
auto gltf_image_bytes(const fastgltf::Asset& asset, const fastgltf::Image& image) -> std::span<const u8> {
  auto bytes = std::span<const u8>{};

  std::visit(
    ox::match{
      [](const auto&) {},
      [&](const fastgltf::sources::BufferView& v) {
        const auto& buffer_view = asset.bufferViews[v.bufferViewIndex];
        const auto& buffer = asset.buffers[buffer_view.bufferIndex];
        std::visit(
          ox::match{
            [](const auto&) {},
            [&](const fastgltf::sources::Array& array) {
              bytes = std::span(
                reinterpret_cast<const u8*>(array.bytes.data() + buffer_view.byteOffset),
                buffer_view.byteLength
              );
            },
          },
          buffer.data
        );
      },
      [&](const fastgltf::sources::Array& v) {
        bytes = std::span(reinterpret_cast<const u8*>(v.bytes.data()), v.bytes.size_bytes());
      },
    },
    image.data
  );

  return bytes;
}

// writes only the two slots it is handed, so a whole model's textures can be decoded at once
auto compile_gltf_texture(
  Session& session,
  const fastgltf::Asset& asset,
  usize texture_index,
  bool is_srgb,
  std::string_view model_name,
  ModelData::Texture& entry,
  CompiledTexture& compiled
) -> void {
  ZoneScoped;

  const auto& gltf_texture = asset.textures[texture_index];
  entry.is_srgb = is_srgb;

  auto image_index = get_effective_image_index(gltf_texture);
  if (!image_index.has_value()) {
    entry.name = fmt::format("{}_texture_{}", model_name, texture_index);
    return;
  }

  const auto& image = asset.images[image_index.value()];
  entry.name = image.name.empty() ? fmt::format("{}_texture_{}", model_name, texture_index) : std::string(image.name);

  if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
    compiled.kind = CompiledTexture::Kind::External;
    compiled.external_path = uri->uri.fspath();
    return;
  }

  const auto bytes = gltf_image_bytes(asset, image);
  if (bytes.empty()) {
    session.push_error(fmt::format("Unsupported image source for texture '{}'.", entry.name));
    return;
  }

  auto data = compile_texture(session, bytes, entry.name, is_srgb);
  if (!data.has_value()) {
    return;
  }

  compiled.kind = CompiledTexture::Kind::Compiled;
  compiled.data = std::move(data.value());
}

auto compile_gltf_lights(const fastgltf::Asset& asset, ModelData& model) -> void {
  model.lights.reserve(asset.lights.size());
  for (const auto& gltf_light : asset.lights) {
    auto& light = model.lights.emplace_back();
    light.name = std::string(gltf_light.name);
    light.type = static_cast<ModelLightType>(gltf_light.type);
    light.color[0] = gltf_light.color.x();
    light.color[1] = gltf_light.color.y();
    light.color[2] = gltf_light.color.z();
    light.intensity = gltf_light.intensity;
    light.has_range = gltf_light.range.has_value();
    light.range = gltf_light.range.value_or(0.0f);
    light.has_inner_cone_angle = gltf_light.innerConeAngle.has_value();
    light.inner_cone_angle = gltf_light.innerConeAngle.value_or(0.0f);
    light.has_outer_cone_angle = gltf_light.outerConeAngle.has_value();
    light.outer_cone_angle = gltf_light.outerConeAngle.value_or(0.0f);
  }
}

struct PendingMesh {
  usize gltf_mesh_index = 0;
  usize gltf_primitive_index = 0;
};

auto flatten_gltf_nodes(
  Session& session,
  const fastgltf::Asset& asset,
  const ankerl::unordered_dense::set<usize>& joint_nodes,
  ModelData& model
) -> std::vector<PendingMesh> {
  ZoneScoped;

  auto pending_meshes = std::vector<PendingMesh>();

  const auto& gltf_default_scene = asset.scenes[asset.defaultScene.value_or(0_sz)];
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

  while (!processing_gltf_nodes.empty()) {
    auto [gltf_node_index, parent_mesh_group_index] = processing_gltf_nodes.front();
    const auto& node = asset.nodes[gltf_node_index];
    processing_gltf_nodes.pop();

    // joints live in the Skeleton, not the scene graph, because an entity per joint would make
    // hundreds of them per character whose transforms would fight the pose every frame
    if (joint_nodes.contains(gltf_node_index)) {
      continue;
    }

    const auto mesh_group_index = static_cast<u32>(model.mesh_groups.size());
    model.mesh_groups[parent_mesh_group_index].child_indices.push_back(mesh_group_index);

    auto& mesh_group = model.mesh_groups.emplace_back();
    mesh_group.name = node.name;

    for (auto child_node_index : node.children) {
      processing_gltf_nodes.push({child_node_index, mesh_group_index});
    }

    auto translation = glm::vec3{};
    auto rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    auto scale = glm::vec3{1.0f, 1.0f, 1.0f};
    if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
      translation = glm::make_vec3(trs->translation.data());
      rotation = glm::quat::wxyz(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
      scale = glm::make_vec3(trs->scale.data());
    } else if (const auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
      auto transform_mat = glm::make_mat4x4(mat->data());
      auto skew = glm::vec3{};
      auto perspective = glm::vec4{};
      glm::decompose(transform_mat, scale, rotation, translation, skew, perspective);
    }

    mesh_group.translation[0] = translation.x;
    mesh_group.translation[1] = translation.y;
    mesh_group.translation[2] = translation.z;
    mesh_group.rotation[0] = rotation.x;
    mesh_group.rotation[1] = rotation.y;
    mesh_group.rotation[2] = rotation.z;
    mesh_group.rotation[3] = rotation.w;
    mesh_group.scale[0] = scale.x;
    mesh_group.scale[1] = scale.y;
    mesh_group.scale[2] = scale.z;

    if (node.lightIndex.has_value()) {
      mesh_group.light_indices.push_back(static_cast<u32>(node.lightIndex.value()));
    }

    if (!node.meshIndex.has_value()) {
      continue;
    }

    const auto gltf_mesh_index = node.meshIndex.value();
    const auto& gltf_mesh = asset.meshes[gltf_mesh_index];
    for (const auto& [gltf_primitive, gltf_primitive_index] :
         std::views::zip(gltf_mesh.primitives, std::views::iota(0_sz))) {
      // dropping a primitive silently is how a whole model goes missing with nothing in the log
      // to say why, so both of these report
      if (gltf_primitive.findAttribute("POSITION") == gltf_primitive.attributes.end()) {
        session.push_message(
          fmt::format("Primitive {} of mesh '{}' has no POSITION attribute.", gltf_primitive_index, gltf_mesh.name)
        );
        continue;
      }

      // meshlets are triangle lists; strips, fans, lines and points have nothing to build from
      if (gltf_primitive.type != fastgltf::PrimitiveType::Triangles) {
        session.push_message(
          fmt::format("Primitive {} of mesh '{}' is not a triangle list.", gltf_primitive_index, gltf_mesh.name)
        );
        continue;
      }

      mesh_group.mesh_indices.push_back(static_cast<u32>(pending_meshes.size()));
      pending_meshes.push_back({gltf_mesh_index, gltf_primitive_index});
    }
  }

  return pending_meshes;
}

auto read_gltf_primitive(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive, std::string name)
  -> MeshSource {
  ZoneScoped;

  auto source = MeshSource{};
  source.name = std::move(name);

  const auto position_attrib = primitive.findAttribute("POSITION");
  if (position_attrib == primitive.attributes.end()) {
    return source;
  }

  {
    const auto& accessor = asset.accessors[position_attrib->accessorIndex];
    source.positions.resize(accessor.count);
    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor, [&](glm::vec3 pos, usize i) {
      source.positions[i] = pos;
    });
  }

  if (primitive.indicesAccessor.has_value()) {
    const auto& index_accessor = asset.accessors[primitive.indicesAccessor.value()];
    source.indices.resize(index_accessor.count);
    fastgltf::iterateAccessorWithIndex<u32>(asset, index_accessor, [&](u32 index, usize i) {
      source.indices[i] = index;
    });
  } else {
    // glTF allows a non-indexed triangle soup but the rest of this pipeline does not, and a
    // sequential index buffer costs one pass while letting meshopt, the meshlet builder and the
    // BLAS work unchanged, leaving vertices unwelded so such a mesh simply gets no LOD chain
    source.indices.resize(source.positions.size());
    std::iota(source.indices.begin(), source.indices.end(), 0_u32);
  }

  if (auto attrib = primitive.findAttribute("NORMAL"); attrib != primitive.attributes.end()) {
    const auto& accessor = asset.accessors[attrib->accessorIndex];
    source.normals.resize(accessor.count);
    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor, [&](glm::vec3 normal, usize i) {
      source.normals[i] = normal;
    });
  }

  if (auto attrib = primitive.findAttribute("TEXCOORD_0"); attrib != primitive.attributes.end()) {
    const auto& accessor = asset.accessors[attrib->accessorIndex];
    source.texcoords.resize(accessor.count);
    fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, accessor, [&](glm::vec2 uv, usize i) {
      source.texcoords[i] = uv;
    });
  }

  const auto joints_attrib = primitive.findAttribute("JOINTS_0");
  const auto weights_attrib = primitive.findAttribute("WEIGHTS_0");
  if (joints_attrib != primitive.attributes.end() && weights_attrib != primitive.attributes.end()) {
    const auto& joints_accessor = asset.accessors[joints_attrib->accessorIndex];
    source.joints.resize(joints_accessor.count);
    fastgltf::iterateAccessorWithIndex<glm::u16vec4>(asset, joints_accessor, [&](glm::u16vec4 joint, usize i) {
      source.joints[i] = joint;
    });

    const auto& weights_accessor = asset.accessors[weights_attrib->accessorIndex];
    source.weights.resize(weights_accessor.count);
    fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, weights_accessor, [&](glm::vec4 weight, usize i) {
      source.weights[i] = weight;
    });
  }

  return source;
}

// writes only the slot it is handed, so a whole model's primitives can be built at once
auto build_pending_mesh(
  Session& session,
  const fastgltf::Asset& asset,
  const PendingMesh& pending,
  const SkinBinding& skin,
  ModelData::Mesh& out
) -> void {
  ZoneScoped;

  const auto& gltf_mesh = asset.meshes[pending.gltf_mesh_index];
  const auto& gltf_primitive = gltf_mesh.primitives[pending.gltf_primitive_index];

  auto name = fmt::format("{}_{}", std::string(gltf_mesh.name), pending.gltf_primitive_index);
  auto built = build_mesh(read_gltf_primitive(asset, gltf_primitive, name), skin);
  if (!built.has_value()) {
    session.push_message(fmt::format("Primitive '{}' produced no renderable geometry.", name));
    // The slot has to stay so `MeshGroup::mesh_indices` keeps pointing at the right mesh.
    built = ModelData::Mesh{.name = std::move(name)};
  }

  if (gltf_primitive.materialIndex.has_value()) {
    built->material_index = static_cast<u32>(gltf_primitive.materialIndex.value());
  }

  out = std::move(built.value());
}

auto compile_model(Session& session, const ModelCompileRequest& request) -> option<ModelCompileResult> {
  ZoneScoped;

  auto gltf_asset = read_gltf(session, request.path);
  if (!gltf_asset.has_value()) {
    return nullopt;
  }

  const auto& asset = gltf_asset.value();
  if (asset.scenes.size() != 1) {
    session.push_error(fmt::format("'{}' must contain exactly one scene.", request.path));
    return nullopt;
  }

  auto result = ModelCompileResult{};
  auto& model = result.model;
  model.name = request.name.empty() ? request.path.filename().string() : request.name;
  model.default_scene_index = static_cast<u32>(asset.defaultScene.value_or(0_sz));

  model.materials.reserve(asset.materials.size());
  for (const auto& gltf_material : asset.materials) {
    model.materials.push_back(gltf_material_to_material(gltf_material, asset.textures.size()));
  }

  compile_gltf_lights(asset, model);

  // a clip names the nodes it drives, so more than one skin makes "which skeleton does this clip
  // target" ambiguous, mirroring the single-scene restriction above
  if (asset.skins.size() > 1) {
    session.push_error(fmt::format("'{}' has {} skins; only one is supported.", request.path, asset.skins.size()));
    return nullopt;
  }

  auto skin = option<SkinBuildData>(nullopt);
  auto joint_nodes = ankerl::unordered_dense::set<usize>();
  if (!asset.skins.empty()) {
    skin = build_gltf_skeleton(session, asset, asset.skins[0]);
    if (!skin.has_value()) {
      return nullopt;
    }

    joint_nodes.insert(skin->bone_to_node.begin(), skin->bone_to_node.end());
    model.skeleton = to_model_skeleton(skin->skeleton);
  }

  const auto skin_binding = skin.has_value() ? SkinBinding{skin->joint_to_bone, skin->skeleton.inverse_bind_pose}
                                             : SkinBinding{};

  // has to stay serial: it walks the node graph breadth-first and grows `model.mesh_groups`
  const auto pending_meshes = flatten_gltf_nodes(session, asset, joint_nodes, model);

  const auto linear_texture_indices = extract_linear_texture_indices(asset);

  // sized up front and filled by index, so the jobs below never touch a growing vector
  model.textures.resize(asset.textures.size());
  result.textures.resize(asset.textures.size());
  model.meshes.resize(pending_meshes.size());
  // one slot per glTF animation, so a clip that resamples to nothing leaves a `frame_count == 0`
  // hole rather than shifting every UUID the sidecar has recorded after it
  model.animations.resize(skin.has_value() ? asset.animations.size() : 0_sz);

  {
    // texture decodes and mesh builds only read the parsed asset, so they share one barrier and
    // balance a texture-heavy model against a primitive-heavy one
    auto scope = ParallelScope(session->job_manager);

    for (auto texture_index = 0_sz; texture_index < asset.textures.size(); texture_index++) {
      const auto is_srgb = !linear_texture_indices.contains(texture_index);
      scope.dispatch([&session, &asset, &model, &result, texture_index, is_srgb] {
        compile_gltf_texture(
          session,
          asset,
          texture_index,
          is_srgb,
          model.name,
          model.textures[texture_index],
          result.textures[texture_index]
        );
      });
    }

    for (auto mesh_index = 0_sz; mesh_index < pending_meshes.size(); mesh_index++) {
      scope.dispatch([&session, &asset, &model, &pending_meshes, &skin_binding, mesh_index] {
        build_pending_mesh(session, asset, pending_meshes[mesh_index], skin_binding, model.meshes[mesh_index]);
      });
    }

    for (auto animation_index = 0_sz; animation_index < model.animations.size(); animation_index++) {
      scope.dispatch([&asset, &model, &skin, animation_index] {
        auto clip = build_gltf_animation(asset, asset.animations[animation_index], skin.value());
        if (clip.has_value()) {
          model.animations[animation_index] = std::move(clip.value());
        }
      });
    }
  }

  for (const auto& mesh : model.meshes) {
    model.skeleton.max_bone_influence_radius = glm::max(
      model.skeleton.max_bone_influence_radius,
      mesh.max_bone_influence_radius
    );
  }

  return result;
}

auto compile_procedural_mesh(Session& session, const ProceduralMeshRequest& request) -> option<ModelData> {
  ZoneScoped;

  if (request.vertices.empty() || request.indices.empty()) {
    session.push_error("Cannot compile a procedural mesh from empty vertices or indices.");
    return nullopt;
  }

  auto source = MeshSource{};
  source.name = request.name;
  source.indices = request.indices;
  source.positions.reserve(request.vertices.size());
  source.normals.reserve(request.vertices.size());
  source.texcoords.reserve(request.vertices.size());
  for (const auto& vertex : request.vertices) {
    source.positions.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
    source.normals.emplace_back(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
    source.texcoords.emplace_back(vertex.uv[0], vertex.uv[1]);
  }

  auto mesh = build_mesh(source);
  if (!mesh.has_value()) {
    session.push_error(fmt::format("Procedural mesh '{}' produced no renderable geometry.", request.name));
    return nullopt;
  }

  auto model = ModelData{};
  model.name = request.name;
  model.meshes.push_back(std::move(mesh.value()));

  auto& root_group = model.mesh_groups.emplace_back();
  root_group.name = "Root";
  root_group.mesh_indices.push_back(0);

  return model;
}
} // namespace ox::rc
