#pragma once

#include <expected>
#include <simdjson.h>
#include <string>
#include <string_view>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct McpArgs {
  simdjson::dom::element element = {};
  std::string path = {};

  static auto from(simdjson::dom::element element, std::string_view path = {}) -> std::expected<McpArgs, std::string>;

  auto raw(this const McpArgs& self) -> const simdjson::dom::element& { return self.element; }
  auto has(this const McpArgs& self, std::string_view key) -> bool;

  auto str(this const McpArgs& self, std::string_view key) -> std::expected<std::string_view, std::string>;
  auto opt_str(this const McpArgs& self, std::string_view key) -> std::expected<option<std::string_view>, std::string>;

  auto integer(this const McpArgs& self, std::string_view key) -> std::expected<i64, std::string>;
  auto opt_integer(this const McpArgs& self, std::string_view key) -> std::expected<option<i64>, std::string>;

  auto number(this const McpArgs& self, std::string_view key) -> std::expected<f64, std::string>;
  auto opt_number(this const McpArgs& self, std::string_view key) -> std::expected<option<f64>, std::string>;

  auto boolean(this const McpArgs& self, std::string_view key) -> std::expected<bool, std::string>;
  auto opt_boolean(this const McpArgs& self, std::string_view key) -> std::expected<option<bool>, std::string>;

  auto object(this const McpArgs& self, std::string_view key) -> std::expected<McpArgs, std::string>;
  auto array(this const McpArgs& self, std::string_view key) -> std::expected<simdjson::dom::array, std::string>;
};
} // namespace ox
