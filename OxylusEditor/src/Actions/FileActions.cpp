#include "Actions/FileActions.hpp"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <fstream>
#include <system_error>
#include <tracy/Tracy.hpp>

#include "Actions/SceneActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Asset/ParticleSystem.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "OS/OS.hpp"
#include "Panels/CinematicEditorPanel.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/ParticleEditorPanel.hpp"
#include "Utils/ExternalEditor.hpp"
#ifdef OX_DOTNET
  #include "Scripting/DotNetProject.hpp"
#endif

namespace ox {
static auto update_selected_path(
  Editor& editor, const std::filesystem::path& old_path, const std::filesystem::path& new_path
) -> void {
  auto& editor_context = editor.get_context();
  if (editor_context.type != EditorContext::Type::File || !editor_context.str.has_value()) {
    return;
  }

  if (const auto relocated_path = remap_path(std::filesystem::path(*editor_context.str), old_path, new_path)) {
    editor_context.reset(EditorContext::Type::File, relocated_path->string());
  }
}

static auto is_in_directory(const std::filesystem::path& path, const std::filesystem::path& root) -> bool {
  const auto normalized_root = root.lexically_normal();
  const auto normalized_path = path.lexically_normal();
  if (normalized_path == normalized_root) {
    return true;
  }

  const auto relative_path = normalized_path.lexically_relative(normalized_root);
  if (relative_path.empty() || relative_path.is_absolute()) {
    return false;
  }

  return std::ranges::none_of(relative_path, [](const auto& component) { return component == ".."; });
}

static auto asset_directory_of(Editor&) -> std::expected<std::filesystem::path, std::string> {
  const auto& vfs = App::get_vfs();
  if (!vfs.is_mounted_dir(VFS::ASSETS_DIR)) {
    return std::unexpected("no project is open");
  }

  return vfs.resolve_physical_dir(VFS::ASSETS_DIR, "");
}

// a visible sidecar describes its sibling, so moving or renaming it has to carry that sibling along,
// otherwise the asset would lose its stable UUID on the next import
static auto content_entry_source(const std::filesystem::path& path, std::error_code& error) -> std::filesystem::path {
  if (std::filesystem::is_directory(path, error) || error || path.extension() != ".oxasset") {
    return path;
  }

  auto companion_path = path;
  companion_path.replace_extension();
  if (std::filesystem::exists(companion_path, error) && !error) {
    return companion_path;
  }

  return path;
}

// on case-insensitive filesystems a case-only rename finds its own source at the destination
static auto destination_taken(
  const std::filesystem::path& source_path, const std::filesystem::path& destination_path, std::error_code& error
) -> bool {
  return std::filesystem::exists(destination_path, error) &&
         !std::filesystem::equivalent(source_path, destination_path, error);
}

static auto relocate_content_entry(
  Editor& editor,
  const std::filesystem::path& source_path,
  const std::filesystem::path& destination_path,
  bool is_directory
) -> std::expected<void, std::string> {
  ZoneScoped;

  std::error_code error;
  if (destination_taken(source_path, destination_path, error) || error) {
    return std::unexpected(fmt::format("{} already exists or can't be inspected.", destination_path.filename()));
  }

  const auto source_meta_path = is_directory ? std::filesystem::path{} : meta_file_path(source_path);
  const bool has_meta_file = !source_meta_path.empty() && source_meta_path != source_path &&
                             std::filesystem::exists(source_meta_path, error) && !error;
  if (error) {
    return std::unexpected(fmt::format("Couldn't inspect sidecar for {}: {}", source_path, error.message()));
  }

  const auto destination_meta_path = has_meta_file ? meta_file_path(destination_path) : std::filesystem::path{};
  if (has_meta_file && (destination_taken(source_meta_path, destination_meta_path, error) || error)) {
    return std::unexpected(
      fmt::format("Sidecar {} already exists or can't be inspected.", destination_meta_path.filename())
    );
  }

  std::filesystem::rename(source_path, destination_path, error);
  if (error) {
    return std::unexpected(error.message());
  }

  if (has_meta_file) {
    std::filesystem::rename(source_meta_path, destination_meta_path, error);
    if (error) {
      const auto sidecar_error = error;
      std::error_code rollback_error;
      std::filesystem::rename(destination_path, source_path, rollback_error);
      return std::unexpected(
        fmt::format(
          "Couldn't move sidecar {} to {}: {}{}",
          source_meta_path,
          destination_meta_path,
          sidecar_error.message(),
          rollback_error ? fmt::format(" (and couldn't restore {}: {})", source_path, rollback_error.message()) : ""
        )
      );
    }
  }

  auto& asset_man = App::mod<AssetManager>();
  relocate_asset_paths(asset_man, source_path, destination_path);

  // standalone assets consist only of an `.oxasset`, their registered path omits that extension
  if (!is_directory && source_path.extension() == ".oxasset" && source_meta_path == source_path) {
    auto source_asset_path = source_path;
    source_asset_path.replace_extension();
    auto destination_asset_path = destination_path;
    destination_asset_path.replace_extension();
    relocate_asset_paths(asset_man, source_asset_path, destination_asset_path);
  }

  update_selected_path(editor, source_path, destination_path);
  if (has_meta_file) {
    update_selected_path(editor, source_meta_path, destination_meta_path);
  }

  return {};
}

auto is_in_asset_directory(const std::filesystem::path& path) -> bool {
  const auto& vfs = App::get_vfs();
  return vfs.is_mounted_dir(VFS::ASSETS_DIR) && is_in_directory(path, vfs.resolve_physical_dir(VFS::ASSETS_DIR, ""));
}

auto entry_name_error(const std::string_view name) -> std::string_view {
  if (name.empty()) {
    return "Enter a name.";
  }

  if (name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos) {
    return "Names cannot contain path separators.";
  }

  return {};
}

auto move_content_entry(Editor& editor, const std::filesystem::path& from, const std::filesystem::path& to_dir)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  const auto normalized_from = from.lexically_normal();
  const auto normalized_to_dir = to_dir.lexically_normal();

