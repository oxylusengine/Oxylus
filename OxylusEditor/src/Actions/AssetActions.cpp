#include "Actions/AssetActions.hpp"

#include <fmt/format.h>
#include <simdjson.h>
#include <tracy/Tracy.hpp>

#include "Actions/FileActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"

namespace ox {
auto import_file(Editor&, const std::filesystem::path& path) -> std::expected<UUID, std::string> {
  ZoneScoped;

  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return std::unexpected(fmt::format("{} is not a file", path.string()));
  }

  if (!is_in_asset_directory(path)) {
    return std::unexpected(fmt::format("{} is outside the asset directory", path.string()));
  }

  if (rc::to_asset_file_type(path) == rc::AssetFileType::None) {
    return std::unexpected(fmt::format("{} is not a file type the importer knows", path.string()));
  }

  const auto uuid = import_asset(App::mod<AssetManager>(), path);
  if (!uuid) {
    return std::unexpected(fmt::format("importing {} failed, see the log", path.string()));
  }

  return uuid;
}

auto save_asset(Editor& editor, const UUID& uuid) -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto type = AssetType::None;
  auto path = std::filesystem::path{};
  auto is_loaded = false;
  auto has_source = false;
  {
    const auto asset = asset_man.get_asset(uuid);
    if (!asset) {
      return std::unexpected(fmt::format("{} is not a registered asset", uuid.str()));
    }

    type = asset->type;
    path = App::get_vfs().to_physical(asset->path);
    is_loaded = asset->is_loaded();
    has_source = !asset->source_path.empty();
  }

  if (!is_exportable(type)) {
    return std::unexpected(
      fmt::format("{} assets can't be saved from the editor", AssetManager::to_asset_type_sv(type))
    );
  }

  if (!has_source || path.extension() == ".oxpack") {
    return std::unexpected("the asset has no file of its own, it is embedded in another asset");
  }

  if (!is_loaded) {
    return std::unexpected("the asset is not loaded, so there is nothing to save");
  }

  if (!export_asset(asset_man, uuid, path)) {
    return std::unexpected(fmt::format("saving {} failed, see the log", path.string()));
  }

  if (type == AssetType::Material) {
    editor.thumbnail_manager.invalidate_material(uuid);
  }

  return path;
}

auto update_material(Editor& editor, const UUID& uuid, const Material& material) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.is_loaded(uuid)) {
    return std::unexpected("the material is not loaded");
  }

  if (!asset_man.set_material(uuid, material)) {
    return std::unexpected("the material could not be updated, one of its textures does not load");
  }

  editor.thumbnail_manager.invalidate_material(uuid);

  return {};
}

auto set_asset_document(const UUID& uuid, const std::string_view document) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto type = AssetType::None;
  auto is_loaded = false;
  {
    const auto asset = asset_man.get_asset(uuid);
    if (!asset) {
      return std::unexpected(fmt::format("{} is not a registered asset", uuid.str()));
    }

    type = asset->type;
    is_loaded = asset->is_loaded();
  }

  if (type != AssetType::ParticleSystem && type != AssetType::Cinematic) {
    return std::unexpected("only particle systems and cinematics have a document");
  }

  if (!is_loaded) {
    return std::unexpected("the asset is not loaded");
  }

  // the readers skip fields they can't read, so a broken document has to be refused before it replaces the asset
  simdjson::dom::parser parser;
  simdjson::dom::element element;
  if (const auto parsed = parser.parse(document.data(), document.size()).get(element); parsed) {
    return std::unexpected(fmt::format("the document is not valid json: {}", simdjson::error_message(parsed)));
  }

  if (!element.is_object()) {
    return std::unexpected("the document must be a json object");
  }

  if (type == AssetType::ParticleSystem) {
    auto parsed = ParticleSystem::from_json(document);
    if (!parsed) {
      return std::unexpected(fmt::format("the particle system document does not parse: {}", parsed.error()));
    }

    // the runtime state (compiled programs, curve atlas) stays, only the authored data is replaced
    asset_man.edit_particle_system(uuid, [&parsed](ParticleSystem& system) {
      system.emitter = parsed->emitter;
      system.render = parsed->render;
      system.emitter_graph = std::move(parsed->emitter_graph);
      system.spawn_graph = std::move(parsed->spawn_graph);
      system.update_graph = std::move(parsed->update_graph);
      system.curves = std::move(parsed->curves);
      system.gradients = std::move(parsed->gradients);
      system.parameters = std::move(parsed->parameters);
    });

    return {};
  }

  auto parsed = Cinematic::from_json(document);
  if (!parsed) {
    return std::unexpected(fmt::format("the cinematic document does not parse: {}", parsed.error()));
  }

  asset_man.edit_cinematic(uuid, [&parsed](Cinematic& cinematic) { cinematic = std::move(*parsed); });

  return {};
}
} // namespace ox
