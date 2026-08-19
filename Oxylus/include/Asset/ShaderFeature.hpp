#pragma once

#include "Core/Enum.hpp"
#include "Core/Types.hpp"

namespace ox {
// What a shader program needs from its environment. `MeshShaders` and `RayTracing` are optional
// device capabilities, so a pipeline requiring one the device lacks is skipped at creation time
// rather than failing the frame. `Bindless` is always satisfiable and instead selects the pipeline
// layout, so it is never device-gated.
enum class ShaderFeatureFlag : u32 {
  None = 0,
  MeshShaders = 1 << 0,
  RayTracing = 1 << 1,
  Bindless = 1 << 2,
};
consteval void enable_bitmask(ShaderFeatureFlag);
} // namespace ox
