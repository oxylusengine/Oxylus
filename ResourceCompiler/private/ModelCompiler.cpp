#include "ModelCompiler.hpp"

#include <ankerl/unordered_dense.h>
#include <cassert>
#include <cstring>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fmt/std.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <queue>

#include "Asset/Model.hpp"
#include "Core/Types.hpp"
#include "Scene/SceneGPU.hpp"
#include "TextureCompiler.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};

namespace ox::rc {
namespace {
using Mesh = ModelData::Mesh;
using Material = ModelData::Material;
using Light = ModelData::Light;
using MeshGroup = ModelData::MeshGroup;
using Meshlet = ModelData::Meshlet;
using MeshletBounds = ModelData::MeshletBounds;
using MeshLOD = ModelData::MeshLOD;

auto default_gltf_extensions() -> fastgltf::Extensions {
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

auto default_gltf_options() -> fastgltf::Options {
  auto options = fastgltf::Options::None;
  options |= fastgltf::Options::LoadExternalBuffers;
  return options;
}

auto effective_image_index(const fastgltf::Texture& texture) -> option<usize> {
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

auto compile_gltf_image(
  const fastgltf::Asset& asset,
  const fastgltf::Image& image,
  const std::filesystem::path& gltf_dir,
  const std::string& name,
  bool srgb
) -> option<TextureData> {
  return std::visit(
    ox::match{
      [&](const fastgltf::sources::URI& src) -> option<TextureData> {
        auto req = TextureCompileRequest{
          .path = gltf_dir / src.uri.fspath(),
          .name = name,
          .srgb = srgb,
        };
        return TextureCompiler::compile(req);
      },
      [&](const fastgltf::sources::Array& src) -> option<TextureData> {
        const auto* base = reinterpret_cast<const u8*>(src.bytes.data());
        auto req = TextureCompileRequest{
          .source_bytes = std::vector<u8>(base, base + src.bytes.size()),
          .name = name,
          .srgb = srgb,
        };
        return TextureCompiler::compile(req);
      },
      [&](const fastgltf::sources::BufferView& src) -> option<TextureData> {
        const auto& buffer_view = asset.bufferViews[src.bufferViewIndex];
        const auto& buffer = asset.buffers[buffer_view.bufferIndex];

        return std::visit(
          ox::match{
            [&](const fastgltf::sources::Array& buf) -> option<TextureData> {
              const auto* base = reinterpret_cast<const u8*>(buf.bytes.data()) + buffer_view.byteOffset;
              auto req = TextureCompileRequest{
                .source_bytes = std::vector<u8>(base, base + buffer_view.byteLength),
                .name = name,
                .srgb = srgb,
              };
              return TextureCompiler::compile(req);
            },
            [&](const auto&) -> option<TextureData> { return nullopt; },
          },
          buffer.data
        );
      },
      [&](const auto&) -> option<TextureData> { return nullopt; },
    },
    image.data
  );
}

auto compile_primitive(const fastgltf::Asset& gltf_asset, const fastgltf::Primitive& gltf_primitive) -> option<Mesh> {
  if (!gltf_primitive.indicesAccessor.has_value()) {
    return nullopt;
  }

  auto& index_accessor = gltf_asset.accessors[gltf_primitive.indicesAccessor.value()];
  auto raw_indices = std::vector<u32>(index_accessor.count);
  fastgltf::iterateAccessorWithIndex<u32>(gltf_asset, index_accessor, [&](u32 index, usize i) {
    raw_indices[i] = index;
  });

  auto vertex_count = 0_u32;
  auto vertex_remap = std::vector<u32>();
  auto positions = std::vector<glm::vec3>();

  auto attrib = gltf_primitive.findAttribute("POSITION");
  if (attrib == gltf_primitive.attributes.end()) {
    return nullopt;
  }

  {
    auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
    auto raw_positions = std::vector<glm::vec3>(accessor.count);
    vertex_remap.resize(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 pos, usize i) {
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
  if (auto n_attrib = gltf_primitive.findAttribute("NORMAL"); n_attrib != gltf_primitive.attributes.end()) {
    auto& accessor = gltf_asset.accessors[n_attrib->accessorIndex];
    auto raw_normals = std::vector<glm::vec3>(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 normal, usize i) {
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
  if (auto uv_attrib = gltf_primitive.findAttribute("TEXCOORD_0"); uv_attrib != gltf_primitive.attributes.end()) {
    auto& accessor = gltf_asset.accessors[uv_attrib->accessorIndex];
    auto raw_texcoords = std::vector<glm::vec2>(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf_asset, accessor, [&](glm::vec2 uv, usize i) {
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

  auto result = Mesh{};

  result.quantized_positions.resize(static_cast<usize>(vertex_count) * sizeof(glm::u16vec4));
  auto* qpos = reinterpret_cast<glm::u16vec4*>(result.quantized_positions.data());
  for (auto i = 0_u32; i < vertex_count; i++) {
    qpos[i] = {
      meshopt_quantizeHalf(positions[i].x),
      meshopt_quantizeHalf(positions[i].y),
      meshopt_quantizeHalf(positions[i].z),
      0,
    };
  }

  result.quantized_normals.resize(static_cast<usize>(vertex_count) * sizeof(u32));
  auto* qnorm = reinterpret_cast<u32*>(result.quantized_normals.data());
  for (auto i = 0_u32; i < vertex_count; i++) {
    const auto& normal = i < normals.size() ? normals[i] : glm::vec3(0.f, 1.f, 0.f);
    qnorm[i] = ((meshopt_quantizeSnorm(normal.x, 10) + 511) << 20) |
               ((meshopt_quantizeSnorm(normal.y, 10) + 511) << 10) | (meshopt_quantizeSnorm(normal.z, 10) + 511);
  }

  result.quantized_texcoords.resize(static_cast<usize>(vertex_count) * sizeof(glm::u16vec2));
  auto* quv = reinterpret_cast<glm::u16vec2*>(result.quantized_texcoords.data());
  for (auto i = 0_u32; i < vertex_count; i++) {
    const auto& uv = i < texcoords.size() ? texcoords[i] : glm::vec2(0.f, 0.f);
    quv[i] = {meshopt_quantizeHalf(uv.x), meshopt_quantizeHalf(uv.y)};
  }

  auto last_lod_indices = std::vector<u32>();
  for (auto lod_index = 0_u32; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
    auto simplified_indices = std::vector<u32>();
    auto lod_error = 0.f;

    if (lod_index == 0) {
      simplified_indices = indices;
    } else {
      const auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
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

      lod_error = result.lods.back().error + result_error;
      if (
        result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 || result_index_count < 6
      ) {
        break;
      }

      simplified_indices.resize(result_index_count);
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
      0.0f
    );

    if (meshlet_count == 0) {
      break;
    }

    raw_meshlets.resize(meshlet_count);
    const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
    indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
    local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32));

    auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
    auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());

    auto lod = MeshLOD{.error = lod_error};
    auto gpu_meshlets = std::vector<Meshlet>(meshlet_count);
    auto gpu_meshlet_bounds = std::vector<MeshletBounds>(meshlet_count);

    for (auto mi = 0_u32; mi < meshlet_count; mi++) {
      const auto& raw_meshlet = raw_meshlets[mi];

      auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
      auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
      for (u32 i = 0; i < raw_meshlet.triangle_count * 3; i++) {
        auto local_triangle_index = local_triangle_indices[raw_meshlet.triangle_offset + i];
        auto indirect_vertex_index = indirect_vertex_indices[raw_meshlet.vertex_offset + local_triangle_index];
        assert(indirect_vertex_index < vertex_count);

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

      gpu_meshlets[mi] = Meshlet{
        .indirect_vertex_index_offset = raw_meshlet.vertex_offset,
        .local_triangle_index_offset = raw_meshlet.triangle_offset,
        .vertex_count = raw_meshlet.vertex_count,
        .triangle_count = raw_meshlet.triangle_count,
      };

      gpu_meshlet_bounds[mi] = MeshletBounds{
        .aabb_center =
          {meshopt_quantizeHalf(meshlet_aabb_center.x),
           meshopt_quantizeHalf(meshlet_aabb_center.y),
           meshopt_quantizeHalf(meshlet_aabb_center.z)},
        .aabb_extent =
          {meshopt_quantizeHalf(meshlet_aabb_extent.x),
           meshopt_quantizeHalf(meshlet_aabb_extent.y),
           meshopt_quantizeHalf(meshlet_aabb_extent.z)},
        .cone_axis_xy = {meshlet_bounds.cone_axis_s8[0], meshlet_bounds.cone_axis_s8[1]},
        .cone_axis_z = meshlet_bounds.cone_axis_s8[2],
        .cone_cutoff = meshlet_bounds.cone_cutoff_s8,
      };

      mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
      mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
    }

    if (lod_index == 0) {
      auto center = (mesh_bb_max + mesh_bb_min) * 0.5f;
      auto extent = mesh_bb_max - mesh_bb_min;
      result.bounds_center[0] = center.x;
      result.bounds_center[1] = center.y;
      result.bounds_center[2] = center.z;
      result.bounds_extent[0] = extent.x;
      result.bounds_extent[1] = extent.y;
      result.bounds_extent[2] = extent.z;
    }

    lod.indices.resize(ox::size_bytes(simplified_indices));
    std::memcpy(lod.indices.data(), simplified_indices.data(), lod.indices.size());

    lod.meshlets.resize(ox::size_bytes(gpu_meshlets));
    std::memcpy(lod.meshlets.data(), gpu_meshlets.data(), lod.meshlets.size());

    lod.meshlet_bounds.resize(ox::size_bytes(gpu_meshlet_bounds));
    std::memcpy(lod.meshlet_bounds.data(), gpu_meshlet_bounds.data(), lod.meshlet_bounds.size());

    lod.indirect_vertex_indices.resize(ox::size_bytes(indirect_vertex_indices));
    std::memcpy(lod.indirect_vertex_indices.data(), indirect_vertex_indices.data(), lod.indirect_vertex_indices.size());

    lod.local_triangle_indices = std::move(local_triangle_indices);

    result.lods.push_back(std::move(lod));
  }

  return result;
}
} // namespace

auto ModelCompiler::compile(const ModelCompileRequest& info, Session& session) -> option<ModelData> {
  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(info.path);
  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    session.push_error(fmt::format("'{}' is not a valid glTF/GLB file.", info.path));
    return nullopt;
  }

  auto gltf_parser = fastgltf::Parser(default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), info.path.parent_path(), default_gltf_options());
  if (!gltf_result) {
    session.push_error(
      fmt::format("Failed to parse '{}': {}", info.path, fastgltf::getErrorMessage(gltf_result.error()))
    );
    return nullopt;
  }

  const auto& gltf_asset = gltf_result.get();
  const auto gltf_dir = info.path.parent_path();

  auto model = ModelData{.name = info.name.empty() ? info.path.stem().string() : info.name};

  auto image_is_srgb = std::vector<bool>(gltf_asset.images.size(), false);
  for (const auto& material : gltf_asset.materials) {
    auto mark_srgb = [&](const fastgltf::TextureInfo& tex_info) {
      if (tex_info.textureIndex >= gltf_asset.textures.size())
        return;
      if (auto image_index = effective_image_index(gltf_asset.textures[tex_info.textureIndex]))
        image_is_srgb[image_index.value()] = true;
    };
    if (material.pbrData.baseColorTexture.has_value())
      mark_srgb(material.pbrData.baseColorTexture.value());
    if (material.emissiveTexture.has_value())
      mark_srgb(material.emissiveTexture.value());
  }

  auto image_to_local_texture = ankerl::unordered_dense::map<usize, u32>{};
  // texture_index (gltf) -> local index into model.textures, for resolving
  // material texture slots below.
  auto texture_to_local = std::vector<option<u32>>(gltf_asset.textures.size(), nullopt);

  for (auto texture_index = 0_u32; texture_index < gltf_asset.textures.size(); texture_index++) {
    auto image_index = effective_image_index(gltf_asset.textures[texture_index]);
    if (!image_index.has_value())
      continue;

    if (auto it = image_to_local_texture.find(image_index.value()); it != image_to_local_texture.end()) {
      texture_to_local[texture_index] = it->second;
      continue;
    }

    const auto& image = gltf_asset.images[image_index.value()];
    auto texture_name = image.name.empty() ? fmt::format("{}_tex{}", model.name, texture_index)
                                           : std::string(image.name);
    auto compiled = compile_gltf_image(gltf_asset, image, gltf_dir, texture_name, image_is_srgb[image_index.value()]);
    if (!compiled.has_value()) {
      session.push_error(fmt::format("'{}': failed to compile image {}.", info.path, image_index.value()));
      continue; // don't fail the whole model over one bad texture
    }

    auto local_index = static_cast<u32>(model.textures.size());
    model.textures.push_back(std::move(compiled.value()));
    image_to_local_texture.emplace(image_index.value(), local_index);
    texture_to_local[texture_index] = local_index;
  }

  auto resolve = [&](const auto& tex_info) -> option<u32> {
    if (!tex_info.has_value() || tex_info->textureIndex >= texture_to_local.size())
      return nullopt;
    return texture_to_local[tex_info->textureIndex];
  };

  model.materials.reserve(gltf_asset.materials.size());
  for (const auto& gltf_material : gltf_asset.materials) {
    auto material = Material{
      .name = std::string(gltf_material.name),
      .albedo_color =
        {gltf_material.pbrData.baseColorFactor.x(),
         gltf_material.pbrData.baseColorFactor.y(),
         gltf_material.pbrData.baseColorFactor.z(),
         gltf_material.pbrData.baseColorFactor.w()},
      .emissive_color =
        {gltf_material.emissiveFactor.x() * gltf_material.emissiveStrength,
         gltf_material.emissiveFactor.y() * gltf_material.emissiveStrength,
         gltf_material.emissiveFactor.z() * gltf_material.emissiveStrength},
      .roughness_factor = gltf_material.pbrData.roughnessFactor,
      .metallic_factor = gltf_material.pbrData.metallicFactor,
      .alpha_cutoff = gltf_material.alphaCutoff,
      .alpha_mode = static_cast<u32>(gltf_material.alphaMode),
      .albedo_texture_index = resolve(gltf_material.pbrData.baseColorTexture),
      .normal_texture_index = resolve(gltf_material.normalTexture),
      .emissive_texture_index = resolve(gltf_material.emissiveTexture),
      .metallic_roughness_texture_index = resolve(gltf_material.pbrData.metallicRoughnessTexture),
      .occlusion_texture_index = resolve(gltf_material.occlusionTexture),
    };

    if (gltf_material.pbrData.baseColorTexture.has_value() && gltf_material.pbrData.baseColorTexture->transform) {
      const auto& transform = *gltf_material.pbrData.baseColorTexture->transform;
      material.uv_offset[0] = transform.uvOffset[0];
      material.uv_offset[1] = transform.uvOffset[1];
      material.uv_scale[0] = transform.uvScale[0];
      material.uv_scale[1] = transform.uvScale[1];
    }

    model.materials.push_back(std::move(material));
  }

  model.lights.reserve(gltf_asset.lights.size());
  for (const auto& gltf_light : gltf_asset.lights) {
    model.lights.push_back(
      Light{
        .name = std::string(gltf_light.name),
        .type = static_cast<u32>(gltf_light.type),
        .color = {gltf_light.color.x(), gltf_light.color.y(), gltf_light.color.z()},
        .intensity = gltf_light.intensity,
        .range = gltf_light.range ? option<f32>(*gltf_light.range) : nullopt,
        .inner_cone_angle = gltf_light.innerConeAngle ? option<f32>(*gltf_light.innerConeAngle) : nullopt,
        .outer_cone_angle = gltf_light.outerConeAngle ? option<f32>(*gltf_light.outerConeAngle) : nullopt,
      }
    );
  }

  if (!gltf_asset.scenes.empty()) {
    const auto& gltf_scene = gltf_asset.scenes[gltf_asset.defaultScene.value_or(0_sz)];

    struct ProcessingNode {
      usize gltf_node_index = 0;
      u32 parent_mesh_group_index = 0;
    };
    auto processing_nodes = std::queue<ProcessingNode>();

    auto& root = model.mesh_groups.emplace_back();
    root.name = std::string(gltf_scene.name);
    for (auto node_index : gltf_scene.nodeIndices) {
      processing_nodes.push({node_index, 0});
    }

    while (!processing_nodes.empty()) {
      auto [gltf_node_index, parent_mesh_group_index] = processing_nodes.front();
      processing_nodes.pop();

      const auto& node = gltf_asset.nodes[gltf_node_index];
      auto mesh_group_index = static_cast<u32>(model.mesh_groups.size());
      model.mesh_groups[parent_mesh_group_index].child_indices.push_back(mesh_group_index);

      auto& mesh_group = model.mesh_groups.emplace_back();
      mesh_group.name = std::string(node.name);

      for (auto child_node_index : node.children) {
        processing_nodes.push({child_node_index, mesh_group_index});
      }

      auto translation = glm::vec3{};
      auto rotation = glm::quat(1.f, 0.f, 0.f, 0.f);
      auto scale = glm::vec3{1.f, 1.f, 1.f};
      if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
        translation = glm::make_vec3(trs->translation.data());
        rotation = glm::quat(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
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

      const auto& gltf_mesh = gltf_asset.meshes[node.meshIndex.value()];
      for (const auto& gltf_primitive : gltf_mesh.primitives) {
        auto mesh = compile_primitive(gltf_asset, gltf_primitive);
        if (!mesh.has_value()) {
          session.push_error(
            fmt::format(
              "'{}': skipped a primitive in mesh '{}' (no indices/positions).",
              info.path,
              std::string(gltf_mesh.name)
            )
          );
          continue;
        }

        mesh->name = std::string(gltf_mesh.name);
        if (gltf_primitive.materialIndex.has_value())
          mesh->material_index = gltf_primitive.materialIndex.value();

        mesh_group.mesh_indices.push_back(static_cast<u32>(model.meshes.size()));
        model.meshes.push_back(std::move(mesh.value()));
      }
    }
  }

  return model;
}

} // namespace ox::rc
