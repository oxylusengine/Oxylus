#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/FileActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Panels/ContentPanel.hpp"

namespace ox {
constexpr static i64 LIST_DEFAULT_LIMIT = 200;
constexpr static i64 LIST_MAX_LIMIT = 5000;

static auto file_type_name(FileType type) -> std::string_view {
  switch (type) {
    case FileType::Unknown       : return "Unknown";
    case FileType::Directory     : return "Directory";
    case FileType::Meta          : return "Meta";
    case FileType::Scene         : return "Scene";
    case FileType::Prefab        : return "Prefab";
    case FileType::Shader        : return "Shader";
    case FileType::Texture       : return "Texture";
    case FileType::Model         : return "Model";
    case FileType::Audio         : return "Audio";
    case FileType::Material      : return "Material";
    case FileType::Terrain       : return "Terrain";
    case FileType::ParticleSystem: return "ParticleSystem";
    case FileType::Cinematic     : return "Cinematic";
    case FileType::UI            : return "UI";
    case FileType::CSharp        : return "CSharp";
    case FileType::Font          : return "Font";
  }

  return "Unknown";
}

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto is_hidden(const std::filesystem::path& path) -> bool {
  const auto name = path.filename().string();
  return name.starts_with('.');
}

static auto sidecar_note(Editor& editor, const std::filesystem::path& path) -> std::string {
  std::error_code error;
  const auto meta = meta_file_path(path);
  if (meta.empty() || meta == path || !std::filesystem::exists(meta, error)) {
    return {};
  }

  return fmt::format("\nsidecar {}", project_relative_text(editor, meta));
}

static auto file_list(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.opt_str("path");
  const auto recursive_arg = args.opt_boolean("recursive");
  const auto meta_arg = args.opt_boolean("show_meta");
  const auto limit_arg = args.opt_integer("limit");
  const auto cursor_arg = args.opt_str("cursor");
  if (!path_arg || !recursive_arg || !meta_arg || !limit_arg || !cursor_arg) {
    return McpResult::error(
      !path_arg        ? path_arg.error()
      : !recursive_arg ? recursive_arg.error()
      : !meta_arg      ? meta_arg.error()
      : !limit_arg     ? limit_arg.error()
                       : cursor_arg.error()
    );
  }

  const auto& vfs = App::get_vfs();
  if (!vfs.is_mounted_dir(VFS::ASSETS_DIR)) {
    return McpResult::error("no project is open");
  }

  auto root = vfs.resolve_physical_dir(VFS::ASSETS_DIR, "");
  if (path_arg->has_value()) {
    const auto resolved = resolve_content_path(editor, path_arg->value(), true);
    if (!resolved) {
      return McpResult::error(fmt::format("argument 'path': {}", resolved.error()));
    }

    root = *resolved;
  }

  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    return McpResult::error(fmt::format("'{}' is not a folder", project_relative_text(editor, root)));
  }

  usize skip = 0;
  if (cursor_arg->has_value()) {
    const auto text = cursor_arg->value();
    u64 value = 0;
    const auto [end, parse_error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parse_error != std::errc{} || end != text.data() + text.size()) {
      return McpResult::error("argument 'cursor': not a cursor returned by this tool");
    }

    skip = static_cast<usize>(value);
  }

  const auto limit = static_cast<usize>(std::clamp<i64>(limit_arg->value_or(LIST_DEFAULT_LIMIT), 1, LIST_MAX_LIMIT));
  const auto is_showing_meta = meta_arg->value_or(false);
  auto& asset_man = App::mod<AssetManager>();

  struct Row {
    std::string sort_key = {};
    std::string line = {};
  };

  std::vector<Row> rows;
  const auto add_entry = [&](const std::filesystem::directory_entry& entry) {
    std::error_code entry_error;
    const auto is_directory = entry.is_directory(entry_error);
    const auto path_text = project_relative_text(editor, entry.path());
    if (is_directory) {
      rows.push_back({.sort_key = "0" + lower(path_text), .line = path_text + "/"});
      return;
    }

    if (!is_showing_meta && entry.path().extension() == ".oxasset") {
      return;
    }

    const auto size = entry.file_size(entry_error);
    auto line = fmt::format(
      "{} {} {}",
      path_text,
      entry_error ? 0 : size,
      file_type_name(classify_file_type(entry.path()))
    );

    auto asset_path = entry.path();
    if (asset_path.extension() == ".oxasset") {
      asset_path.replace_extension();
    }

    if (const auto uuid = asset_man.find_asset(asset_path); uuid) {
      line += " " + uuid.str();
    }

    rows.push_back({.sort_key = "1" + lower(path_text), .line = std::move(line)});
  };

