#pragma once

#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
enum class ScriptID : u64 { Invalid = std::numeric_limits<u64>::max() };

// The script asset: source only, never executed. Each scene instantiates its own LuaSystem from this, so two scenes
// running the same script never share an environment.
struct LuaScript {
  // the file a source script is read from, or for a cooked one the virtual path of the source it was cooked from
  std::filesystem::path path = {};
  // the cooked chunk, which names its own source. Empty unless the script came out of a pack.
  std::vector<u8> bytecode = {};
  // Set only when the script came from memory instead of a file.
  ox::option<std::string> source = {};
};
} // namespace ox
