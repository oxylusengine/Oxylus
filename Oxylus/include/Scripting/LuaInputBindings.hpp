#pragma once

#include "Scripting/LuaBinding.hpp"

namespace ox {
class InputBinding : public LuaBinding {
public:
  auto bind(sol::state* state) -> void override;
};
} // namespace ox

