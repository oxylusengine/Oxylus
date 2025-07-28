#pragma once

#include "Scripting/LuaBinding.hpp"

namespace ox {
class MathBinding : public LuaBinding {
public:
  auto bind(sol::state* state) -> void override;
};
} // namespace ox
