#pragma once

#include "Core/Enum.hpp"
#include "Core/Types.hpp"

namespace ox {
enum class ShaderFeatureFlag : u32 {
  None = 0,
  MeshShaders = 1 << 0,
  RayTracing = 1 << 1,
  RayTracingPipeline = 1 << 2,
  Bindless = 1 << 3,
};
consteval void enable_bitmask(ShaderFeatureFlag);
} // namespace ox
