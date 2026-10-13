#include "SceneCompiler.hpp"

#include <fmt/format.h>
#include <simdjson.h>

#include "Core/Types.hpp"

namespace ox::rc {
// every UUID below a component, with the field path that leads to it
static auto collect_field_references(
  simdjson::dom::element value, const std::string& where, std::vector<SceneReference>& references
) -> void {
  if (auto object = value.get_object(); !object.error()) {
    for (const auto field : object.value_unsafe()) {
      collect_field_references(field.value, fmt::format("{}.{}", where, std::string_view(field.key)), references);
    }
    return;
  }

  if (auto array = value.get_array(); !array.error()) {
    auto index = 0_sz;
    for (const auto element : array.value_unsafe()) {
      collect_field_references(element, fmt::format("{}[{}]", where, index), references);
      index += 1;
    }
    return;
  }

  // an unset slot is written as the nil UUID, which the loader skips too
  if (auto string = value.get_string(); !string.error()) {
    if (const auto uuid = UUID::from_string(string.value_unsafe()); uuid.has_value() && *uuid) {
      references.push_back(SceneReference{.uuid = *uuid, .where = where});
    }
  }
}

static auto collect_entity_references(
  simdjson::dom::element entity, std::string_view parent_path, std::vector<SceneReference>& references
) -> void {
  auto name = std::string_view("<unnamed>");
  if (auto name_json = entity["name"].get_string(); !name_json.error()) {
    name = name_json.value_unsafe();
  }

  // the hierarchy path, a name alone is rarely unique in a scene
  const auto path = parent_path.empty() ? std::string(name) : fmt::format("{}/{}", parent_path, name);
  // an array of one-key objects, component path to its fields, as `Scene::entity_to_json` writes it
  if (auto components = entity["components"].get_array(); !components.error()) {
    for (const auto component_json : components.value_unsafe()) {
      auto component = component_json.get_object();
      if (component.error()) {
        continue;
      }

      for (const auto field : component.value_unsafe()) {
        const auto where = fmt::format("entity '{}', {}", path, std::string_view(field.key));
        collect_field_references(field.value, where, references);
      }
    }
  }

  if (auto children = entity["children"].get_array(); !children.error()) {
    for (const auto child : children.value_unsafe()) {
      collect_entity_references(child, path, references);
    }
  }
}

auto compile_scene(std::string_view source) -> std::expected<std::string, std::string> {
  ZoneScoped;

  // the whole document up front: the loader reads on demand, so a truncated or hand-broken file would only trip it
  // partway through building the scene, in the shipped game
  auto parser = simdjson::dom::parser{};
  const auto padded = simdjson::padded_string(source);
  const auto document = parser.parse(padded);
  if (document.error()) {
    return std::unexpected(std::string(simdjson::error_message(document.error())));
  }

  if (!document.value_unsafe().is_object()) {
    return std::unexpected("A scene is a JSON object.");
  }

  auto minified = std::string(source.size(), '\0');
  auto minified_size = 0_sz;
  if (const auto error = simdjson::minify(source.data(), source.size(), minified.data(), minified_size); error) {
    return std::unexpected(std::string(simdjson::error_message(error)));
  }

  minified.resize(minified_size);

  return minified;
}

auto scene_references(std::string_view source) -> std::expected<std::vector<SceneReference>, std::string> {
  ZoneScoped;

  auto parser = simdjson::dom::parser{};
  const auto padded = simdjson::padded_string(source);
  const auto document = parser.parse(padded);
  if (document.error()) {
    return std::unexpected(std::string(simdjson::error_message(document.error())));
  }

  auto references = std::vector<SceneReference>{};
  if (auto scripts = document["scripts"].get_array(); !scripts.error()) {
    auto index = 0_sz;
    for (const auto script : scripts.value_unsafe()) {
      if (auto uuid_json = script["uuid"].get_string(); !uuid_json.error()) {
        if (const auto uuid = UUID::from_string(uuid_json.value_unsafe()); uuid.has_value() && *uuid) {
          references.push_back(
            SceneReference{
              .uuid = *uuid,
              .where = fmt::format("scripts[{}]", index),
              .expected_type = AssetType::Script,
            }
          );
        }
      }
      index += 1;
    }
  }

  if (auto entities = document["entities"].get_array(); !entities.error()) {
    for (const auto entity : entities.value_unsafe()) {
      collect_entity_references(entity, {}, references);
    }
  }

  return references;
}
} // namespace ox::rc
