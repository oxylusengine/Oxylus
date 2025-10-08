#pragma once

#include <string>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct AppCommandLineArgs {
  struct Arg {
    std::string arg_str;
    u32 arg_index;
  };

  std::vector<Arg> args = {};

  AppCommandLineArgs() = default;
  AppCommandLineArgs(const int argc, char** argv) {
    for (int i = 0; i < argc; i++)
      args.emplace_back(Arg{.arg_str = argv[i], .arg_index = (u32)i});
  }

  bool contains(const std::string_view arg) const {
    for (const auto& a : args) {
      if (a.arg_str == arg) {
        return true;
      }
    }
    return false;
  }

  ox::option<Arg> get(const u32 index) const {
    try {
      return args.at(index);
    } catch ([[maybe_unused]]
             std::exception& exception) {
      return ox::nullopt;
    }
  }

  ox::option<u32> get_index(const std::string_view arg) const {
    for (const auto& a : args) {
      if (a.arg_str == arg) {
        return a.arg_index;
      }
    }
    return ox::nullopt;
  }
};
} // namespace ox
