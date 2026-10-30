#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "Core/UUID.hpp"

namespace ox::rc {
// Checks `source` is a whole, well formed scene document and strips it down to what the loader reads: the JSON,
// minified. A script entry naming a Lua asset is dropped with a line in `warnings`. An error comes back as simdjson
// phrased it.
auto compile_scene(std::string_view source, std::vector<std::string>& warnings)
  -> std::expected<std::string, std::string>;

// An asset a scene names, with where it names it so an error can point at the entity and field
struct SceneReference {
  UUID uuid = UUID(nullptr);
  std::string where = {};
};

// The assets `source` names, by the loader's own rule: a set UUID in a component field. The JSON
// keeps no field types, but the serializer writes a UUID field as the canonical string and nothing else in that form.
auto scene_references(std::string_view source) -> std::expected<std::vector<SceneReference>, std::string>;
} // namespace ox::rc