  std::error_code error;
  if (
    !is_in_directory(normalized_from, *assets_directory) || !is_in_directory(normalized_to_dir, *assets_directory) ||
    !std::filesystem::exists(normalized_from, error) || error ||
    !std::filesystem::is_directory(normalized_to_dir, error) || error
  ) {
    return std::unexpected(
      fmt::format(
        "Couldn't move {} into {}: source or target is outside the asset directory or unavailable.",
        from,
        to_dir
      )
    );
  }

  const auto source_path = content_entry_source(normalized_from, error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't inspect {} before moving it: {}", from, error.message()));
  }

  const bool is_directory = std::filesystem::is_directory(source_path, error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't inspect {} before moving it: {}", source_path, error.message()));
  }

  if (source_path.parent_path().lexically_normal() == normalized_to_dir) {
    return source_path;
  }

  // moving a directory into itself (or any child) would create an invalid recursive move
  if (is_directory && remap_path(normalized_to_dir, source_path, source_path)) {
    return std::unexpected(fmt::format("Can't move directory {} into itself.", source_path));
  }

  const auto destination_path = normalized_to_dir / source_path.filename();
  if (const auto moved = relocate_content_entry(editor, source_path, destination_path, is_directory); !moved) {
    return std::unexpected(fmt::format("Couldn't move {} into {}: {}", source_path, normalized_to_dir, moved.error()));
  }

  return destination_path;
}

