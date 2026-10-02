#include "Scripting/LuaAssetManagerBindings.hpp"

#include <memory>
#include <sol/state.hpp>
#include <string>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
// one reference held on the script's behalf, dropped by `unload()` or when lua collects the handle, so a script that
// forgets to unload can't pin the asset forever
class LuaAssetHandle {
public:
  explicit LuaAssetHandle(const UUID& uuid) : uuid_(uuid) {}
  LuaAssetHandle(const LuaAssetHandle&) = delete;
  auto operator=(const LuaAssetHandle&) -> LuaAssetHandle& = delete;
  ~LuaAssetHandle() { unload(); }

  auto uuid(this const LuaAssetHandle& self) -> UUID { return self.uuid_; }
  auto is_loaded(this const LuaAssetHandle& self) -> bool { return static_cast<bool>(self.uuid_); }

  auto unload(this LuaAssetHandle& self) -> void {
    if (!self.uuid_) {
      return;
    }

    App::mod<AssetManager>().unload_asset(self.uuid_);
    self.uuid_ = UUID(nullptr);
  }

private:
  UUID uuid_ = {};
};

static auto acquire_asset(AssetManager& asset_man, const UUID& uuid) -> std::unique_ptr<LuaAssetHandle> {
  if (!uuid || !asset_man.load_asset(uuid)) {
    return nullptr;
  }

  return std::make_unique<LuaAssetHandle>(uuid);
}

// scripts only ever name game content, so their paths are relative to ASSETS_DIR
static auto find_script_asset(AssetManager& asset_man, std::string_view path) -> UUID {
  const auto uuid = asset_man.find_asset(std::filesystem::path(VFS::ASSETS_DIR) / path);
  if (!uuid) {
    OX_LOG_WARN("No asset was imported from '{}'.", path);
  }

  return uuid;
}

auto AssetManagerBinding::bind(sol::state* state) -> void {
  auto uuid_type = state->new_usertype<UUID>("UUID");

  SET_TYPE_FUNCTION(uuid_type, UUID, str);

  auto asset_manager = state->new_usertype<AssetManager>("AssetManager");

  SET_TYPE_FUNCTION(asset_manager, AssetManager, load_asset);
  SET_TYPE_FUNCTION(asset_manager, AssetManager, unload_asset);

  asset_manager.set_function("load_asset", [](AssetManager* am, const UUID& uuid) { return am->load_asset(uuid); });
  asset_manager.set_function("get_model", [](AssetManager* am, const UUID& uuid) { return am->get_model(uuid); });
  asset_manager.set_function("get_material", [](AssetManager* am, const UUID& uuid) { return am->get_material(uuid); });
  asset_manager.set_function("get_mut_material", [](AssetManager* am, const UUID& uuid) {
    am->set_material_dirty(uuid);
    return am->get_material(uuid).value;
  });
  asset_manager.set_function("set_material_dirty", [](AssetManager* am, const UUID& uuid) {
    am->set_material_dirty(uuid);
  });
  asset_manager.set_function("find_asset", [](AssetManager* am, std::string_view path) -> sol::optional<UUID> {
    const auto uuid = find_script_asset(*am, path);
    return uuid ? sol::optional<UUID>(uuid) : sol::nullopt;
  });
  asset_manager.set_function(
    "acquire",
    sol::overload(
      [](AssetManager* am, std::string_view path) { return acquire_asset(*am, find_script_asset(*am, path)); },
      [](AssetManager* am, const UUID& uuid) { return acquire_asset(*am, uuid); }
    )
  );

  state->new_usertype<LuaAssetHandle>(
    "AssetHandle",
    sol::no_constructor,
    "uuid",
    &LuaAssetHandle::uuid,
    "is_loaded",
    &LuaAssetHandle::is_loaded,
    "unload",
    &LuaAssetHandle::unload
  );

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
