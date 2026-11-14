#include "Actions/AssetActions.hpp"

#include <atomic>
#include <fmt/format.h>
#include <simdjson.h>
#include <tracy/Tracy.hpp>

#include "Actions/FileActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Asset/AssetMeta.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
static auto document_temp_path(const UUID& uuid) -> std::filesystem::path {
  static std::atomic<u32> counter = 0;
  return std::filesystem::temp_directory_path() / fmt::format("oxmcp_{}_{}.json", uuid.str(), counter.fetch_add(1));
}

// Components only carry a UUID, and nothing in the reflection says which kind of asset a field
// points at, so an empty field is matched by the name the component gave it. A field that already
// holds something uses the type of what it holds instead.
auto expected_asset_type(const std::string_view field_name) -> AssetType {
  const auto has = [field_name](const std::string_view needle) {
    return field_name.find(needle) != std::string_view::npos;
  };

  if (has("skeleton"))
    return AssetType::Skeleton;
  if (has("clip") || has("animation"))
    return AssetType::Animation;
  if (has("cinematic"))
    return AssetType::Cinematic;
  if (has("model"))
    return AssetType::Model;
  if (has("material"))
    return AssetType::Material;
  if (has("texture") || has("layer_") || has("lut"))
    return AssetType::Texture;
  if (has("audio") || has("sound"))
    return AssetType::Audio;
  if (has("particle"))
    return AssetType::ParticleSystem;
  if (has("terrain"))
    return AssetType::Terrain;
  if (has("scene") || has("prefab"))
    return AssetType::Scene;

  return AssetType::None;
}

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
    return std::unexpected(fmt::format("importing {} failed, log_read has the reason", path.string()));
  }

  return uuid;
}

auto create_asset(Editor& editor, const AssetType type, const std::filesystem::path& path)
  -> std::expected<CreatedAsset, std::string> {
  ZoneScoped;

  return create_asset_file(editor, path.parent_path(), type, path.filename().string());
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

  const auto is_saveable = type == AssetType::Material || type == AssetType::ParticleSystem ||
                           type == AssetType::Cinematic || type == AssetType::Scene || type == AssetType::Terrain;
  if (!is_saveable) {
    return std::unexpected(
      fmt::format(
        "{} assets are not saved from the editor, only materials, particle systems, cinematics, scenes and terrain "
        "are",
        AssetManager::to_asset_type_sv(type)
      )
    );
  }

  if (!has_source || path.extension() == ".oxpack") {
    return std::unexpected("the asset has no file of its own, it is embedded in another asset");
  }

  if (!is_loaded) {
    return std::unexpected("the asset is not loaded, so there is nothing to save, asset_load it first");
  }

  if (!export_asset(asset_man, uuid, path)) {
    return std::unexpected(fmt::format("saving {} failed, log_read has the reason", path.string()));
  }

  if (type == AssetType::Material) {
    editor.thumbnail_manager.invalidate_material(uuid);
  }

  return path;
}

auto material_json(const UUID& uuid, const Material& material) -> std::string {
  ZoneScoped;

  JsonWriter writer{};
  writer.is_pretty = false;
  rc::write_material_asset_meta(writer, uuid, material);

  return writer.stream.str();
}

auto update_material(Editor& editor, const UUID& uuid, const Material& material) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.is_loaded(uuid)) {
    return std::unexpected("the material is not loaded, asset_load it first");
  }

  if (!asset_man.set_material(uuid, material)) {
    return std::unexpected("the material could not be updated, one of its textures does not load");
  }

  editor.thumbnail_manager.invalidate_material(uuid);

  return {};
}

auto asset_document(const UUID& uuid) -> std::expected<std::string, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto type = AssetType::None;
  {
    const auto asset = asset_man.get_asset(uuid);
    if (!asset) {
      return std::unexpected(fmt::format("{} is not a registered asset", uuid.str()));
    }

    type = asset->type;
  }

  if (type != AssetType::ParticleSystem && type != AssetType::Cinematic) {
    return std::unexpected("only particle systems and cinematics have a document");
  }

  const auto temp_path = document_temp_path(uuid);
  auto is_written = false;
  if (type == AssetType::ParticleSystem) {
    const auto system = asset_man.get_particle_system(uuid);
    is_written = system && system->write(temp_path);
  } else {
    const auto cinematic = asset_man.get_cinematic(uuid);
    is_written = cinematic && cinematic->write(temp_path);
  }

  if (!is_written) {
    return std::unexpected("the asset is not loaded, asset_load it first");
  }

  const auto text = File::to_string(temp_path);
  std::error_code error;
  std::filesystem::remove(temp_path, error);

  simdjson::dom::parser parser;
  simdjson::dom::element element;
  if (const auto parsed = parser.parse(text).get(element); parsed) {
    return std::unexpected(fmt::format("the document could not be read back: {}", simdjson::error_message(parsed)));
  }

  return simdjson::minify(element);
}

auto set_asset_document(Editor&, const UUID& uuid, const std::string_view document)
  -> std::expected<void, std::string> {
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
    return std::unexpected("the asset is not loaded, asset_load it first");
  }

  simdjson::dom::parser parser;
  simdjson::dom::element element;
  if (const auto parsed = parser.parse(document).get(element); parsed) {
    return std::unexpected(fmt::format("the document is not valid json: {}", simdjson::error_message(parsed)));
  }

  if (!element.is_object()) {
    return std::unexpected("the document must be a json object");
  }

  // the parsers read from files and log what they dislike
  const auto temp_path = document_temp_path(uuid);
  {
    auto file = File(temp_path, FileAccess::Write);
    if (!file) {
      return std::unexpected(fmt::format("could not write {}", temp_path.string()));
    }

    file.write(document);
    file.close();
  }

  OX_DEFER(&) {
    std::error_code error;
    std::filesystem::remove(temp_path, error);
  };

  if (type == AssetType::ParticleSystem) {
    auto parsed = ParticleSystem::read(temp_path);
    if (!parsed) {
      return std::unexpected("the particle system parser rejected the document, log_read has the reason");
    }

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

  auto parsed = Cinematic::read(temp_path);
  if (!parsed) {
    return std::unexpected("the cinematic parser rejected the document, log_read has the reason");
  }

  asset_man.edit_cinematic(uuid, [&parsed](Cinematic& cinematic) { cinematic = std::move(*parsed); });

  return {};
}
} // namespace ox
