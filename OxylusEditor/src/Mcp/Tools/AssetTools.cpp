#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fmt/format.h>
#include <memory>
#include <simdjson.h>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/AssetActions.hpp"
#include "Actions/FileActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Render/Utils/TextureFormat.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
constexpr static i64 LIST_DEFAULT_LIMIT = 200;
constexpr static i64 LIST_MAX_LIMIT = 5000;
constexpr static i64 THUMBNAIL_DEFAULT_SIZE = 256;
constexpr static i64 THUMBNAIL_MAX_SIZE = 1024;
constexpr static u32 THUMBNAIL_TIMEOUT_MS = 30000;
constexpr static u32 IMPORT_TIMEOUT_MS = 300000;
constexpr static u32 LOAD_TIMEOUT_MS = 120000;
constexpr static auto ASSET_TYPE_COUNT = std::to_underlying(AssetType::UI) + 1;

enum class PatchKind : u8 { Number, Bool, Vec2, Vec3, Vec4, Enum, Texture };

struct PatchField {
  std::string_view name = {};
  PatchKind kind = PatchKind::Number;
  i64 enum_max = 0;
};

constexpr static std::array<PatchField, 17> MATERIAL_FIELDS = {{
  {"albedo_color", PatchKind::Vec4, 0},
  {"uv_size", PatchKind::Vec2, 0},
  {"uv_offset", PatchKind::Vec2, 0},
  {"emissive_color", PatchKind::Vec3, 0},
  {"roughness_factor", PatchKind::Number, 0},
  {"metallic_factor", PatchKind::Number, 0},
  {"normal_scale", PatchKind::Number, 0},
  {"occlusion_strength", PatchKind::Number, 0},
  {"alpha_mode", PatchKind::Enum, 2},
  {"alpha_cutoff", PatchKind::Number, 0},
  {"sampling_mode", PatchKind::Enum, 4},
  {"flip_normal_y", PatchKind::Bool, 0},
  {"albedo_texture", PatchKind::Texture, 0},
  {"normal_texture", PatchKind::Texture, 0},
  {"emissive_texture", PatchKind::Texture, 0},
  {"metallic_roughness_texture", PatchKind::Texture, 0},
  {"occlusion_texture", PatchKind::Texture, 0},
}};

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto parse_asset_type(std::string_view name) -> option<AssetType> {
  const auto wanted = lower(name);
  for (u32 i = 0; i < ASSET_TYPE_COUNT; i++) {
    const auto type = static_cast<AssetType>(i);
    if (lower(AssetManager::to_asset_type_sv(type)) == wanted) {
      return type;
    }
  }

  return nullopt;
}

static auto asset_type_names() -> std::vector<std::string> {
  std::vector<std::string> names;
  for (u32 i = 0; i < ASSET_TYPE_COUNT; i++) {
    names.emplace_back(AssetManager::to_asset_type_sv(static_cast<AssetType>(i)));
  }

  return names;
}

static auto parse_cursor(const McpArgs& args) -> std::expected<usize, std::string> {
  const auto cursor_arg = args.opt_str("cursor");
  if (!cursor_arg) {
    return std::unexpected(cursor_arg.error());
  }

  if (!cursor_arg->has_value()) {
    return usize{0};
  }

  const auto text = cursor_arg->value();
  u64 value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::unexpected("argument 'cursor': not a cursor returned by this tool");
  }

  return static_cast<usize>(value);
}

static auto asset_path_text(Editor& editor, const Asset& asset) -> std::string {
  const auto& vfs = App::get_vfs();
  if (!asset.source_path.empty()) {
    return project_relative_text(editor, vfs.to_physical(asset.source_path));
  }

  if (const auto source = asset_source(asset.uuid); !source.path.empty()) {
    return fmt::format("{}#{}", project_relative_text(editor, source.path), source.name);
  }

  return project_relative_text(editor, vfs.to_physical(asset.path));
}

