#pragma once

#include <ranges>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct AppCommandLineArgs {
  std::vector<std::string_view> args = {};

  AppCommandLineArgs() = default;
  AppCommandLineArgs(const int argc, char** argv) {
    for (int i = 0; i < argc; i++)
      args.emplace_back(argv[i]);
  }

  bool contains(std::string_view arg) const {
    for (const auto& a : args) {
      if (a == arg) {
        return true;
      }
    }
    return false;
  }

  ox::option<std::string_view> get(const u32 index) const {
    if (index < args.size()) {
      return args.at(index);
    }

    return ox::nullopt;
  }

  ox::option<u32> get_index(const std::string_view arg) const {
    for (const auto& [a, i] : std::views::zip(args, std::views::iota(0_u32))) {
      if (a == arg) {
        return i;
      }
    }
    return ox::nullopt;
  }
};
} // namespace ox