auto rename_content_entry(Editor& editor, const std::filesystem::path& path, const std::string_view new_name)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  const auto normalized_requested_path = path.lexically_normal();
  if (!is_in_directory(normalized_requested_path, *assets_directory)) {
    return std::unexpected(fmt::format("{} is outside the asset directory.", path));
  }

  const bool is_requested_meta_file = normalized_requested_path.extension() == ".oxasset";

  // whatever sits behind an `.oxasset` stays one, whether or not the typed name keeps the extension
  auto base_name = new_name;
  if (is_requested_meta_file && base_name.ends_with(".oxasset")) {
    base_name.remove_suffix(std::string_view(".oxasset").size());
  }

  if (const auto name_error = entry_name_error(base_name); !name_error.empty()) {
    return std::unexpected(std::string(name_error));
  }

  std::error_code error;
  const auto source_path = content_entry_source(normalized_requested_path, error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't inspect {}: {}", path, error.message()));
  }

  const bool is_directory = std::filesystem::is_directory(source_path, error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't inspect {}: {}", source_path, error.message()));
  }

  auto destination_path = source_path.parent_path() / base_name;
  if (is_requested_meta_file && source_path == normalized_requested_path) {
    destination_path += ".oxasset";
  }

  if (destination_path == source_path) {
    return source_path;
  }

  if (const auto renamed = relocate_content_entry(editor, source_path, destination_path, is_directory); !renamed) {
    return std::unexpected(renamed.error());
  }

  return destination_path;
}

auto delete_content_entry(Editor& editor, const std::filesystem::path& path, const bool is_forced)
  -> std::expected<DeletedEntry, std::string> {
  ZoneScoped;

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  const auto normalized_path = path.lexically_normal();
  if (!is_in_directory(normalized_path, *assets_directory) || normalized_path == assets_directory->lexically_normal()) {
    return std::unexpected(fmt::format("{} is not inside the asset directory.", path));
  }

  std::error_code error;
  const auto source_path = content_entry_source(normalized_path, error);
  if (error || !std::filesystem::exists(source_path, error)) {
    return std::unexpected(fmt::format("{} does not exist.", path));
  }

  const bool is_directory = std::filesystem::is_directory(source_path, error);

  // a standalone asset is registered without its `.oxasset`
  auto roots = std::vector<std::filesystem::path>{source_path};
  if (!is_directory && source_path.extension() == ".oxasset") {
    auto asset_path = source_path;
    asset_path.replace_extension();
    roots.push_back(std::move(asset_path));
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto& vfs = App::get_vfs();
  auto doomed = std::vector<Asset>{};
  for (const auto& asset : asset_man.get_registry_snapshot()) {
    const auto candidates = std::array<std::filesystem::path, 3>{
      vfs.to_physical(asset.source_path),
      vfs.to_physical(asset.path),
      asset_source(asset.uuid).path,
    };
    const auto is_doomed = std::ranges::any_of(roots, [&candidates](const auto& root) {
      return std::ranges::any_of(candidates, [&root](const auto& candidate) {
        return remap_path(candidate, root, root).has_value();
      });
    });
    if (is_doomed) {
      doomed.push_back(asset);
    }
  }

  if (!is_forced) {
    auto blockers = std::string{};
    usize blocker_count = 0;
    for (const auto& asset : doomed) {
      if (asset.ref_count == 0) {
        continue;
      }

      blocker_count += 1;
      if (blocker_count <= 10) {
        blockers += fmt::format(
          "\n  {} {} {} refs={}",
          asset.uuid.str(),
          AssetManager::to_asset_type_sv(asset.type),
          asset.source_path.generic_string(),
          asset.ref_count
        );
      }
    }

    if (blocker_count > 0) {
      return std::unexpected(
        fmt::format(
          "{} asset(s) under {} are still referenced, scenes or other assets that use them would lose them. Unload "
          "or remove the users first, or delete again with force{}",
          blocker_count,
          path.generic_string(),
          blockers
        )
      );
    }
  }

  const auto source_meta_path = is_directory ? std::filesystem::path{} : meta_file_path(source_path);
  std::filesystem::remove_all(source_path, error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't delete {}: {}", source_path, error.message()));
  }

  if (!source_meta_path.empty() && source_meta_path != source_path) {
    std::filesystem::remove(source_meta_path, error);
    if (error) {
      return std::unexpected(fmt::format("Deleted {} but not its sidecar: {}", source_path, error.message()));
    }
  }

  // a model gives up its materials and textures when it is deleted first, so the parents go ahead of their children
  const auto delete_order = [](const AssetType type) {
    return type == AssetType::Model ? 0 : type == AssetType::Material || type == AssetType::ParticleSystem ? 1 : 2;
  };
  std::ranges::stable_sort(doomed, [&delete_order](const Asset& lhs, const Asset& rhs) {
    return delete_order(lhs.type) < delete_order(rhs.type);
  });

  // whatever still holds a ref keeps a registered asset, deleting it would strand those holders on a freed slot
  auto result = DeletedEntry{.path = source_path};
  for (const auto& asset : doomed) {
    auto live_refs = 0_u64;
    {
      const auto live = asset_man.get_asset(asset.uuid);
      if (!live) {
        continue;
      }

      live_refs = live->ref_count;
    }

    if (live_refs != 0) {
      result.kept.push_back(asset.uuid);
      continue;
    }

    asset_man.delete_asset(asset.uuid);
    result.unregistered.push_back(asset.uuid);
  }

  if (!result.kept.empty()) {
    OX_LOG_WARN(
      "Deleted {} but {} asset(s) from it are still in use and stay registered until the editor restarts",
      source_path,
      result.kept.size()
    );
  }

  auto& editor_context = editor.get_context();
  if (editor_context.type == EditorContext::Type::File && editor_context.str.has_value()) {
    if (remap_path(std::filesystem::path(*editor_context.str), source_path, source_path)) {
      editor_context.reset();
    }
  }

  return result;
}

auto create_folder(Editor& editor, const std::filesystem::path& parent, const std::string_view name)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  if (const auto name_error = entry_name_error(name); !name_error.empty()) {
    return std::unexpected(std::string(name_error));
  }

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  if (!is_in_directory(parent, *assets_directory)) {
    return std::unexpected(fmt::format("{} is outside the asset directory.", parent));
  }

  const auto new_folder_path = parent / name;
  std::error_code error;
  if (std::filesystem::create_directory(new_folder_path, error)) {
    editor.get_context().reset(EditorContext::Type::File, new_folder_path.string());
    return new_folder_path;
  }

  if (error) {
    return std::unexpected(fmt::format("Couldn't create folder: {}", error.message()));
  }

  return std::unexpected("A file or folder with that name already exists.");
}