  if (recursive_arg->value_or(false)) {
    auto it = std::filesystem::recursive_directory_iterator(
      root,
      std::filesystem::directory_options::skip_permission_denied,
      error
    );
    for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
      if (is_hidden(it->path())) {
        it.disable_recursion_pending();
        continue;
      }

      add_entry(*it);
    }
  } else {
    auto it = std::filesystem::directory_iterator(
      root,
      std::filesystem::directory_options::skip_permission_denied,
      error
    );
    for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
      if (!is_hidden(it->path())) {
        add_entry(*it);
      }
    }
  }

  std::ranges::sort(rows, [](const Row& lhs, const Row& rhs) { return lhs.sort_key < rhs.sort_key; });

  const auto first = std::min(skip, rows.size());
  const auto last = std::min(first + limit, rows.size());
  auto out = fmt::format(
    "{} entries {}-{} of {}\n",
    project_relative_text(editor, root),
    rows.empty() ? 0 : first + 1,
    last,
    rows.size()
  );
  for (usize i = first; i < last; i++) {
    out += rows[i].line;
    out += '\n';
  }

  if (last < rows.size()) {
    out += fmt::format("next_cursor {}", last);
  } else {
    out.pop_back();
  }

  return McpResult::ok(std::move(out));
}

static auto file_mkdir(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto path = resolve_content_path(editor, *path_arg, false);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto created = create_folder(editor, path->parent_path(), path->filename().string());
  if (!created) {
    return McpResult::error(created.error());
  }

  return McpResult::ok(fmt::format("created {}/", project_relative_text(editor, *created)));
}

static auto file_move(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto from_arg = args.str("from");
  const auto to_arg = args.str("to_dir");
  if (!from_arg || !to_arg) {
    return McpResult::error(!from_arg ? from_arg.error() : to_arg.error());
  }

  const auto from = resolve_content_path(editor, *from_arg, true);
  if (!from) {
    return McpResult::error(fmt::format("argument 'from': {}", from.error()));
  }

  const auto to_dir = resolve_content_path(editor, *to_arg, true);
  if (!to_dir) {
    return McpResult::error(fmt::format("argument 'to_dir': {}", to_dir.error()));
  }

  const auto moved = move_content_entry(editor, *from, *to_dir);
  if (!moved) {
    return McpResult::error(moved.error());
  }

  return McpResult::ok(
    fmt::format("moved to {}{}", project_relative_text(editor, *moved), sidecar_note(editor, *moved))
  );
}

static auto file_rename(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  const auto name_arg = args.str("name");
  if (!path_arg || !name_arg) {
    return McpResult::error(!path_arg ? path_arg.error() : name_arg.error());
  }

  const auto path = resolve_content_path(editor, *path_arg, true);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto renamed = rename_content_entry(editor, *path, *name_arg);
  if (!renamed) {
    return McpResult::error(renamed.error());
  }

  return McpResult::ok(
    fmt::format("renamed to {}{}", project_relative_text(editor, *renamed), sidecar_note(editor, *renamed))
  );
}

static auto file_delete(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  const auto force_arg = args.opt_boolean("force");
  if (!path_arg || !force_arg) {
    return McpResult::error(!path_arg ? path_arg.error() : force_arg.error());
  }

  const auto path = resolve_content_path(editor, *path_arg, true);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto deleted = delete_content_entry(editor, *path, force_arg->value_or(false));
  if (!deleted) {
    return McpResult::error(deleted.error());
  }

  auto& asset_man = App::mod<AssetManager>();
  std::erase_if(editor.mcp_server.held_assets, [&asset_man](const auto& entry) {
    return !asset_man.get_asset(entry.first);
  });

  auto out = fmt::format(
    "deleted {}, {} asset(s) unregistered",
    project_relative_text(editor, deleted->path),
    deleted->unregistered.size()
  );
  for (const auto& uuid : deleted->unregistered) {
    out += "\n" + uuid.str();
  }

  if (!deleted->kept.empty()) {
    out += fmt::format(
      "\n{} asset(s) are still in use and stay registered until the editor restarts:",
      deleted->kept.size()
    );
    for (const auto& uuid : deleted->kept) {
      out += "\n" + uuid.str();
    }
  }

  return McpResult::ok(std::move(out));
}

