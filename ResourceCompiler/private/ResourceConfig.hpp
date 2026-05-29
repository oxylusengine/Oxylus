#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox::rc {
struct ShaderProgramConfig {
  std::string name = {};
  std::filesystem::path path = {};
  std::vector<std::string> entry_points = {};
  bool bindless = false;
};

struct ShaderSessionConfig {
  std::filesystem::path root_directory = {};
  std::string session_name = {};
  bool debug_symbols = false;
  i32 optimization_level = 3;
  std::filesystem::path output = {};
  std::vector<std::pair<std::string, std::string>> definitions = {};
  std::vector<ShaderProgramConfig> programs = {};
};

struct ModelConfig {
  std::filesystem::path path = {};
  bool is_foliage = false;
};

struct ResourceConfig {
  i32 version = {};
  std::vector<ShaderSessionConfig> shader_sessions = {};
  std::vector<ModelConfig> models = {};
};

auto parse_resource_config(const std::filesystem::path& config_path) -> option<ResourceConfig>;

} // namespace ox::rc
