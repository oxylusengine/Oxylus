#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace ox::rc {
// Checks `source` is a whole, well formed scene document and strips it down to what the loader reads: the JSON,
// minified. An error comes back as simdjson phrased it.
auto compile_scene(std::string_view source) -> std::expected<std::string, std::string>;
} // namespace ox::rc