auto create_asset_file(Editor& editor, const std::filesystem::path& dir, const AssetType type, std::string_view name)
  -> std::expected<CreatedAsset, std::string> {
  ZoneScoped;

  if (type != AssetType::Material && type != AssetType::ParticleSystem && type != AssetType::Cinematic) {
    return std::unexpected(
      fmt::format(
        "Can't create a {} asset file, materials, particle systems and cinematics can be created",
        AssetManager::to_asset_type_sv(type)
      )
    );
  }

  if (const auto name_error = entry_name_error(name); !name_error.empty()) {
    return std::unexpected(std::string(name_error));
  }

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  std::error_code error;
  if (!is_in_directory(dir, *assets_directory) || !std::filesystem::is_directory(dir, error)) {
    return std::unexpected(fmt::format("{} is not a folder inside the asset directory.", dir));
  }

  auto asset_path = dir / name;
  if (asset_path.extension() == ".oxasset") {
    asset_path.replace_extension("");
  }

  if (type == AssetType::ParticleSystem && asset_path.extension() != ".oxparticle") {
    asset_path += ".oxparticle";
  }

  if (type == AssetType::Cinematic && asset_path.extension() != ".oxcine") {
    asset_path += ".oxcine";
  }

  if (std::filesystem::exists(asset_path, error) || std::filesystem::exists(meta_file_path(asset_path), error)) {
    return std::unexpected(fmt::format("{} already exists.", asset_path.filename()));
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto type_name = AssetManager::to_asset_type_sv(type);

  if (type == AssetType::ParticleSystem) {
    // loading reads the file, and a new system has nothing on disk yet
    if (!ParticleSystem::make_default().write(asset_path)) {
      return std::unexpected(fmt::format("Couldn't create {} asset {}", type_name, asset_path.filename()));
    }
  }

  const auto uuid = asset_man.create_asset(type, asset_path);
  asset_man.load_asset(uuid);
  if (!export_asset(asset_man, uuid, asset_path)) {
    asset_man.unload_asset(uuid);
    asset_man.delete_asset(uuid);
    if (type == AssetType::ParticleSystem) {
      std::filesystem::remove(asset_path, error);
    }

    return std::unexpected(fmt::format("Couldn't create {} asset {}", type_name, asset_path.filename()));
  }

  // export_asset wrote what a later load reads back, so the creation ref isn't needed
  asset_man.unload_asset(uuid);

  OX_LOG_INFO("Created new {} asset {}", type_name, name);
  return CreatedAsset{.uuid = uuid, .path = asset_path};
}

#ifdef OX_DOTNET
auto create_csharp_script(
  Editor& editor, const std::filesystem::path& dir, CSharpScriptKind kind, std::string_view name
) -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  const auto& scripts_project = editor.active_project->get_config().scripts_project;
  if (scripts_project.empty()) {
    return std::unexpected("the project has no C# scripts project");
  }

  if (const auto name_error = entry_name_error(name); !name_error.empty()) {
    return std::unexpected(std::string(name_error));
  }

  const auto assets_directory = asset_directory_of(editor);
  if (!assets_directory) {
    return std::unexpected(assets_directory.error());
  }

  if (!is_in_directory(dir, *assets_directory)) {
    return std::unexpected(fmt::format("{} is outside the asset directory.", dir));
  }

  const auto class_name = to_csharp_identifier(name);
  const auto script_path = dir / (class_name + ".cs");
  if (std::filesystem::exists(script_path)) {
    return std::unexpected(fmt::format("Couldn't create C# script, {} already exists", script_path.string()));
  }

  auto file = std::ofstream(script_path, std::ios::binary);
  if (!file) {
    return std::unexpected(fmt::format("Couldn't create C# script {}", script_path.string()));
  }

  file << csharp_script_template(kind, scripts_project.stem().string(), class_name);
  file.close();

  OX_LOG_INFO("Created new C# script {}", class_name);
  return script_path;
}
#endif

