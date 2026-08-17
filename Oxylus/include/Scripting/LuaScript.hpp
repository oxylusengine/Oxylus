#pragma once

#include <filesystem>
#include <limits>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
enum class ScriptID : u64 { Invalid = std::numeric_limits<u64>::max() };

// The script asset: source only, never executed. Each scene instantiates its own LuaSystem from this, so two scenes
// running the same script never share an environment.
struct LuaScript {
  std::filesystem::path path = {};
  // Set only when the script came from memory instead of a file.
  ox::option<std::string> source = {};
};
} // namespace ox
