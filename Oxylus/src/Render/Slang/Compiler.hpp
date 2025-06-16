#pragma once

#include "Core/Handle.hpp"
#include "Core/Option.hpp"

namespace ox {
struct SlangSessionInfo {
  std::vector<std::pair<std::string, std::string>> definitions = {};
  std::string root_directory = {};
};

struct SlangModuleInfo {
  std::string path = {};
  std::string module_name = {};
};

struct SlangEntryPoint {
  std::vector<u32> ir = {};
};

struct ShaderReflection {
  u32 pipeline_layout_index = 0;
  glm::u64vec3 thread_group_size = {};
};

struct SlangSession;
struct SlangModule : Handle<SlangModule> {
  void destroy();

  option<SlangEntryPoint> get_entry_point(std::string_view name);
  ShaderReflection get_reflection();
  SlangSession session();
};

struct SlangSession : Handle<SlangSession> {
  friend SlangModule;

  auto destroy() -> void;

  option<SlangModule> load_module(const SlangModuleInfo& info);
};

struct SlangCompiler : Handle<SlangCompiler> {
  static option<SlangCompiler> create();
  void destroy();

  option<SlangSession> new_session(const SlangSessionInfo& info);
};
} // namespace ox
