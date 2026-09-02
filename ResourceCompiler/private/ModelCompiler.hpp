#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "ResourceCompiler.hpp"

namespace ox::rc {
// Un-indexed attribute arrays straight out of a source file. `normals` and `texcoords` may be empty;
// everything else is required.
struct MeshSource {
  std::string name = {};
  std::vector<glm::vec3> positions = {};
  std::vector<glm::vec3> normals = {};
  std::vector<glm::vec2> texcoords = {};
  std::vector<u32> indices = {};
};

auto build_mesh(const MeshSource& source) -> option<ModelData::Mesh>;
auto compile_model(Session& session, const ModelCompileRequest& request) -> option<ModelCompileResult>;
auto compile_procedural_mesh(Session& session, const ProceduralMeshRequest& request) -> option<ModelData>;
} // namespace ox::rc
