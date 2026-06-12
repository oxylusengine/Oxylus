#pragma once

#include "Scripting/LuaBinding.hpp"

namespace ox {
class RMLBinding : public LuaBinding {
public:
  auto bind(sol::state* state) -> void override;
};
} // namespace ox
