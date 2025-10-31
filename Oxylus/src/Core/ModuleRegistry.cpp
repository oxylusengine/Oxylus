#include "Core/ModuleRegistry.hpp"

#include <ranges>

#include "Utils/Timer.hpp"

namespace ox {
auto ModuleRegistry::init(this ModuleRegistry& self) -> bool {
  ZoneScoped;

  for (const auto& [name, cb] : std::views::zip(self.module_names, self.init_callbacks)) {
    Timer timer{};

    auto result = cb();
    if (!result.has_value()) {
      OX_LOG_ERROR("Module {} failed to initialize! Error: {}", name, result.error());
      return false;
    }

    OX_LOG_INFO("Initialized module {} in {} ms.", name, timer.get_elapsed_ms());
  }

  return true;
}

auto ModuleRegistry::deinit(this ModuleRegistry& self) -> bool {
  ZoneScoped;

  for (const auto& [name, cb, type] : std::views::reverse(std::views::zip(self.module_names, self.deinit_callbacks, self.module_types))) {
    Timer timer{};

    auto result = cb();
    if (!result.has_value()) {
      OX_LOG_ERROR("Module {} failed to deinitialize! Error: {}", name, result.error());
      return false;
    }

    self.registry.erase(type);

    OX_LOG_INFO("Deinitialized module {} in {} ms.", name, timer.get_elapsed_ms());
  }

  return true;
}

auto ModuleRegistry::update(this ModuleRegistry& self, const Timestep& timestep) -> void {
  ZoneScoped;

  for (const auto& cb : self.update_callbacks) {
    if (cb.has_value()) {
      cb.value()(timestep);
    }
  }
}
} // namespace ox
