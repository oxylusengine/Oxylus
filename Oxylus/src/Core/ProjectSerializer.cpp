#include "Core/ProjectSerializer.hpp"

#include <toml++/toml.hpp>

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
bool ProjectSerializer::serialize(const std::filesystem::path& file_path) const {
  const auto& config = project->get_config();

  const auto root = toml::table{{
    "project",
    toml::table{
      {"name", config.name},
      {"asset_directory", config.asset_directory.string()},
      {"start_scene", config.start_scene},
    },
  }};

  std::ofstream file(file_path);
  if (!file.is_open()) {
    return false;
  }

  file << root;
  return true;
}

bool ProjectSerializer::deserialize(const std::filesystem::path& file_path) const {
  auto& [name, start_scene, asset_directory] = project->get_config();
  const auto content = File::to_string(file_path);
  if (content.empty()) {
    OX_ASSERT(!content.empty(), "Couldn't load project file: {0}", file_path);
    return false;
  }

  try {
    toml::table toml = toml::parse(content);

    const auto project_node = toml["project"];
    name = project_node["name"].as_string()->get();
    asset_directory = project_node["asset_directory"].as_string()->get();
    start_scene = project_node["start_scene"].as_string()->get();
  } catch (const std::exception& exc) {
    OX_LOG_ERROR("{}", exc.what());
    return false;
  }

  return true;
}
} // namespace ox
