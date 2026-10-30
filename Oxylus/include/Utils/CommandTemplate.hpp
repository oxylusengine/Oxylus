#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct CommandTemplateArgs {
  std::filesystem::path file = {};
  u32 line = 1;
  u32 column = 1;
  option<std::filesystem::path> root = nullopt;
};

auto expand_command_template(std::string_view command, const CommandTemplateArgs& args)
  -> std::expected<std::vector<std::string>, std::string>;
} // namespace ox
