#include "ModuleRegistry.hpp"

#include "ModuleInterface.hpp"

namespace ox {
auto ModuleRegistry::init() -> std::expected<void, std::string> { return {}; }

auto ModuleRegistry::deinit() -> std::expected<void, std::string> {
  clear();
  return {};
}

Module* ModuleRegistry::add_lib(const std::string& name, std::string_view path) {
  try {
    const std::string path_str = std::string(path);
    const auto file_name = path_str + dylib::filename_components::suffix;
    const auto new_file_name = std::string(path) + "_copy" + dylib::filename_components::suffix;
    if (fs::exists(new_file_name)) {
      remove_lib(name);
      fs::remove(new_file_name);
    }
    fs::copy_file(file_name, new_file_name);
    copied_file_paths.emplace_back(new_file_name);

    const auto new_path = path_str + "_copy";

    auto lib = std::make_unique<dylib>(new_path);

    const auto create_func = lib->get_function<ModuleInterface*()>("create_module");
    ModuleInterface* interface = create_func();

    auto module = std::make_unique<Module>(std::move(lib), interface, path_str);
    libs.emplace(name, std::move(module));

    OX_LOG_INFO("Successfully loaded module: {}", name);

    return libs[name].get();
  } catch (const std::exception& exc) {
    OX_LOG_ERROR("{}", exc.what());
    return nullptr;
  }
}

Module* ModuleRegistry::get_lib(const std::string& name) {
  try {
    return libs.at(name).get();
  } catch ([[maybe_unused]]
           const std::exception& exc) {
    OX_LOG_ERROR("Module {} doesn't exists or has not been registered.", name);
    return nullptr;
  }
}

void ModuleRegistry::remove_lib(const std::string& name) { libs.erase(name); }

void ModuleRegistry::clear() {
  for (auto&& [name, module] : libs) {
    delete module->interface;
    module->lib.reset();
  }
  libs.clear();

  for (auto& p : copied_file_paths) {
    fs::remove(p);
  }
  copied_file_paths.clear();
}
} // namespace ox
