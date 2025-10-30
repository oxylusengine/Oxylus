#pragma once

#include "Core/Project.hpp"

namespace ox {
class ProjectSerializer {
public:
  ProjectSerializer(Project* project_) : project(project_) {}

  bool serialize(const std::filesystem::path& file_path) const;
  bool deserialize(const std::filesystem::path& file_path) const;

private:
  Project* project;
};
} // namespace ox
