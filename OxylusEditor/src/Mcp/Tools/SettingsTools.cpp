#include <algorithm>
#include <cctype>
#include <cmath>
#include <fmt/format.h>
#include <string>
#include <tracy/Tracy.hpp>
#include <utility>
#include <vector>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Render/RenderContext.hpp"

namespace ox {
constexpr static std::string_view HIDDEN_VALUE = "<hidden>";
constexpr static usize SUGGESTION_COUNT = 5;

struct CVarScope {
  std::string_view name = {};
  CVarSystem* system = nullptr;
};

struct CVarEntry {
  std::string_view scope = {};
  CVarSystem* system = nullptr;
  CVarParameter* parameter = nullptr;
};

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto has_flag(const CVarParameter& parameter, CVarFlags flag) -> bool {
  return (std::to_underlying(parameter.flags) & std::to_underlying(flag)) != 0;
}

static auto is_protected(const CVarParameter& parameter) -> bool {
  return parameter.name == "editor.mcp_token" || parameter.name == "editor.mcp_enabled" ||
         parameter.name == "editor.mcp_port";
}

static auto available_scopes(Editor& editor) -> std::vector<CVarScope> {
  std::vector<CVarScope> scopes;

  if (const auto* active = editor.editor_panel_registry.get<SceneHierarchyPanel>().get_scene()) {
    scopes.push_back({"scene", &active->get_scene()->renderer_cvar.system});
  }

  scopes.push_back({"context", &App::get_rendercontext().context_cvar.system});
  scopes.push_back({"editor", &editor.editor_cvar.system});
  return scopes;
}

static auto scope_filter(Editor& editor, option<std::string_view> scope)
  -> std::expected<std::vector<CVarScope>, std::string> {
  auto scopes = available_scopes(editor);
  if (!scope.has_value()) {
    return scopes;
  }

  if (scope.value() != "scene" && scope.value() != "context" && scope.value() != "editor") {
    return std::unexpected("argument 'scope': expected scene, context or editor");
  }

  std::erase_if(scopes, [&](const CVarScope& item) { return item.name != scope.value(); });
  if (scopes.empty()) {
    return std::unexpected("the scene scope needs an open scene");
  }

  return scopes;
}

static auto all_entries(std::span<const CVarScope> scopes) -> std::vector<CVarEntry> {
  std::vector<CVarEntry> entries;
  for (const auto& scope : scopes) {
    for (auto& item : scope.system->int_cvars) {
      entries.push_back({scope.name, scope.system, item.parameter});
    }

    for (auto& item : scope.system->float_cvars) {
      entries.push_back({scope.name, scope.system, item.parameter});
    }

    for (auto& item : scope.system->string_cvars) {
      entries.push_back({scope.name, scope.system, item.parameter});
    }
  }

  std::ranges::sort(entries, [](const CVarEntry& a, const CVarEntry& b) {
    return a.scope != b.scope ? a.scope < b.scope : a.parameter->name < b.parameter->name;
  });
  return entries;
}

static auto type_name(const CVarParameter& parameter) -> std::string_view {
  switch (parameter.type) {
    case CVarType::INT   : return has_flag(parameter, CVarFlags::EditCheckbox) ? "bool" : "int";
    case CVarType::FLOAT : return "float";
    case CVarType::STRING: return "string";
  }

  return "int";
}

static auto format_int(const CVarParameter& parameter, i32 value) -> std::string {
  return has_flag(parameter, CVarFlags::EditCheckbox) ? std::string(value != 0 ? "true" : "false")
                                                      : std::to_string(value);
}

static auto value_text(const CVarEntry& entry, bool is_default) -> std::string {
  const auto& parameter = *entry.parameter;
  if (is_protected(parameter) && parameter.name == "editor.mcp_token") {
    return std::string(HIDDEN_VALUE);
  }

  switch (parameter.type) {
    case CVarType::INT: {
      const auto& item = entry.system->int_cvars[parameter.array_index];
      return format_int(parameter, is_default ? item.initial : item.current);
    }
    case CVarType::FLOAT: {
      const auto& item = entry.system->float_cvars[parameter.array_index];
      return fmt::format("{}", is_default ? item.initial : item.current);
    }
    case CVarType::STRING: {
      const auto& item = entry.system->string_cvars[parameter.array_index];
      return fmt::format("\"{}\"", is_default ? item.initial : item.current);
    }
  }

  return {};
}

static auto entry_line(const CVarEntry& entry) -> std::string {
  return fmt::format(
    "{} {} = {} (default {}) {}: {}",
    entry.scope,
    entry.parameter->name,
    value_text(entry, false),
    value_text(entry, true),
    type_name(*entry.parameter),
    entry.parameter->description
  );
}

static auto find_entry(Editor& editor, std::string_view name, option<std::string_view> scope)
  -> std::expected<CVarEntry, std::string> {
  const auto scopes = scope_filter(editor, scope);
  if (!scopes) {
    return std::unexpected(scopes.error());
  }

  const auto hash = std::hash<std::string>{}(std::string(name));
  std::vector<CVarEntry> found;
  for (const auto& item : *scopes) {
    if (auto* parameter = item.system->get_cvar(hash)) {
      found.push_back({item.name, item.system, parameter});
    }
  }

  if (found.size() == 1) {
    return found.front();
  }

  if (found.size() > 1) {
    std::string names;
    for (const auto& entry : found) {
      names += fmt::format("{}{}", names.empty() ? "" : ", ", entry.scope);
    }

    return std::unexpected(fmt::format("'{}' exists in several scopes ({}), pass scope", name, names));
  }

  std::string suggestions;
  usize suggested = 0;
  const auto needle = lower(name);
  for (const auto& entry : all_entries(*scopes)) {
    if (suggested < SUGGESTION_COUNT && lower(entry.parameter->name).find(needle) != std::string::npos) {
      suggestions += fmt::format("{}{}", suggestions.empty() ? "" : ", ", entry.parameter->name);
      suggested += 1;
    }
  }

  return std::unexpected(
    fmt::format(
      "no cvar '{}'{}",
      name,
      suggestions.empty() ? ", cvar_list shows the names" : fmt::format(", similar: {}", suggestions)
    )
  );
}

static auto optional_scope(const McpArgs& args) -> std::expected<option<std::string_view>, std::string> {
  return args.opt_str("scope");
}

static auto cvar_list(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto scope = optional_scope(args);
  if (!scope) {
    return McpResult::error(scope.error());
  }

  const auto filter_arg = args.opt_str("filter");
  if (!filter_arg) {
    return McpResult::error(filter_arg.error());
  }

  const auto scopes = scope_filter(editor, *scope);
  if (!scopes) {
    return McpResult::error(scopes.error());
  }

  const auto filter = lower(filter_arg->value_or(""));
  std::string out;
  usize count = 0;
  for (const auto& entry : all_entries(*scopes)) {
    if (!filter.empty() && lower(entry.parameter->name).find(filter) == std::string::npos) {
      continue;
    }

    out += entry_line(entry);
    out += '\n';
    count += 1;
  }

  if (count == 0) {
    return McpResult::ok("no cvars match");
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}

static auto cvar_get(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto name = args.str("name");
  if (!name) {
    return McpResult::error(name.error());
  }

  const auto scope = optional_scope(args);
  if (!scope) {
    return McpResult::error(scope.error());
  }

  const auto entry = find_entry(editor, *name, *scope);
  if (!entry) {
    return McpResult::error(entry.error());
  }

  return McpResult::ok(entry_line(*entry));
}

static auto apply_value(CVarEntry& entry, const simdjson::dom::element& value) -> std::expected<void, std::string> {
  const auto& parameter = *entry.parameter;
  const auto hash = std::hash<std::string>{}(parameter.name);
  const auto type = value.type();

  switch (parameter.type) {
    case CVarType::INT: {
      double number = 0.0;
      if (type == simdjson::dom::element_type::BOOL) {
        number = bool(value) ? 1.0 : 0.0;
      } else if (
        type == simdjson::dom::element_type::INT64 || type == simdjson::dom::element_type::UINT64 ||
        type == simdjson::dom::element_type::DOUBLE
      ) {
        number = double(value);
      } else {
        return std::unexpected("value: expected a number or a boolean");
      }

      if (std::floor(number) != number || number < -2147483648.0 || number > 2147483647.0) {
        return std::unexpected("value: expected an integer");
      }

      entry.system->set_int_cvar(hash, static_cast<i32>(number));
      return {};
    }
    case CVarType::FLOAT: {
      if (
        type != simdjson::dom::element_type::INT64 && type != simdjson::dom::element_type::UINT64 &&
        type != simdjson::dom::element_type::DOUBLE
      ) {
        return std::unexpected("value: expected a number");
      }

      entry.system->set_float_cvar(hash, static_cast<f32>(double(value)));
      return {};
    }
    case CVarType::STRING: {
      if (type != simdjson::dom::element_type::STRING) {
        return std::unexpected("value: expected a string");
      }

      entry.system->set_string_cvar(hash, std::string_view(value));
      return {};
    }
  }

  return {};
}

static auto cvar_set(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto name = args.str("name");
  if (!name) {
    return McpResult::error(name.error());
  }

  const auto scope = optional_scope(args);
  if (!scope) {
    return McpResult::error(scope.error());
  }

  simdjson::dom::object object;
  simdjson::dom::element value;
  if (args.raw().get(object) || object.at_key("value").get(value)) {
    return McpResult::error("argument 'value' is required");
  }

  auto entry = find_entry(editor, *name, *scope);
  if (!entry) {
    return McpResult::error(entry.error());
  }

  if (is_protected(*entry->parameter)) {
    return McpResult::error(fmt::format("'{}' can only be changed in the editor settings", *name));
  }

  if (has_flag(*entry->parameter, CVarFlags::EditReadOnly)) {
    return McpResult::error(fmt::format("'{}' is read only", *name));
  }

  const auto applied = apply_value(*entry, value);
  if (!applied) {
    return McpResult::error(fmt::format("'{}' is {}, {}", *name, type_name(*entry->parameter), applied.error()));
  }

  if (entry->scope == "context") {
    App::get_rendercontext().context_cvar.save();
  } else if (entry->scope == "editor") {
    editor.editor_cvar.save();
  }

  return McpResult::ok(entry_line(*entry));
}

auto register_settings_tools(McpServer& server) -> void {
  ZoneScoped;

  const std::vector<std::string> scopes = {"scene", "context", "editor"};

  server.add_tool(
    McpTool{
      .name = "cvar_list",
      .title = "List cvars",
      .description = "Lists settings, one per line: scope name = value (default d) type: description. Scopes: scene "
                     "is the active scene's renderer settings and is saved with the scene, context is the global "
                     "render and window settings, editor is the editor's own.",
      .input_schema = SchemaBuilder{}
                        .string("scope", "only this scope, default all", false, scopes)
                        .string("filter", "case-insensitive substring of the name", false)
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = cvar_list,
    }
  );

  server.add_tool(
    McpTool{
      .name = "cvar_get",
      .title = "Get cvar",
      .description = "Reads one setting. Without a scope all scopes are searched and an ambiguous name is an error.",
      .input_schema = SchemaBuilder{}
                        .string("name", "full cvar name from cvar_list", true)
                        .string("scope", "scene, context or editor", false, scopes)
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = cvar_get,
    }
  );

  server.add_tool(
    McpTool{
      .name = "cvar_set",
      .title = "Set cvar",
      .description = "Changes one setting. The value is a JSON number, boolean or string matching the cvar type, "
                     "integers reject fractions. Context and editor settings are saved to disk right away, scene "
                     "settings are saved with the scene. The MCP server's own settings cannot be changed here.",
      .input_schema = SchemaBuilder{}
                        .string("name", "full cvar name from cvar_list", true)
                        .object("value", "new value: number, boolean or string", "{}", true)
                        .string("scope", "scene, context or editor", false, scopes)
                        .build(),
      .is_idempotent = true,
      .handler = cvar_set,
    }
  );
}
} // namespace ox
