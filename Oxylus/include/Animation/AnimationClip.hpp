#pragma once

#include <span>
#include <string>
#include <vector>

#include "Animation/Quantization.hpp"
#include "Core/UUID.hpp"

namespace ox {
struct BoneTransform;
struct Pose;

// a component that never changes across the clip is "static": its value lives in the range start,
// or in `constant_rotation`, and costs zero bytes per frame
struct TrackDefinition {
  animation::FloatRange translation_range_x = {};
  animation::FloatRange translation_range_y = {};
  animation::FloatRange translation_range_z = {};
  animation::FloatRange scale_range = {};
  animation::EncodedQuaternion constant_rotation = {};

  // in u16 units, from the start of a frame block
  u32 track_read_offset = 0;

  bool is_rotation_static = false;
  bool is_translation_static = false;
  bool is_scale_static = false;

  auto static_translation(this const TrackDefinition& self) -> glm::vec3 {
    return {self.translation_range_x.start, self.translation_range_y.start, self.translation_range_z.start};
  }

  auto static_scale(this const TrackDefinition& self) -> f32 { return self.scale_range.start; }

  auto per_frame_u16_count(this const TrackDefinition& self) -> u32 {
    return (self.is_rotation_static ? 0_u32 : 3_u32) + (self.is_translation_static ? 0_u32 : 3_u32) +
           (self.is_scale_static ? 0_u32 : 1_u32);
  }
};

struct AnimationClip {
  std::string name = {};
  UUID skeleton_uuid = {};
  u32 frame_count = 0;
  f32 duration = 0.f;

  std::vector<TrackDefinition> track_defs = {};
  // frame-major: `compressed_pose_offsets[frame]` starts a whole-pose block, and within it a
  // bone's data sits at `track_defs[bone].track_read_offset`
  std::vector<u16> compressed_pose_data = {};
  std::vector<u32> compressed_pose_offsets = {};

  auto bone_count(this const AnimationClip& self) -> u32 { return static_cast<u32>(self.track_defs.size()); }
  auto fps(this const AnimationClip& self) -> f32;

  auto read_compressed_pose(this const AnimationClip& self, u32 frame, std::span<BoneTransform> out) -> void;
  auto sample(this const AnimationClip& self, f32 time, Pose& out) -> void;
};
namespace animation {
// `sampled[frame * bone_count + bone]`, in parent space
auto compress_tracks(AnimationClip& clip, std::span<const BoneTransform> sampled, u32 bone_count, u32 frame_count)
  -> void;
} // namespace animation
} // namespace ox
