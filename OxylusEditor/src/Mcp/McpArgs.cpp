#include "Mcp/McpArgs.hpp"

#include <fmt/format.h>
#include <span>

#include "Utils/StringUtils.hpp"

namespace ox {
static auto type_name(simdjson::dom::element_type type) -> std::string_view {
  switch (type) {
    case simdjson::dom::element_type::ARRAY     : return "array";
    case simdjson::dom::element_type::OBJECT    : return "object";
    case simdjson::dom::element_type::INT64     : return "integer";
    case simdjson::dom::element_type::UINT64    : return "integer";
    case simdjson::dom::element_type::DOUBLE    : return "number";
    case simdjson::dom::element_type::STRING    : return "string";
    case simdjson::dom::element_type::BOOL      : return "boolean";
    case simdjson::dom::element_type::NULL_VALUE: return "null";
  }

  return "unknown";
}

static auto field_path(const McpArgs& args, std::string_view key) -> std::string {
  if (args.path.empty()) {
    return std::string(key);
  }

  return fmt::format("{}.{}", args.path, key);
}

static auto field_error(
  const McpArgs& args, std::string_view key, std::string_view expected, simdjson::dom::element value
) -> std::string {
  return fmt::format("argument '{}': expected {}, got {}", field_path(args, key), expected, type_name(value.type()));
}

static auto find_field(const McpArgs& args, std::string_view key) -> option<simdjson::dom::element> {
  simdjson::dom::object object;
  if (args.element.get(object)) {
    return nullopt;
  }

  simdjson::dom::element value;
  if (object.at_key(key).get(value)) {
    return nullopt;
  }

  if (value.type() == simdjson::dom::element_type::NULL_VALUE) {
    return nullopt;
  }

  return value;
}

// false when the key is absent
static auto read_floats(const McpArgs& args, std::string_view key, std::span<f32> out)
  -> std::expected<bool, std::string> {
  const auto field = find_field(args, key);
  if (!field.has_value()) {
    return false;
  }

  const auto mismatch = fmt::format("{}: expected an array of {} numbers", field_path(args, key), out.size());
  simdjson::dom::array array;
  if (field->get(array) || array.size() != out.size()) {
    return std::unexpected(mismatch);
  }

  for (usize i = 0; i < out.size(); i++) {
    auto value = 0.0;
    if (array.at(i).get(value)) {
      return std::unexpected(mismatch);
    }

    out[i] = static_cast<f32>(value);
  }

  return true;
}

static auto required_error(const McpArgs& args, std::string_view key, std::string_view expected) -> std::string {
  return fmt::format("argument '{}': required {} is missing", field_path(args, key), expected);
}

auto McpArgs::from(simdjson::dom::element element, std::string_view path) -> std::expected<McpArgs, std::string> {
  if (element.type() != simdjson::dom::element_type::OBJECT) {
    return std::unexpected(
      fmt::format("{}: expected object, got {}", path.empty() ? "arguments" : path, type_name(element.type()))
    );
  }

  return McpArgs{.element = element, .path = std::string(path)};
}

auto McpArgs::has(this const McpArgs& self, std::string_view key) -> bool { return find_field(self, key).has_value(); }

auto McpArgs::opt_str(this const McpArgs& self, std::string_view key)
  -> std::expected<option<std::string_view>, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return option<std::string_view>(nullopt);
  }

  std::string_view value;
  if (field->get(value)) {
    return std::unexpected(field_error(self, key, "string", *field));
  }

  return option<std::string_view>(value);
}

auto McpArgs::opt_vec2(this const McpArgs& self, std::string_view key)
  -> std::expected<option<glm::vec2>, std::string> {
  auto value = glm::vec2();
  const auto is_present = read_floats(self, key, std::span(&value.x, 2));
  if (!is_present) {
    return std::unexpected(is_present.error());
  }

  return is_present.value() ? option<glm::vec2>(value) : nullopt;
}

auto McpArgs::opt_vec3(this const McpArgs& self, std::string_view key)
  -> std::expected<option<glm::vec3>, std::string> {
  auto value = glm::vec3();
  const auto is_present = read_floats(self, key, std::span(&value.x, 3));
  if (!is_present) {
    return std::unexpected(is_present.error());
  }

  return is_present.value() ? option<glm::vec3>(value) : nullopt;
}

