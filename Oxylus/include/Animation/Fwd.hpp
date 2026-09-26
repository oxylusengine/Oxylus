#pragma once

#include "Core/Types.hpp"

namespace ox {
struct Skeleton;
struct AnimationClip;
struct Pose;
struct BoneTransform;

// defined in Asset/Model.hpp, which includes this header
enum class ModelID : u64;

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
  // the SlotMap slot this instance came from, which survives a rebuild of the instance array and is
  // therefore what the skinned acceleration structure pool keys its entries on
  u32 mesh_instance_slot = 0;
  ModelID model_id = static_cast<ModelID>(~0_u64);
  u32 mesh_node_index = 0;
  // LOD0 of the mesh, re-read every frame because the model may finish loading long after the
  // instance array was last rebuilt
  u64 index_address = 0;
  u32 index_count = 0;
  // a pose that did not move reskins to the same vertices, so its structure is still valid
  bool pose_advanced = false;
  AnimationInstanceID animation_instance_id = AnimationInstanceID::Invalid;
};
} // namespace ox
