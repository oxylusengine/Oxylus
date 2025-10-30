#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <memory>
#include <tracy/Tracy.hpp>
#include <typeindex>
#include <vuk/Types.hpp>

#include "Core/Option.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
template <typename T>
concept Module = requires(T t) {
  t.init();
  t.deinit();
  { T::MODULE_NAME } -> std::convertible_to<std::string_view>;
};

template <typename T>
concept ModuleHasUpdate = requires(T t, const Timestep& timestep) { t.update(timestep); };

template <typename T>
concept ModuleHasRender = requires(T t, vuk::Extent3D extent, vuk::Format format) { t.render(extent, format); };

struct TypeIndexHash {
  std::size_t operator()(const std::type_index& ti) const noexcept { return ti.hash_code(); }
};

struct ModuleRegistry {
  using ModulePtr = std::unique_ptr<void, void (*)(void*)>;
  using Registry = ankerl::unordered_dense::map<std::type_index, ModulePtr, TypeIndexHash>;

  Registry registry = {};
  std::vector<std::type_index> module_types = {};
  std::vector<std::function<std::expected<void, std::string>()>> init_callbacks = {};
  std::vector<ox::option<std::function<void(const Timestep&)>>> update_callbacks = {};
  std::vector<std::function<std::expected<void, std::string>()>> deinit_callbacks = {};
  std::vector<std::string_view> module_names = {};

  template <Module T, typename... Args>
  auto add(Args&&... args) -> void {
    ZoneScoped;

    auto type_index = std::type_index(typeid(T));
    module_types.emplace_back(type_index);
    auto deleter = [](void* self) {
      delete static_cast<T*>(self);
    };
    auto& module = registry.try_emplace(type_index, ModulePtr(new T(std::forward<Args>(args)...), deleter))
                     .first->second;

    init_callbacks.emplace_back([m = static_cast<T*>(module.get())]() { return m->init(); });
    deinit_callbacks.emplace_back([m = static_cast<T*>(module.get())]() { return m->deinit(); });
    if constexpr (ModuleHasUpdate<T>) {
      update_callbacks.emplace_back([m = static_cast<T*>(module.get())](const Timestep& timestep) {
        m->update(timestep);
      });
    } else {
      update_callbacks.emplace_back(ox::nullopt);
    }

    module_names.emplace_back(T::MODULE_NAME);
  }

  template <typename T>
  auto has() const -> bool {
    ZoneScoped;

    return registry.contains(std::type_index(typeid(T)));
  }

  template <typename T>
  auto get() -> T& {
    ZoneScoped;

    auto it = registry.find(std::type_index(typeid(T)));
    OX_CHECK_NE(it, registry.end());

    return *static_cast<T*>(it->second.get());
  }

  auto init(this ModuleRegistry& self) -> bool;
  auto deinit(this ModuleRegistry& self) -> bool;
  auto update(this ModuleRegistry& self, const Timestep& timestep) -> void;
};
} // namespace ox
