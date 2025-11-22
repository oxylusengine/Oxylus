#include "Scripting/LuaAssetManagerBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto AssetManagerBinding::bind(sol::state* state) -> void {
  auto uuid_type = state->new_usertype<UUID>("UUID");

  SET_TYPE_FUNCTION(uuid_type, UUID, str);

  auto asset_manager = state->new_usertype<AssetManager>("AssetManager");

  asset_manager.set_function("import_asset", [](AssetManager* am, const std::string& path) {
    return am->import_asset(path);
  });
  SET_TYPE_FUNCTION(asset_manager, AssetManager, load_asset);
  SET_TYPE_FUNCTION(asset_manager, AssetManager, unload_asset);

  asset_manager.set_function("load_asset", [](AssetManager* am, const UUID& uuid) { return am->load_asset(uuid); });
  asset_manager.set_function("get_model", [](AssetManager* am, const UUID& uuid) { return am->get_model(uuid); });
  asset_manager.set_function("get_material", [](AssetManager* am, const UUID& uuid) { return am->get_material(uuid); });
  asset_manager.set_function("get_mut_material", [](AssetManager* am, const UUID& uuid) {
    am->set_material_dirty(uuid);
    return am->get_material(uuid);
  });
  asset_manager.set_function("set_material_dirty", [](AssetManager* am, const UUID& uuid) {
    am->set_material_dirty(uuid);
  });

  state->new_enum(
    "SamplingMode",

    "LinearRepeated",
    SamplingMode::LinearRepeated,
    "LinearClamped",
    SamplingMode::LinearClamped,
    "NearestRepeated",
    SamplingMode::NearestRepeated,
    "NearestClamped",
    SamplingMode::NearestClamped
  );

  auto model = state->new_usertype<Model>("Model", "materials", &Model::materials);
  auto material = state->new_usertype<Material>(
    "Material",

    "albedo_color",
    &Material::albedo_color,
    "set_albedo_color",
    [](Material* mat, glm::vec4 v) { mat->albedo_color = v; },

    "emissive_color",
    &Material::emissive_color,
    "set_emissive_color",
    [](Material* mat, glm::vec4 v) { mat->emissive_color = v; },

    "sampling_mode",
    &Material::sampling_mode,
    "set_sampling_mode",
    [](Material* mat, u32 sampling_mode) { mat->sampling_mode = static_cast<SamplingMode>(sampling_mode); },

    "albedo_texture",
    &Material::albedo_texture,
    "set_albedo_texture",
    [](Material* mat, const UUID& albedo_texture) { mat->albedo_texture = albedo_texture; }
  );
}
} // namespace ox
