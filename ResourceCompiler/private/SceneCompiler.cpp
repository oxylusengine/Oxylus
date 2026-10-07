#include "SceneCompiler.hpp"

#include <simdjson.h>

#include "Core/Types.hpp"

namespace ox::rc {
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
} // namespace ox::rc
