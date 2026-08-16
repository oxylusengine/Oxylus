#pragma once

#include <filesystem>
#include <glm/vec2.hpp>
#include <limits>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
enum class TerrainEditsID : u64 { Invalid = std::numeric_limits<u64>::max() };

// One terrain's brush strokes: a per-texel height delta and a per-texel splat override, both keyed
// to the terrain's texel grid. Held on the CPU — `Terrain` owns the GPU maps it paints into, so a
// play-mode scene copy can diverge from the asset without writing back through it.
struct TerrainEdits {
  glm::uvec2 resolution = {};
  std::vector<u8> height = {};
  std::vector<u8> splat = {};

  static auto read(const std::filesystem::path& path) -> option<TerrainEdits>;
  auto write(this const TerrainEdits& self, const std::filesystem::path& path) -> bool;

  auto is_empty(this const TerrainEdits& self) -> bool { return self.height.empty(); }
};
} // namespace ox
