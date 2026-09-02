#pragma once

#include <ankerl/unordered_dense.h>
#include <mutex>
#include <slang-com-ptr.h>
#include <slang.h>

#include "Asset/AssetFile.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ShaderDiagnostics {
  std::vector<std::string> errors = {};
  std::vector<std::string> messages = {};
};

// shared by every worker compiling one request. slang only allows concurrency in the backend: a
// linked component type can generate code on its own thread, but everything in front of that runs
// under `front_end_mutex`
struct ShaderSession {
  Slang::ComPtr<slang::ISession> slang_session = {};
  std::string name = {};
  std::filesystem::path root_directory = {};

  std::mutex front_end_mutex = {};
  ankerl::unordered_dense::map<std::filesystem::path, slang::IModule*> cached_modules = {};

  auto compile_shader(this ShaderSession& self, const ShaderCompileInfo& info, ShaderDiagnostics& diag)
    -> option<std::vector<ShaderEntryPointData>>;
};

} // namespace ox::rc
