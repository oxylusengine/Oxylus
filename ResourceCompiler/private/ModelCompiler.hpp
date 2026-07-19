#pragma once

#include "Asset/AssetFile.hpp"
#include "Core/Option.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ModelCompiler {
  static auto compile(const ModelCompileRequest& info, Session& session) -> option<ModelData>;
};

} // namespace ox::rc
