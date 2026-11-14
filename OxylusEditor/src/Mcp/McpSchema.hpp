#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct SchemaBuilder {
  struct Property {
    std::string name = {};
    std::string description = {};
    std::string type = {};
    std::string raw_schema = {};
    std::vector<std::string> enum_values = {};
    option<i64> minimum = nullopt;
    option<i64> maximum = nullopt;
    bool is_required = false;
  };

  std::vector<Property> properties = {};

  auto string(
    this SchemaBuilder self,
    std::string_view name,
    std::string_view description,
    bool is_required,
    std::vector<std::string> enum_values = {}
  ) -> SchemaBuilder;

  auto integer(
    this SchemaBuilder self,
    std::string_view name,
    std::string_view description,
    bool is_required,
    option<i64> minimum = nullopt,
    option<i64> maximum = nullopt
  ) -> SchemaBuilder;

  auto number(this SchemaBuilder self, std::string_view name, std::string_view description, bool is_required)
    -> SchemaBuilder;

  auto boolean(this SchemaBuilder self, std::string_view name, std::string_view description, bool is_required)
    -> SchemaBuilder;

  // raw_schema is a complete schema object like {"type":"object",...}
  auto object(
    this SchemaBuilder self,
    std::string_view name,
    std::string_view description,
    std::string_view raw_schema,
    bool is_required
  ) -> SchemaBuilder;

  // raw_item_schema is a complete schema object like {"type":"string"}
  auto array(
    this SchemaBuilder self,
    std::string_view name,
    std::string_view description,
    std::string_view raw_item_schema,
    bool is_required
  ) -> SchemaBuilder;

  auto build(this const SchemaBuilder& self) -> std::string;
};
} // namespace ox
