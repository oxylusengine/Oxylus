#pragma once

#include <string_view>

#include "Core/Types.hpp"

namespace ox {
struct EngineBuildInfo {
  u32 major;
  u32 minor;
  u32 patch;
  std::string_view version;
  std::string_view full_version;
  std::string_view revision;
  std::string_view build_mode;
  bool dirty;
};

// describes the linked engine library, with strings that live for the lifetime of the program
auto engine_build_info() -> const EngineBuildInfo&;
} // namespace ox
