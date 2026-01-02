#pragma once

#include "Common.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ModelProcessor {
  auto process(const ModelCompileRequest& request) -> option<CompileResult<ModelAssetEntry>>;
};
} // namespace ox::rc