auto open_file(Editor& editor, const std::filesystem::path& path) -> std::expected<std::string, std::string> {
  ZoneScoped;

  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return std::unexpected(fmt::format("{} does not exist", path.string()));
  }

  switch (classify_file_type(path)) {
    case FileType::Scene: {
      const auto opened = open_scene(editor, path);
      if (!opened) {
        return std::unexpected(opened.error());
      }

      return fmt::format("opened the scene as scene {}", std::to_underlying((*opened)->get_id()));
    }
    case FileType::Shader:
    case FileType::CSharp: {
      open_in_external_editor(path);
      return std::string("opened in the external editor");
    }
    case FileType::ParticleSystem: {
      // `open_asset` loads it and holds the ref for as long as the panel shows it
      const auto uuid = import_asset(App::mod<AssetManager>(), path);
      if (!uuid) {
        return std::unexpected(fmt::format("could not import {}, log_read has the reason", path.string()));
      }

      editor.editor_panel_registry.get<ParticleEditorPanel>().open_asset(uuid);
      return fmt::format("opened {} in the particle editor", uuid.str());
    }
    case FileType::Cinematic: {
      const auto uuid = import_asset(App::mod<AssetManager>(), path);
      if (!uuid) {
        return std::unexpected(fmt::format("could not import {}, log_read has the reason", path.string()));
      }

      editor.editor_panel_registry.get<CinematicEditorPanel>().open_asset(uuid);
      return fmt::format("opened {} in the cinematic editor", uuid.str());
    }
    case FileType::Unknown: {
      if (path.extension().empty()) {
        return std::string("nothing to open for a file without an extension");
      }

      os::open_file_externally(path);
      return std::string("opened with the system's default application");
    }
    default: return std::string("double-clicking this kind of file does nothing");
  }
}
} // namespace ox
