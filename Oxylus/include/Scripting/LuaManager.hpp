#pragma once

#include <sol/state.hpp>

#include "Core/ESystem.hpp"
#include "Scripting/LuaBinding.hpp"

namespace ox {
class LuaManager : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  auto get_state() const -> sol::state* { return _state.get(); }

  template <typename T>
  void bind(this LuaManager& self, const std::string& name, sol::state* state) {
    static_assert(std::is_base_of_v<LuaBinding, T>, "T must derive from LuaBinding");
    auto binding = std::make_unique<T>();
    binding->bind(state);
    self.bindings.emplace(name, std::move(binding));
  }

  template<typename T>
  auto get_binding(this LuaManager& self, const std::string& name) -> T* {
    static_assert(std::is_base_of_v<LuaBinding, T>, "T must derive from LuaBinding");
    return dynamic_cast<T*>(self.bindings[name].get());
  }

private:
  ankerl::unordered_dense::map<std::string, std::unique_ptr<LuaBinding>> bindings = {};
  std::unique_ptr<sol::state> _state = nullptr;

  void bind_log() const;
};
} // namespace ox
