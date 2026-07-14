#pragma once

#include <shared_mutex>
#include <slang-com-ptr.h>
#include <string>

#include "Asset/AssetFile.hpp"
#include "ResourceCompiler.hpp"

namespace ox {
template <>
struct Handle<rc::Session>::Impl {
  u16 version = 1;
  std::shared_mutex messages_mutex = {};
  std::vector<std::string> errors = {};
  std::vector<std::string> messages = {};

  std::shared_mutex session_mutex = {};
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};

  std::vector<rc::ShaderCompileRequest> shader_requests = {};
  std::vector<rc::TextureCompileRequest> texture_requests = {};
  AssetFile asset_file = {};
};
} // namespace ox
