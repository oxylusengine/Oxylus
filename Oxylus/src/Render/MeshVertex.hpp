#pragma once
#include <vuk/runtime/CommandBuffer.hpp>

namespace ox {
struct Vertex {
  glm::vec3 position;
  u32 normal;
  glm::vec2 uv;
};

inline auto vertex_pack = vuk::Packed{
    vuk::Format::eR32Sfloat, // 4 postition x
    vuk::Format::eR32Sfloat, // 4 postition y
    vuk::Format::eR32Sfloat, // 4 postition z
    vuk::Format::eR32Uint,   // 4 normal
    vuk::Format::eR32Sfloat, // 4 uv x
    vuk::Format::eR32Sfloat, // 4 uv y
};
} // namespace ox
