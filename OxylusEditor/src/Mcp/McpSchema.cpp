#include "Mcp/McpSchema.hpp"

#include <tracy/Tracy.hpp>

#include "Utils/JsonWriter.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
static auto describe_raw(std::string_view description, std::string_view raw) -> std::string {
  std::string out = "{\"description\":\"" + StringUtils::escape_str(description) + "\"";

  const auto open = raw.find('{');
  const auto close = raw.rfind('}');
  if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
    const auto inner = raw.substr(open + 1, close - open - 1);
    if (inner.find_first_not_of(" \t\r\n") != std::string_view::npos) {
      out += ',';
      out += inner;
    }
  }

  out += '}';
  return out;
}

static auto add_property(
  SchemaBuilder& self, std::string_view name, std::string_view description, std::string_view type, bool is_required
) -> SchemaBuilder::Property& {
  auto& property = self.properties.emplace_back();
  property.name = name;
  property.description = description;
  property.type = type;
  property.is_required = is_required;
  return property;
}

auto SchemaBuilder::string(
  this SchemaBuilder self,
  std::string_view name,
  std::string_view description,
  bool is_required,
  std::vector<std::string> enum_values
) -> SchemaBuilder {
  add_property(self, name, description, "string", is_required).enum_values = std::move(enum_values);
  return self;
}

auto SchemaBuilder::integer(
  this SchemaBuilder self,
  std::string_view name,
  std::string_view description,
  bool is_required,
  option<i64> minimum,
  option<i64> maximum
) -> SchemaBuilder {
  auto& property = add_property(self, name, description, "integer", is_required);
  property.minimum = minimum;
  property.maximum = maximum;
  return self;
}

auto SchemaBuilder::number(
  this SchemaBuilder self, std::string_view name, std::string_view description, bool is_required
) -> SchemaBuilder {
  add_property(self, name, description, "number", is_required);
  return self;
}

auto SchemaBuilder::boolean(
  this SchemaBuilder self, std::string_view name, std::string_view description, bool is_required
) -> SchemaBuilder {
  add_property(self, name, description, "boolean", is_required);
  return self;
}

auto SchemaBuilder::object(
  this SchemaBuilder self,
  std::string_view name,
  std::string_view description,
  std::string_view raw_schema,
  bool is_required
) -> SchemaBuilder {
  add_property(self, name, description, "object", is_required).raw_schema = raw_schema;
  return self;
}

auto SchemaBuilder::array(
  this SchemaBuilder self,
  std::string_view name,
  std::string_view description,
  std::string_view raw_item_schema,
  bool is_required
) -> SchemaBuilder {
  add_property(self, name, description, "array", is_required).raw_schema = raw_item_schema;
  return self;
}

auto SchemaBuilder::build(this const SchemaBuilder& self) -> std::string {
  ZoneScoped;

  JsonWriter writer;
  writer.is_pretty = false;

  writer.begin_obj();
  writer.key("type");
  writer.str("object");

  writer.key("properties");
  writer.begin_obj();
  for (const auto& property : self.properties) {
    writer.key(property.name);

    if (property.type == "object") {
      writer.raw(describe_raw(property.description, property.raw_schema));
      continue;
    }

    writer.begin_obj();
    writer.key("type");
    writer.str(property.type);
    writer.key("description");
    writer.str(property.description);

    if (!property.enum_values.empty()) {
      writer.key("enum");
      writer.begin_array();
      for (const auto& value : property.enum_values) {
        writer.str(value);
      }
      writer.end_array();
    }

    if (property.minimum.has_value()) {
      writer["minimum"] = property.minimum.value();
    }

    if (property.maximum.has_value()) {
      writer["maximum"] = property.maximum.value();
    }

    if (property.type == "array") {
      writer.key("items");
      writer.raw(property.raw_schema.empty() ? "{}" : property.raw_schema);
    }

    writer.end_obj();
  }
  writer.end_obj();

  bool has_required = false;
  for (const auto& property : self.properties) {
    has_required = has_required || property.is_required;
  }

  if (has_required) {
    writer.key("required");
    writer.begin_array();
    for (const auto& property : self.properties) {
      if (property.is_required) {
        writer.str(property.name);
      }
    }
    writer.end_array();
  }

  writer["additionalProperties"] = false;
  writer.end_obj();

  return writer.stream.str();
}
} // namespace ox
