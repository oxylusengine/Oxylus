#include "Scripting/LuaBinding.hpp"

namespace ox {
class AssetManagerBinding : public LuaBinding {
public:
  auto bind(sol::state* state) -> void override;
};
} // namespace o
