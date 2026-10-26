#pragma once

#include <expected>
#include <slang.h>
#include <string>
#include <string_view>
#include <vir/ShaderLayout.hpp>

namespace ox::rc {
// everything the render graph needs to build a pipeline around this entry point, read off Slang's
// reflection so the engine never parses SPIR-V. Needs a linked program with one entry point
auto reflect_shader_layout(slang::IComponentType* linked_program, std::string_view entry_point_name)
  -> std::expected<vir::ShaderLayout, std::string>;

// drops what codegen left out, so descriptors an imported module declares but the entry point never
// touches can't collide with its own. Call once the entry point's code was generated
auto remove_unused_bindings(vir::ShaderLayout& layout, slang::IComponentType* linked_program) -> void;
} // namespace ox::rc
