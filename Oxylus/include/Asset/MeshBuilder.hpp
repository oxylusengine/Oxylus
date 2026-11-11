#pragma once

#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <span>

#include "Animation/Fwd.hpp"
#include "Asset/AssetFile.hpp"
#include "Core/UUID.hpp"

namespace ox {
// what build_mesh reads, borrowed from whoever owns the arrays. `normals` and `texcoords` may be empty
struct MeshView {
  std::string_view name = {};
  std::span<const glm::vec3> positions = {};
  std::span<const glm::vec3> normals = {};
  std::span<const glm::vec2> texcoords = {};
  std::span<const glm::u16vec4> joints = {};
  std::span<const glm::vec4> weights = {};
  std::span<const u32> indices = {};
};

// Un-indexed attribute arrays straight out of a source file. `normals` and `texcoords` may be empty;
// everything else is required.
struct MeshSource {
  std::string name = {};
  std::vector<glm::vec3> positions = {};
  std::vector<glm::vec3> normals = {};
  std::vector<glm::vec2> texcoords = {};
  // glTF joint slots and their weights, both empty on a static mesh
  std::vector<glm::u16vec4> joints = {};
  std::vector<glm::vec4> weights = {};
  std::vector<u32> indices = {};

  operator MeshView() const;
};

// what turns a primitive's glTF joint slots into skeleton bone indices, empty for a static mesh
struct SkinBinding {
  std::span<const u32> joint_to_bone = {};
  std::span<const BoneTransform> inverse_bind_pose = {};
};

// one mesh of a model made at runtime, `material` names a loaded material asset or is null
struct RuntimeMeshSource {
  std::span<const glm::vec3> positions = {};
  std::span<const glm::vec3> normals = {};
  std::span<const u32> indices = {};
  UUID material = UUID(nullptr);
};

// what the model cook turns a primitive into: quantized vertices, meshlets, up to `max_lod_count` LODs and the
// un-quantized LOD0 triangles physics builds shapes from. nullopt for an empty or malformed source
auto build_mesh(const MeshView& source, const SkinBinding& skin = {}, u32 max_lod_count = GPU::Mesh::MAX_LODS)
  -> option<ModelData::Mesh>;

// a model of one mesh group holding every mesh, each with a single LOD. A mesh with no triangles is skipped, nullopt
// when none is left or one is malformed
auto build_runtime_model(std::span<const RuntimeMeshSource> meshes) -> option<ModelData>;
} // namespace ox
