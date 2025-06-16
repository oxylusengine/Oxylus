#pragma once

#include "Project.hpp"

namespace ox {
class ProjectSerializer {
public:
  ProjectSerializer(Project* project_) : project(project_) {}

  bool serialize(const std::string& file_path) const;
  bool deserialize(const std::string& file_path) const;

private:
  Project* project;
};
} // namespace ox
