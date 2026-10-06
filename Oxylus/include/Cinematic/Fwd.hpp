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

// the member's real layout, resolved from reflection at bind time, so an Int or Enum key lands in
// the 1, 2, 4 or 8 bytes the member actually has. `size == 0` means the kind's natural layout
struct CinematicStorage {
  u32 size = 0;
  bool is_signed = true;
};
} // namespace ox