static auto resolve_target(Editor& editor, const McpArgs& args) -> std::expected<UUID, std::string> {
  const auto ref = args.str("asset");
  if (!ref) {
    return std::unexpected(ref.error());
  }

  const auto uuid = resolve_asset(*ref);
  if (!uuid) {
    return std::unexpected(fmt::format("argument 'asset': {}", uuid.error()));
  }

  return *uuid;
}

static auto asset_type_of(const UUID& uuid) -> AssetType {
  const auto asset = App::mod<AssetManager>().get_asset(uuid);
  return asset ? asset->type : AssetType::None;
}

// holds one ref for MCP when nothing has the asset loaded, so an edit has something to land on
static auto ensure_loaded(Editor& editor, const UUID& uuid) -> std::expected<void, std::string> {
  auto& asset_man = App::mod<AssetManager>();
  if (asset_man.is_loaded(uuid)) {
    return {};
  }

  if (!asset_man.load_asset(uuid)) {
    return std::unexpected(fmt::format("asset {} could not be loaded, log_read has the reason", uuid.str()));
  }

  editor.mcp_server.held_assets[uuid] += 1;
  return {};
}

static auto asset_list(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto type_arg = args.opt_str("type");
  const auto prefix_arg = args.opt_str("path_prefix");
  const auto loaded_arg = args.opt_boolean("loaded_only");
  const auto limit_arg = args.opt_integer("limit");
  if (!type_arg || !prefix_arg || !loaded_arg || !limit_arg) {
    return McpResult::error(
      !type_arg     ? type_arg.error()
      : !prefix_arg ? prefix_arg.error()
      : !loaded_arg ? loaded_arg.error()
                    : limit_arg.error()
    );
  }

  const auto cursor = parse_cursor(args);
  if (!cursor) {
    return McpResult::error(cursor.error());
  }

  auto type_filter = option<AssetType>(nullopt);
  if (type_arg->has_value()) {
    type_filter = parse_asset_type(type_arg->value());
    if (!type_filter) {
      return McpResult::error(fmt::format("argument 'type': unknown asset type '{}'", type_arg->value()));
    }
  }

  auto prefix = std::string{};
  if (prefix_arg->has_value()) {
    const auto resolved = resolve_content_path(editor, prefix_arg->value(), false);
    if (!resolved) {
      return McpResult::error(fmt::format("argument 'path_prefix': {}", resolved.error()));
    }

    prefix = project_relative_text(editor, *resolved);
  }

  const auto limit = static_cast<usize>(std::clamp<i64>(limit_arg->value_or(LIST_DEFAULT_LIMIT), 1, LIST_MAX_LIMIT));
  const auto is_loaded_only = loaded_arg->value_or(false);

  struct Row {
    std::string path = {};
    std::string line = {};
  };

  std::vector<Row> rows;
  for (const auto& asset : App::mod<AssetManager>().get_registry_snapshot()) {
    if (type_filter && asset.type != *type_filter) {
      continue;
    }

    if (is_loaded_only && !asset.is_loaded()) {
      continue;
    }

    auto path = asset_path_text(editor, asset);
    if (!prefix.empty() && path != prefix && !path.starts_with(prefix + "/")) {
      continue;
    }

    auto line = fmt::format("{} {} {}", asset.uuid.str(), AssetManager::to_asset_type_sv(asset.type), path);
    if (asset.is_loaded()) {
      line += fmt::format(" loaded refs={}", asset.ref_count);
    }

    rows.push_back({.path = std::move(path), .line = std::move(line)});
  }

  std::ranges::sort(rows, [](const Row& lhs, const Row& rhs) {
    return lhs.path != rhs.path ? lhs.path < rhs.path : lhs.line < rhs.line;
  });

  const auto first = std::min(*cursor, rows.size());
  const auto last = std::min(first + limit, rows.size());
  auto out = fmt::format("assets {}-{} of {}\n", rows.empty() ? 0 : first + 1, last, rows.size());
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

static auto write_uuid_list(JsonWriter& writer, std::string_view key, std::span<const UUID> uuids) -> void {
  writer[key].begin_array();
  for (const auto& uuid : uuids) {
    writer = uuid.str();
  }

  writer.end_array();
}

static auto asset_get(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uuid = resolve_target(editor, args);
  if (!uuid) {
    return McpResult::error(uuid.error());
  }

  auto& asset_man = App::mod<AssetManager>();
  auto asset = Asset{};
  {
    const auto guard = asset_man.get_asset(*uuid);
    if (!guard) {
      return McpResult::error(fmt::format("asset {} is not registered anymore", uuid->str()));
    }

    asset = *guard.value;
  }

  const auto is_loaded = asset.is_loaded();
  const auto& vfs = App::get_vfs();

  JsonWriter writer{};
  writer.is_pretty = false;
  writer.begin_obj();
  writer["uuid"] = uuid->str();
  writer["type"] = AssetManager::to_asset_type_sv(asset.type);
  writer["path"] = asset_path_text(editor, asset);
  writer["pack"] = project_relative_text(editor, vfs.to_physical(asset.path));
  writer["ref_count"] = asset.ref_count;
  writer["loaded"] = is_loaded;
  if (const auto held = editor.mcp_server.held_assets.find(*uuid); held != editor.mcp_server.held_assets.end()) {
    writer["held_by_mcp"] = static_cast<u64>(held->second);
  }

  switch (asset.type) {
    case AssetType::Material: {
      auto material = Material{};
      auto is_known = false;
      if (is_loaded) {
        if (const auto guard = asset_man.get_material(*uuid)) {
          material = *guard.value;
          is_known = true;
        }
      } else if (
        const auto pending = asset_man.get_pending_load_info(*uuid); std::holds_alternative<Material>(pending)
      ) {
        material = std::get<Material>(pending);
        is_known = true;
      }

      if (is_known) {
        writer["material"];
        writer.raw(material_json(*uuid, material));
      }
    } break;
    case AssetType::ParticleSystem:
    case AssetType::Cinematic     : {
      if (is_loaded) {
        if (const auto document = asset_document(*uuid)) {
          writer["document"];
          writer.raw(*document);
        }
      }
    } break;
    case AssetType::Model: {
      if (!is_loaded) {
        break;
      }

      const auto guard = asset_man.get_model(*uuid);
      if (!guard) {
        break;
      }

      writer["mesh_count"] = static_cast<u64>(guard->gpu_meshes.size());
      writer["material_count"] = static_cast<u64>(guard->materials.size());
      writer["texture_count"] = static_cast<u64>(guard->textures.size());
      writer["animation_count"] = static_cast<u64>(guard->animations.size());
      write_uuid_list(writer, "materials", guard->materials);
      write_uuid_list(writer, "textures", guard->textures);
      write_uuid_list(writer, "animations", guard->animations);
      if (guard->skeleton_uuid) {
        writer["skeleton"] = guard->skeleton_uuid.str();
      }
    } break;
    case AssetType::Texture: {
      if (!is_loaded) {
        break;
      }

      const auto guard = asset_man.get_texture(*uuid);
      if (!guard) {
        break;
      }

      const auto extent = guard->get_extent();
      writer["width"] = extent.width;
      writer["height"] = extent.height;
      writer["mip_levels"] = guard->get_level_count();
      writer["format"] = format_name(guard->get_format());
    } break;
    default: break;
  }

  writer.end_obj();
  return McpResult::ok(writer.stream.str());
}

static auto asset_import(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto path = resolve_content_path(editor, *path_arg, true);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto uuid = import_file(editor, *path);
  if (!uuid) {
    return McpResult::error(uuid.error());
  }

  return McpResult::ok(
    fmt::format(
      "{} {} {}",
      uuid->str(),
      AssetManager::to_asset_type_sv(asset_type_of(*uuid)),
      project_relative_text(editor, *path)
    )
  );
}

static auto asset_create(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto type_arg = args.str("type");
  const auto path_arg = args.str("path");
  if (!type_arg || !path_arg) {
    return McpResult::error(!type_arg ? type_arg.error() : path_arg.error());
  }

  const auto type = parse_asset_type(*type_arg);
  if (!type) {
    return McpResult::error(fmt::format("argument 'type': unknown asset type '{}'", *type_arg));
  }

  const auto path = resolve_content_path(editor, *path_arg, false);
  if (!path) {
    return McpResult::error(fmt::format("argument 'path': {}", path.error()));
  }

  const auto created = create_asset(editor, *type, *path);
  if (!created) {
    return McpResult::error(created.error());
  }

  return McpResult::ok(
    fmt::format(
      "{} {} {}",
      created->uuid.str(),
      AssetManager::to_asset_type_sv(*type),
      project_relative_text(editor, created->path)
    )
  );
}

static auto read_number(simdjson::dom::element value, std::string_view key) -> std::expected<f64, std::string> {
  f64 number = 0.0;
  if (value.get(number) || !std::isfinite(number)) {
    return std::unexpected(fmt::format("material.{}: expected a finite number", key));
  }

  return number;
}

static auto check_material_patch(simdjson::dom::object patch) -> std::expected<void, std::string> {
  for (const auto& [key, value] : patch) {
    const auto field = std::ranges::find(MATERIAL_FIELDS, key, &PatchField::name);
    if (field == MATERIAL_FIELDS.end()) {
      std::string valid;
      for (const auto& candidate : MATERIAL_FIELDS) {
        valid += valid.empty() ? "" : ", ";
        valid += candidate.name;
      }

      return std::unexpected(fmt::format("material.{}: unknown field, valid: {}", key, valid));
    }

    switch (field->kind) {
      case PatchKind::Number: {
        if (const auto number = read_number(value, key); !number) {
          return std::unexpected(number.error());
        }
      } break;
      case PatchKind::Bool: {
        if (!value.is_bool()) {
          return std::unexpected(fmt::format("material.{}: expected true or false", key));
        }
      } break;
      case PatchKind::Vec2:
      case PatchKind::Vec3:
      case PatchKind::Vec4: {
        simdjson::dom::object vec;
        if (value.get(vec)) {
          return std::unexpected(fmt::format("material.{}: expected an object like {{\"x\":1,\"y\":1}}", key));
        }

        const auto axis_count = field->kind == PatchKind::Vec2 ? 2 : field->kind == PatchKind::Vec3 ? 3 : 4;
        constexpr std::array<std::string_view, 4> axes = {"x", "y", "z", "w"};
        for (const auto& [axis, component] : vec) {
          const auto* const end = axes.begin() + axis_count;
          if (std::find(axes.begin(), end, axis) == end) {
            return std::unexpected(fmt::format("material.{}.{}: not an axis of this vector", key, axis));
          }

          if (const auto number = read_number(component, fmt::format("{}.{}", key, axis)); !number) {
            return std::unexpected(number.error());
          }
        }
      } break;
      case PatchKind::Enum: {
        i64 number = 0;
        if (value.get(number) || number < 0 || number > field->enum_max) {
          return std::unexpected(fmt::format("material.{}: expected an integer from 0 to {}", key, field->enum_max));
        }
      } break;
      case PatchKind::Texture: {
        if (!value.is_string() && !value.is_null()) {
          return std::unexpected(fmt::format("material.{}: expected an asset path or uuid, or null to clear", key));
        }
      } break;
    }
  }

  return {};
}

static auto patch_material(Editor& editor, const UUID& uuid, const McpArgs& patch_args) -> McpResult {
  simdjson::dom::object patch;
  if (patch_args.raw().get(patch)) {
    return McpResult::error("argument 'material': expected an object");
  }

  if (const auto checked = check_material_patch(patch); !checked) {
    return McpResult::error(checked.error());
  }

  if (const auto loaded = ensure_loaded(editor, uuid); !loaded) {
    return McpResult::error(loaded.error());
  }

  auto& asset_man = App::mod<AssetManager>();
  auto material = Material{};
  {
    const auto guard = asset_man.get_material(uuid);
    if (!guard) {
      return McpResult::error("the asset is not a loaded material");
    }

    material = *guard.value;
  }

  if (const auto read = rc::read_material_asset_json(simdjson::minify(patch_args.raw()), material); !read) {
    return McpResult::error(fmt::format("argument 'material': {}", read.error()));
  }

  std::string applied;
  for (const auto& [key, value] : patch) {
    applied += applied.empty() ? "" : ", ";
    applied += key;

    const auto field = std::ranges::find(MATERIAL_FIELDS, key, &PatchField::name);
    if (field->kind != PatchKind::Texture) {
      continue;
    }

    auto texture = UUID(nullptr);
    std::string_view ref;
    if (!value.get(ref) && !ref.empty()) {
      const auto resolved = resolve_asset(ref);
      if (!resolved) {
        return McpResult::error(fmt::format("material.{}: {}", key, resolved.error()));
      }

      if (asset_type_of(*resolved) != AssetType::Texture) {
        return McpResult::error(
          fmt::format(
            "material.{}: '{}' is a {} asset, a texture is needed",
            key,
            ref,
            AssetManager::to_asset_type_sv(asset_type_of(*resolved))
          )
        );
      }

      texture = *resolved;
    }

    if (key == "albedo_texture") {
      material.albedo_texture = texture;
    } else if (key == "normal_texture") {
      material.normal_texture = texture;
    } else if (key == "emissive_texture") {
      material.emissive_texture = texture;
    } else if (key == "metallic_roughness_texture") {
      material.metallic_roughness_texture = texture;
    } else {
      material.occlusion_texture = texture;
    }
  }

  if (const auto updated = update_material(editor, uuid, material); !updated) {
    return McpResult::error(updated.error());
  }

  return McpResult::ok(
    fmt::format(
      "updated {}: {}\nnot saved to disk yet, asset_save writes it\n{}",
      uuid.str(),
      applied,
      material_json(uuid, material)
    )
  );
}

static auto asset_update(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uuid = resolve_target(editor, args);
  if (!uuid) {
    return McpResult::error(uuid.error());
  }

  const auto document_arg = args.opt_str("document");
  if (!document_arg) {
    return McpResult::error(document_arg.error());
  }

  const auto has_material = args.has("material");
  if (has_material == document_arg->has_value()) {
    return McpResult::error("give exactly one of 'material' (a patch) or 'document' (the whole json)");
  }

  const auto type = asset_type_of(*uuid);
  if (has_material) {
    if (type != AssetType::Material) {
      return McpResult::error(
        fmt::format("'material' needs a material asset, this is a {}", AssetManager::to_asset_type_sv(type))
      );
    }

    const auto patch = args.object("material");
    if (!patch) {
      return McpResult::error(patch.error());
    }

    return patch_material(editor, *uuid, *patch);
  }

  if (type != AssetType::ParticleSystem && type != AssetType::Cinematic) {
    return McpResult::error(
      fmt::format("'document' needs a particle system or cinematic, this is a {}", AssetManager::to_asset_type_sv(type))
    );
  }

  if (const auto loaded = ensure_loaded(editor, *uuid); !loaded) {
    return McpResult::error(loaded.error());
  }

  if (const auto set = set_asset_document(editor, *uuid, document_arg->value()); !set) {
    return McpResult::error(set.error());
  }

  return McpResult::ok(
    fmt::format(
      "replaced the {} document of {}, not saved to disk yet, asset_save writes it",
      lower(AssetManager::to_asset_type_sv(type)),
      uuid->str()
    )
  );
}

static auto asset_save(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uuid = resolve_target(editor, args);
  if (!uuid) {
    return McpResult::error(uuid.error());
  }

  const auto path = save_asset(editor, *uuid);
  if (!path) {
    return McpResult::error(path.error());
  }

  return McpResult::ok(fmt::format("saved {} to {}", uuid->str(), project_relative_text(editor, *path)));
}

static auto asset_load(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uuid = resolve_target(editor, args);
  const auto unload_arg = args.opt_boolean("unload");
  if (!uuid || !unload_arg) {
    return McpResult::error(!uuid ? uuid.error() : unload_arg.error());
  }

  auto& asset_man = App::mod<AssetManager>();
  auto& held = editor.mcp_server.held_assets;
  if (unload_arg->value_or(false)) {
    const auto entry = held.find(*uuid);
    if (entry == held.end()) {
      return McpResult::error(
        "MCP holds no reference to this asset, only references taken with asset_load can be released"
      );
    }

    if (--entry->second == 0) {
      held.erase(entry);
    }

    asset_man.unload_asset(*uuid);
    return McpResult::ok(
      fmt::format(
        "released one MCP reference to {}, {} left, asset is {}",
        uuid->str(),
        held.contains(*uuid) ? held.at(*uuid) : 0,
        asset_man.is_loaded(*uuid) ? "still loaded by other holders" : "unloaded"
      )
    );
  }

  if (!asset_man.load_asset(*uuid)) {
    return McpResult::error(fmt::format("asset {} could not be loaded, log_read has the reason", uuid->str()));
  }

  held[*uuid] += 1;
  return McpResult::ok(fmt::format("loaded {}, MCP now holds {} reference(s) to it", uuid->str(), held[*uuid]));
}

static auto asset_thumbnail(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uuid = resolve_target(editor, args);
  const auto size_arg = args.opt_integer("size");
  if (!uuid || !size_arg) {
    return McpResult::error(!uuid ? uuid.error() : size_arg.error());
  }

  const auto type = asset_type_of(*uuid);
  if (type != AssetType::Model && type != AssetType::Material && type != AssetType::Texture) {
    return McpResult::error(
      fmt::format(
        "{} assets have no thumbnail, models, materials and textures do",
        AssetManager::to_asset_type_sv(type)
      )
    );
  }

  const auto size = static_cast<u32>(
    std::clamp<i64>(size_arg->value_or(THUMBNAIL_DEFAULT_SIZE), 16, THUMBNAIL_MAX_SIZE)
  );
  const auto capture = editor.thumbnail_manager.capture_png(*uuid, size);
  const auto
    caption = fmt::format("{} {} at up to {}x{}", uuid->str(), AssetManager::to_asset_type_sv(type), size, size);
  return McpResult::pending([capture, caption]() -> option<McpResult> {
    if (!capture->is_done.load(std::memory_order_acquire)) {
      return nullopt;
    }

    if (!capture->error.empty()) {
      return McpResult::error(capture->error);
    }

    return McpResult::image(std::move(capture->png), caption);
  });
}

auto register_asset_tools(McpServer& server) -> void {
  ZoneScoped;

  const auto types = asset_type_names();

  server.add_tool(
    McpTool{
      .name = "asset_list",
      .title = "List assets",
      .description = "One line per registered asset: 'uuid Type path', with 'loaded refs=N' when it is loaded. Paths "
                     "are project-relative, a model's materials and textures show as 'model path#slot name'. Sorted "
                     "by path. 'next_cursor N' ends a truncated listing.",
      .input_schema = SchemaBuilder{}
                        .string("type", "only assets of this type", false, types)
                        .string("path_prefix", "only this file or folder and what is inside it", false)
                        .boolean("loaded_only", "only assets that are loaded right now", false)
                        .integer("limit", "lines per page, default 200", false, 1, LIST_MAX_LIMIT)
                        .string("cursor", "next_cursor from the previous call", false)
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = asset_list,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_get",
      .title = "Get asset",
      .description = "Compact json about one asset: type, path, pack, ref_count, loaded. Loaded materials add the "
                     "material json (the shape asset_update patches), particle systems and cinematics add their "
                     "document, models add mesh counts and the uuids of the materials, textures and animations they "
                     "own, textures add size and format. Unloaded assets only show the registry facts, asset_load "
                     "them for the rest.",
      .input_schema = SchemaBuilder{}.string("asset", "asset uuid or the path of its file", true).build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = asset_get,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_import",
      .title = "Import file",
      .description = "Imports a model, texture, audio or other source file inside the asset directory and registers "
                     "everything it produces, then returns 'uuid Type path'. It runs on the editor's main thread, so "
                     "a large model freezes the editor until it is done. Importing again updates the same asset.",
      .input_schema = SchemaBuilder{}.string("path", "project-relative file", true).build(),
      .is_idempotent = true,
      .timeout_ms = IMPORT_TIMEOUT_MS,
      .handler = asset_import,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_create",
      .title = "Create asset",
      .description = "Creates a new asset file with its sidecar and returns 'uuid Type path'. Materials and particle "
                     "systems start with default values, a cinematic with an empty timeline. A particle system gets "
                     ".oxparticle and a cinematic .oxcine appended when the name lacks it. Use scene_new and "
                     "scene_save for scenes.",
      .input_schema = SchemaBuilder{}
                        .string("type", "what to create", true, {"Material", "ParticleSystem", "Cinematic"})
                        .string("path", "project-relative file to create, inside the asset directory", true)
                        .build(),
      .handler = asset_create,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_update",
      .title = "Update asset",
      .description = "Edits a loaded asset in memory (an unloaded one is loaded and kept by MCP, see asset_load). "
                     "'material' patches a material, only the keys you give change: albedo_color, uv_size, uv_offset, "
                     "emissive_color (objects of x y z w), roughness_factor, metallic_factor, normal_scale, "
                     "occlusion_strength, alpha_cutoff (numbers), alpha_mode (0 Opaque, 1 Mask, 2 Blend), "
                     "sampling_mode (0 LinearRepeated, 1 LinearClamped, 2 NearestRepeated, 3 NearestClamped, "
                     "4 LinearRepeatedAnisotropy), flip_normal_y (boolean), and albedo_texture, normal_texture, "
                     "emissive_texture, metallic_roughness_texture, occlusion_texture (texture asset path or uuid, "
                     "null clears). 'document' replaces the whole json of a particle system or cinematic, as asset_get "
                     "shows it. Returns what changed. Not undoable, the editor's undo only covers scenes, and "
                     "nothing reaches disk until asset_save. A particle or cinematic editor panel that has the asset "
                     "open keeps its own working copy and can overwrite the change.",
      .input_schema =
        SchemaBuilder{}
          .string("asset", "asset uuid or the path of its file", true)
          .object("material", "material fields to change", R"({"type":"object","additionalProperties":true})", false)
          .string("document", "complete json document for a particle system or cinematic", false)
          .build(),
      .handler = asset_update,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_save",
      .title = "Save asset",
      .description = "Writes a loaded material, particle system, cinematic, scene or terrain asset and its sidecar to "
                     "its file. Materials of an imported model have no file of their own.",
      .input_schema = SchemaBuilder{}.string("asset", "asset uuid or the path of its file", true).build(),
      .is_idempotent = true,
      .handler = asset_save,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_load",
      .title = "Load or unload asset",
      .description = "Loads an asset and keeps one reference to it for MCP, so it stays loaded while you work on it. "
                     "Each call takes another reference. With unload=true it releases one of the references MCP took, "
                     "never one that belongs to the editor or a scene, and the asset only unloads when nothing else "
                     "holds it. MCP's references are released when the server stops.",
      .input_schema = SchemaBuilder{}
                        .string("asset", "asset uuid or the path of its file", true)
                        .boolean("unload", "release one MCP reference instead of taking one", false)
                        .build(),
      .timeout_ms = LOAD_TIMEOUT_MS,
      .handler = asset_load,
    }
  );

  server.add_tool(
    McpTool{
      .name = "asset_thumbnail",
      .title = "Asset thumbnail",
      .description = "Renders a PNG preview of a model, material (on a sphere) or texture and returns it as an "
                     "image. Models and materials are rendered by the editor and are loaded for it, textures are "
                     "decoded from their source file (PNG, JPEG, BMP, TGA) and fit inside size.",
      .input_schema = SchemaBuilder{}
                        .string("asset", "asset uuid or the path of its file", true)
                        .integer("size", "edge length in pixels, default 256", false, 16, THUMBNAIL_MAX_SIZE)
                        .build(),
      .is_read_only = true,
      .timeout_ms = THUMBNAIL_TIMEOUT_MS,
      .handler = asset_thumbnail,
    }
  );
}
} // namespace ox
