#include "LuaAssetManagerBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"

namespace ox::LuaBindings {
void bind_asset_manager(const std::shared_ptr<sol::state>& state) { auto asset_table = state->create_table("Assets"); }
} // namespace ox::LuaBindings
