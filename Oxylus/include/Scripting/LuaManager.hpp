#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <sol/state.hpp>

#include "Scripting/LuaBinding.hpp"

namespace ox {
class LuaManager {
public:
  constexpr static auto MODULE_NAME = "LuaManager";

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto get_state() const -> sol::state* { return _state.get(); }

  template <typename T>
  void bind(this LuaManager& self, const std::string& name, sol::state* state) {
    static_assert(std::is_base_of_v<LuaBinding, T>, "T must derive from LuaBinding");
    auto binding = std::make_unique<T>();
    binding->bind(state);
    self.bindings.emplace(name, std::move(binding));
  }

  template <typename T>
  auto get_binding(this LuaManager& self, const std::string& name) -> T* {
    static_assert(std::is_base_of_v<LuaBinding, T>, "T must derive from LuaBinding");
    return dynamic_cast<T*>(self.bindings[name].get());
  }

private:
  ankerl::unordered_dense::map<std::string, std::unique_ptr<LuaBinding>> bindings = {};
  std::unique_ptr<sol::state> _state = nullptr;

  void bind_log() const;
  void bind_vector() const;
};
} // namespace ox
