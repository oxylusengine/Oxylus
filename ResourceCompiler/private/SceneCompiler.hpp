#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Core/Option.hpp"
#include "Core/UUID.hpp"

namespace ox::rc {
// Checks `source` is a whole, well formed scene document and strips it down to what the loader reads: the JSON,
// minified. An error comes back as simdjson phrased it.
auto compile_scene(std::string_view source) -> std::expected<std::string, std::string>;

// An asset a scene names, with where it names it so an error can point at the entity and field
struct SceneReference {
  UUID uuid = UUID(nullptr);
  std::string where = {};
  // set where the scene says what it has to be, a scene script
  option<AssetType> expected_type = nullopt;
};

// The assets `source` names, by the loader's own rule: a set UUID in a component field, or a scene script. The JSON
// keeps no field types, but the serializer writes a UUID field as the canonical string and nothing else in that form.
auto scene_references(std::string_view source) -> std::expected<std::vector<SceneReference>, std::string>;
} // namespace ox::rc
