#pragma once

#include "Core/ESystem.hpp"

#include <sol/state.hpp>

namespace ox {
class LuaManager : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  sol::state* get_state() const { return _state.get(); }

private:
  std::unique_ptr<sol::state> _state = nullptr;

  void bind_log() const;
};
} // namespace ox
