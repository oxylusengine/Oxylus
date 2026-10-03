#pragma once

#include <ankerl/svector.h>
#include <vector>

#include "Animation/Fwd.hpp"
#include "Animation/Pose.hpp"

namespace ox {
// two-phase so that a graph runtime can later emit into the same system rather than replace it
enum class PoseTaskType : u8 {
  Sample,
  Blend,
};

struct PoseTask {
  PoseTaskType type = PoseTaskType::Sample;
  ankerl::svector<i8, 2> dependencies = {};

  const AnimationClip* clip = nullptr;
  f32 time = 0.f;
  f32 weight = 0.f;
};

constexpr static i8 INVALID_POSE_TASK = -1;

class PoseTaskSystem {
public:
  auto reset(this PoseTaskSystem& self) -> void;

  auto register_sample(this PoseTaskSystem& self, const AnimationClip* clip, f32 time) -> i8;
  auto register_blend(this PoseTaskSystem& self, i8 source, i8 target, f32 weight) -> i8;

  auto is_empty(this const PoseTaskSystem& self) -> bool { return self.tasks.empty(); }

  auto execute(this PoseTaskSystem& self, const Skeleton& skeleton, Pose& out) -> void;

private:
  auto acquire_pose_buffer(this PoseTaskSystem& self, const Skeleton& skeleton) -> u32;
  auto release_pose_buffer(this PoseTaskSystem& self, u32 index) -> void;

  std::vector<PoseTask> tasks = {};
  std::vector<Pose> pose_buffers = {};
  std::vector<bool> pose_buffer_in_use = {};
  // task index -> the pose buffer holding its result while dependents still need it
  std::vector<u32> task_results = {};
};
} // namespace ox
