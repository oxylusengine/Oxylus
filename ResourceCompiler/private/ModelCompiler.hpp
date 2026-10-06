#pragma once

#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <span>

#include "Animation/Fwd.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
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
};

// what turns a primitive's glTF joint slots into skeleton bone indices, empty for a static mesh
struct SkinBinding {
  std::span<const u32> joint_to_bone = {};
  std::span<const BoneTransform> inverse_bind_pose = {};
};

auto build_mesh(const MeshSource& source, const SkinBinding& skin = {}) -> option<ModelData::Mesh>;
auto compile_model(Session& session, const ModelCompileRequest& request) -> option<ModelCompileResult>;
auto compile_procedural_mesh(Session& session, const ProceduralMeshRequest& request) -> option<ModelData>;
} // namespace ox::rc
