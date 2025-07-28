#pragma once

#include "Scripting/LuaBinding.hpp"

namespace ox {
class AppBinding : public LuaBinding {
public:
  auto bind(sol::state* state) -> void override;
};
} // namespace ox
