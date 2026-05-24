#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <slang-com-ptr.h>
#include <slang.h>

#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ShaderSession {
  Session rc_session = {};
  std::string name = {};
  slang::ISession* slang_session = nullptr;
  std::filesystem::path root_directory = {};

  std::shared_mutex cached_modules_mutex = {};
  ankerl::unordered_dense::map<std::filesystem::path, slang::IModule*> cached_modules = {};

  auto compile_shader(this ShaderSession& self, const ShaderCompileInfo& info)
    -> option<std::vector<ShaderEntryPointData>>;
};

} // namespace ox::rc
