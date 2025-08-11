#pragma once

namespace sol {
class state;
}

namespace ox {
class LuaBinding {
public:
  virtual ~LuaBinding() = default;

  virtual auto bind(sol::state* state) -> void = 0;
};
} // namespace ox
