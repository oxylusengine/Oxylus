#pragma once

#include "Core/ESystem.hpp"

namespace sol {
class state;
}

namespace ox {
class LuaManager : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  sol::state* get_state() const { return m_state.get(); }

private:
  std::shared_ptr<sol::state> m_state = nullptr;

  void bind_log() const;
};
} // namespace ox
