#include "Scripting/LuaAssetManagerBindings.hpp"

#include <sol/state.hpp>

#include "Oxylus.hpp"

// #include "Asset/AssetManager.hpp"

namespace ox::LuaBindings {
void bind_asset_manager(sol::state* state) {
  ZoneScoped;
  auto asset_table = state->create_table("Assets");
}
} // namespace ox::LuaBindings
