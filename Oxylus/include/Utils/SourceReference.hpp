#pragma once

#include <ankerl/svector.h>
#include <string_view>

#include "Core/Types.hpp"

namespace ox {
struct SourceReference {
  // points into the text it was found in
  std::string_view path = {};
  u32 line = 0;
  // 0 when the reference names none
  u32 column = 0;
};

// MSBuild's `path(line,column): ...` at the start of a line and .NET stack frames' `at ... in path:line N`, in the
// order they appear
auto find_source_references(std::string_view text) -> ankerl::svector<SourceReference, 4>;
} // namespace ox
