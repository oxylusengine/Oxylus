#pragma once

#include <glm/gtx/compatibility.hpp>
#include <vector>

#include "Core/Handle.hpp"
#include "Core/Option.hpp"

namespace ox {
struct SlangSessionInfo {
  std::string name = {};
  i32 optimizaton_level = 3;
  std::vector<std::pair<std::string, std::string>> definitions = {};
  std::filesystem::path root_directory = {};
};

struct SlangShaderInfo {
  std::filesystem::path path = {};
  std::string module_name = {};
  std::vector<std::string> entry_points = {};
};

struct SlangEntryPoint {
  std::vector<u32> code = {};
};

struct SlangSession : Handle<SlangSession> {
  auto destroy() -> void;

  auto compile_shader(const SlangShaderInfo& info) -> option<std::vector<SlangEntryPoint>>;
};

struct SlangCompiler : Handle<SlangCompiler> {
  static option<SlangCompiler> create();
  void destroy();

  option<SlangSession> new_session(const SlangSessionInfo& info);
};
} // namespace ox