static auto file_open(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto path = resolve_content_path(editor, *path_arg, true);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto opened = open_file(editor, *path);
  if (!opened) {
    return McpResult::error(opened.error());
  }

  return McpResult::ok(fmt::format("{}: {}", project_relative_text(editor, *path), *opened));
}

auto register_file_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "file_list",
      .title = "List files",
      .description = "Lists the asset directory or a folder in it. One line per entry: 'dir/' for folders, 'file size "
                     "Type [uuid]' for files, with the uuid of the asset registered for the file. Folders come first, "
                     ".oxasset sidecars are hidden unless show_meta. Paths are project-relative. 'next_cursor N' ends "
                     "a truncated listing.",
      .input_schema = SchemaBuilder{}
                        .string("path", "folder to list, default the asset directory", false)
                        .boolean("recursive", "include everything below it", false)
                        .boolean("show_meta", "list .oxasset sidecars too", false)
                        .integer("limit", "lines per page, default 200", false, 1, LIST_MAX_LIMIT)
                        .string("cursor", "next_cursor from the previous call", false)
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = file_list,
    }
  );

  server.add_tool(
    McpTool{
      .name = "file_mkdir",
      .title = "Create folder",
      .description = "Creates a folder inside the asset directory. The parent must exist.",
      .input_schema = SchemaBuilder{}.string("path", "project-relative folder to create", true).build(),
      .handler = file_mkdir,
    }
  );

  server.add_tool(
    McpTool{
      .name = "file_move",
      .title = "Move file or folder",
      .description = "Moves a file or folder into another folder of the asset directory. Use this instead of a shell "
                     "mv: the .oxasset sidecar moves along and the asset registry follows, so uuids and every "
                     "reference to them survive. Returns the new path.",
      .input_schema = SchemaBuilder{}
                        .string("from", "project-relative file or folder", true)
                        .string("to_dir", "project-relative destination folder", true)
                        .build(),
      .handler = file_move,
    }
  );

  server.add_tool(
    McpTool{
      .name = "file_rename",
      .title = "Rename file or folder",
      .description = "Renames a file or folder in place, with its sidecar and registry entries, which a shell mv would "
                     "break. 'name' is the new file name without a path. Returns the new path.",
      .input_schema = SchemaBuilder{}
                        .string("path", "project-relative file or folder", true)
                        .string("name", "new name, no separators", true)
                        .build(),
      .handler = file_rename,
    }
  );

  server.add_tool(
    McpTool{
      .name = "file_delete",
      .title = "Delete file or folder",
      .description =
        "Deletes a file or folder of the asset directory for good, with its sidecar, and unregisters every "
        "asset that came from it. Refuses while any of those assets is still loaded (scenes or other "
        "assets use it), the error lists them. force=true deletes anyway and unloads them, and whatever "
        "still points at them ends up with a missing asset.",
      .input_schema =
        SchemaBuilder{}
          .string("path", "project-relative file or folder", true)
          .boolean(
            "force",
            "delete the files even when assets under them are in use, those assets stay registered until restart",
            false
          )
          .build(),
      .is_destructive = true,
      .handler = file_delete,
    }
  );

  server.add_tool(
    McpTool{
      .name = "file_open",
      .title = "Open file",
      .description = "Does what double-clicking the file in the content panel does: scenes open as a new scene, "
                     "particle systems and cinematics open in their editors, scripts and shaders open in the external "
                     "editor, other known files open with the system's default application. Textures, models and "
                     "materials do nothing.",
      .input_schema = SchemaBuilder{}.string("path", "project-relative file", true).build(),
      .handler = file_open,
    }
  );
}
} // namespace ox
