#include "Asset/MeshBuilder.hpp"

#include <algorithm>
#include <cstring>
#include <glm/common.hpp>
#include <limits>
#include <meshoptimizer.h>
#include <ranges>

#include "Animation/BoneTransform.hpp"

namespace ox {
static auto blob_append(std::vector<u8>& blob, const void* data, usize size, usize alignment) -> u64 {
  const auto offset = ox::align_up(blob.size(), alignment);
  blob.resize(offset + size);
  if (size != 0) {
    std::memcpy(blob.data() + offset, data, size);
  }

  return offset;
}

template <typename T>
static auto blob_append(std::vector<u8>& blob, const std::vector<T>& data, usize alignment) -> u64 {
  return blob_append(blob, data.data(), ox::size_bytes(data), alignment);
}

MeshSource::operator MeshView() const {
  return MeshView{
    .name = name,
    .positions = positions,
    .normals = normals,
    .texcoords = texcoords,
    .joints = joints,
    .weights = weights,
    .indices = indices,
  };
}

auto build_mesh(const MeshView& source, const SkinBinding& skin, u32 max_lod_count) -> option<ModelData::Mesh> {
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
  const auto lod_limit = glm::min(max_lod_count, GPU::Mesh::MAX_LODS);
  for (auto lod_index = 0_sz; lod_index < lod_limit; lod_index++) {
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

auto build_runtime_model(std::span<const RuntimeMeshSource> meshes) -> option<ModelData> {
  ZoneScoped;

  auto model = ModelData{};
  model.name = "runtime";
  auto& root_group = model.mesh_groups.emplace_back();
  root_group.name = "Root";

  for (const auto& runtime_mesh : meshes) {
    if (runtime_mesh.indices.empty()) {
      continue;
    }

    if (!runtime_mesh.normals.empty() && runtime_mesh.normals.size() != runtime_mesh.positions.size()) {
      return nullopt;
    }

    const auto source = MeshView{
      .positions = runtime_mesh.positions,
      .normals = runtime_mesh.normals,
      .indices = runtime_mesh.indices,
    };
    auto mesh = build_mesh(source, {}, 1);
    if (!mesh.has_value()) {
      return nullopt;
    }

    if (runtime_mesh.material) {
      mesh->material_index = static_cast<u32>(model.materials.size());
      model.materials.push_back({.uuid = PackedUUID::pack(runtime_mesh.material)});
    }

    root_group.mesh_indices.push_back(static_cast<u32>(model.meshes.size()));
    model.meshes.push_back(std::move(mesh.value()));
  }

  if (model.meshes.empty()) {
    return nullopt;
  }

  return model;
}
} // namespace ox
