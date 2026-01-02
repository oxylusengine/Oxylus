#pragma once

#include <slang-com-ptr.h>
#include <slang.h>

#include "Common.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ShaderSession {
  Session rc_session = {};
  std::string name = {};
  slang::ISession* slang_session = nullptr;
  std::filesystem::path root_dir = {};

  std::shared_mutex cached_modules_mutex = {};
  ankerl::unordered_dense::map<std::filesystem::path, slang::IModule*> cached_modules = {};

  auto compile_shader(this ShaderSession& self, const ShaderInfo& info) -> option<CompileResult<ShaderAssetEntry>>;
};

} // namespace ox::rc
