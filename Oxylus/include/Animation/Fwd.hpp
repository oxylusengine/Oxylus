#pragma once

#include "Core/Types.hpp"

namespace ox {
struct Skeleton;
struct AnimationClip;
struct Pose;
struct BoneTransform;

enum class SkeletonID : u64 { Invalid = ~0_u64 };
enum class AnimationID : u64 { Invalid = ~0_u64 };
enum class AnimationInstanceID : u64 { Invalid = ~0_u64 };

constexpr static auto MAX_BONE_INFLUENCES = 4_u32;

struct SkinnedMeshInstance {
  u32 gpu_instance_index = 0;
  u32 vertex_count = 0;
  // into the scene-wide skinned vertex arena, in vertices
  u32 vertex_offset = 0;
  u32 bone_offset = 0;
  // zero until the skeleton asset finishes loading, so the instance renders in bind pose till then
  u32 bone_count = 0;
  AnimationInstanceID animation_instance_id = AnimationInstanceID::Invalid;
};
} // namespace ox
