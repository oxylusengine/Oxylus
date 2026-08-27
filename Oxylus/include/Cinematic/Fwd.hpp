#pragma once

#include "Core/Types.hpp"

namespace ox {
struct Cinematic;
struct CinematicKey;
struct CameraWaypoint;
struct CinematicPropertyTrack;
struct CinematicCameraTrack;

enum class CinematicID : u64 { Invalid = ~0_u64 };
enum class CinematicInstanceID : u64 { Invalid = ~0_u64 };

enum class CinematicValueKind : u8 {
  Float = 0,
  Float2,
  Float3,
  Float4,
  Quat,
  Int,
  Bool,
  Enum,
  Count,
};

enum class CameraInterp : u8 { Linear = 0, CatmullRom, Count };
} // namespace ox
