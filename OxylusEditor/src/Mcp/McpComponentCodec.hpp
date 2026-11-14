#pragma once

#include <expected>
#include <flecs.h>
#include <simdjson.h>
#include <string>
#include <vector>

#include "Core/UUID.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Command.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
// writes the component as one JSON object value, `{}` when `data` is nullptr
auto component_to_json(JsonWriter& writer, flecs::world& world, flecs::id component, const void* data) -> void;

// every non-null uuid the component names, repeats included
auto collect_asset_fields(flecs::world& world, flecs::id component, const void* data, std::vector<UUID>& out) -> void;

// `target` is a clone of the live component and is edited in place, the result is the list of warnings
auto patch_component(
  Scene& scene, flecs::entity entity, flecs::id component, simdjson::dom::element patch, ComponentBytes& target
) -> std::expected<std::vector<std::string>, std::string>;

auto component_schema_json(JsonWriter& writer, Scene& scene, flecs::id component) -> void;
} // namespace ox
