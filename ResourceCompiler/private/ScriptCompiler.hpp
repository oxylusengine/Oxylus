#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Types.hpp"

namespace ox::rc {
// lua's bytecode only loads into the release that wrote it, so this goes into a script's staleness hash
auto script_bytecode_version() -> u64;

// Parses `source` and dumps it as bytecode with its debug info, so errors still carry lines. `chunk_name` is what the
// bytecode reports as its source, the `@` prefix included. A syntax error comes back as lua phrased it.
auto compile_script(std::span<const u8> source, std::string_view chunk_name)
  -> std::expected<std::vector<u8>, std::string>;
} // namespace ox::rc
