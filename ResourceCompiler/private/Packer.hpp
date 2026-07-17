#pragma once

#include <slang-com-ptr.h>

#include "Asset/AssetFile.hpp"
#include "ResourceCompiler.hpp"

namespace ox {
template <>
struct Handle<rc::Packer>::Impl {
  rc::Session session = {};
  u16 version = 1;

  std::vector<rc::ShaderCompileRequest> shader_requests = {};
  std::vector<rc::TextureCompileRequest> texture_requests = {};
  AssetFile asset_file = {};
};
} // namespace ox
