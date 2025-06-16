#pragma once

#include <dylib.hpp>

#include "Core/ESystem.hpp"

namespace ox {
class ModuleInterface;

struct Module {
  std::unique_ptr<dylib> lib;
  ModuleInterface* interface;
  std::string path;
};

class ModuleRegistry : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  Module* add_lib(const std::string& name, std::string_view path);
  Module* get_lib(const std::string& name);
  void remove_lib(const std::string& name);
  void clear();

private:
  ankerl::unordered_dense::map<std::string, std::unique_ptr<Module>> libs = {};
  std::vector<std::string> copied_file_paths = {};
};
} // namespace ox
