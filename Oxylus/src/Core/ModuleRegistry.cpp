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

    self.initialized_count++;
    OX_LOG_INFO("Initialized module {} in {} ms.", name, timer.get_elapsed_ms());
  }

  return true;
}

auto ModuleRegistry::deinit(this ModuleRegistry& self) -> bool {
  ZoneScoped;

  // a failed init leaves the modules after it untouched, so they get no deinit either
  auto initialized = std::views::zip(self.module_names, self.deinit_callbacks, self.module_types) |
                     std::views::take(self.initialized_count);
  for (const auto& [name, cb, type] : std::views::reverse(initialized)) {
    Timer timer{};

    auto result = cb();
    if (!result.has_value()) {
      OX_LOG_ERROR("Module {} failed to deinitialize! Error: {}", name, result.error());
      return false;
    }

    self.registry.erase(type);
    self.initialized_count--;

    OX_LOG_INFO("Deinitialized module {} in {} ms.", name, timer.get_elapsed_ms());
  }

  return true;
}

auto ModuleRegistry::update(this ModuleRegistry& self, const Timestep& timestep) -> void {
  ZoneScoped;

  // init stops at the first failure, so the initialized modules are exactly the first initialized_count
  for (const auto& [module_index, fn] : self.update_callbacks) {
    if (module_index < self.initialized_count) {
      fn(timestep);
    }
  }
}
} // namespace ox