auto McpArgs::vec3(this const McpArgs& self, std::string_view key) -> std::expected<glm::vec3, std::string> {
  const auto value = self.opt_vec3(key);
  if (!value) {
    return std::unexpected(value.error());
  }

  if (!value->has_value()) {
    return std::unexpected(required_error(self, key, "array of 3 numbers"));
  }

  return value->value();
}

auto McpArgs::cursor(this const McpArgs& self) -> std::expected<usize, std::string> {
  const auto text = self.opt_str("cursor");
  if (!text) {
    return std::unexpected(text.error());
  }

  if (!text->has_value()) {
    return usize{0};
  }

  const auto value = StringUtils::parse_integer<usize>(text->value());
  if (!value.has_value()) {
    return std::unexpected("argument 'cursor': not a cursor returned by this tool");
  }

  return value.value();
}

auto McpArgs::str(this const McpArgs& self, std::string_view key) -> std::expected<std::string_view, std::string> {
  const auto value = self.opt_str(key);
  if (!value) {
    return std::unexpected(value.error());
  }

  if (!value->has_value()) {
    return std::unexpected(required_error(self, key, "string"));
  }

  return value->value();
}

auto McpArgs::opt_integer(this const McpArgs& self, std::string_view key) -> std::expected<option<i64>, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return option<i64>(nullopt);
  }

  i64 value = 0;
  if (field->get(value)) {
    return std::unexpected(field_error(self, key, "integer", *field));
  }

  return option<i64>(value);
}

auto McpArgs::integer(this const McpArgs& self, std::string_view key) -> std::expected<i64, std::string> {
  const auto value = self.opt_integer(key);
  if (!value) {
    return std::unexpected(value.error());
  }

  if (!value->has_value()) {
    return std::unexpected(required_error(self, key, "integer"));
  }

  return value->value();
}

auto McpArgs::opt_number(this const McpArgs& self, std::string_view key) -> std::expected<option<f64>, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return option<f64>(nullopt);
  }

  f64 value = 0.0;
  if (field->get(value)) {
    return std::unexpected(field_error(self, key, "number", *field));
  }

  return option<f64>(value);
}

auto McpArgs::number(this const McpArgs& self, std::string_view key) -> std::expected<f64, std::string> {
  const auto value = self.opt_number(key);
  if (!value) {
    return std::unexpected(value.error());
  }

  if (!value->has_value()) {
    return std::unexpected(required_error(self, key, "number"));
  }

  return value->value();
}

auto McpArgs::opt_boolean(this const McpArgs& self, std::string_view key) -> std::expected<option<bool>, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return option<bool>(nullopt);
  }

  bool value = false;
  if (field->get(value)) {
    return std::unexpected(field_error(self, key, "boolean", *field));
  }

  return option<bool>(value);
}

auto McpArgs::boolean(this const McpArgs& self, std::string_view key) -> std::expected<bool, std::string> {
  const auto value = self.opt_boolean(key);
  if (!value) {
    return std::unexpected(value.error());
  }

  if (!value->has_value()) {
    return std::unexpected(required_error(self, key, "boolean"));
  }

  return value->value();
}

auto McpArgs::object(this const McpArgs& self, std::string_view key) -> std::expected<McpArgs, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return std::unexpected(required_error(self, key, "object"));
  }

  if (field->type() != simdjson::dom::element_type::OBJECT) {
    return std::unexpected(field_error(self, key, "object", *field));
  }

  return McpArgs{.element = *field, .path = field_path(self, key)};
}

auto McpArgs::array(this const McpArgs& self, std::string_view key)
  -> std::expected<simdjson::dom::array, std::string> {
  const auto field = find_field(self, key);
  if (!field.has_value()) {
    return std::unexpected(required_error(self, key, "array"));
  }

  simdjson::dom::array value;
  if (field->get(value)) {
    return std::unexpected(field_error(self, key, "array", *field));
  }

  return value;
}
} // namespace ox
