#include "ModuleUtil.hpp"

#include "Core/App.hpp"
#include "ModuleInterface.hpp"
#include "ModuleRegistry.hpp"

namespace ox {
void ModuleUtil::load_module(const std::string& name, const std::string& path) {
  const auto lib = App::get_system<ModuleRegistry>(EngineSystems::ModuleRegistry)->add_lib(name, path);
  if (!lib)
    return;

  auto app_instance = App::get();
  auto* imgui_context = ImGui::GetCurrentContext();
  lib->interface->init(app_instance, imgui_context);
}

void ModuleUtil::unload_module(const std::string& module_name) {
  const auto lib = App::get_system<ModuleRegistry>(EngineSystems::ModuleRegistry)->get_lib(module_name);
  if (!lib)
    return;
}
} // namespace ox
