#include "Scripting/LuaAssetManagerBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto AssetManagerBinding::bind(sol::state* state) -> void {
  auto uuid = state->new_usertype<UUID>("UUID");

  SET_TYPE_FUNCTION(uuid, UUID, str);

  auto asset_manager = state->new_usertype<AssetManager>("AssetManager");

  SET_TYPE_FUNCTION(asset_manager, AssetManager, import_asset);
  SET_TYPE_FUNCTION(asset_manager, AssetManager, load_asset);
  SET_TYPE_FUNCTION(asset_manager, AssetManager, unload_asset);

  asset_manager.set_function("get_model", [](AssetManager* am, const UUID& uuid) { return am->get_mesh(uuid); });
  asset_manager.set_function("get_material", [](AssetManager* am, const UUID& uuid) { return am->get_material(uuid); });
  asset_manager.set_function("get_mut_material", [](AssetManager* am, const UUID& uuid) {
    am->set_material_dirty(uuid);
    return am->get_material(uuid);
  });
  asset_manager.set_function("set_material_dirty",
                             [](AssetManager* am, const UUID& uuid) { am->set_material_dirty(uuid); });

  auto mesh = state->new_usertype<Mesh>("Model", "materials", &Mesh::materials);
  auto material = state->new_usertype<Material>(
      "Material",

      "albedo_color",
      &Material::albedo_color,
      "set_albedo_color",
      [](Material* mat, glm::vec4 v) { mat->albedo_color = v; },

      "emissive_color",
      &Material::emissive_color,
      "set_emissive_color",
      [](Material* mat, glm::vec4 v) { mat->emissive_color = v; });
}
} // namespace ox
