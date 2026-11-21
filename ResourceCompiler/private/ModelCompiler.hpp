#pragma once

#include "Asset/MeshBuilder.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
auto compile_model(Session& session, const ModelCompileRequest& request) -> option<ModelCompileResult>;
auto compile_procedural_mesh(Session& session, const ProceduralMeshRequest& request) -> option<ModelData>;
} // namespace ox::rc
