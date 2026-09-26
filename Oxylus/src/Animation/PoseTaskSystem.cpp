#include "Animation/PoseTaskSystem.hpp"

#include <algorithm>

#include "Animation/AnimationClip.hpp"
#include "Animation/Blender.hpp"
#include "Animation/Skeleton.hpp"

namespace ox {
auto PoseTaskSystem::reset(this PoseTaskSystem& self) -> void {
  self.tasks.clear();
  self.task_results.clear();
}

auto PoseTaskSystem::register_sample(this PoseTaskSystem& self, const AnimationClip* clip, const f32 time) -> i8 {
  if (clip == nullptr || self.tasks.size() >= 127) {
    return INVALID_POSE_TASK;
  }

  self.tasks.emplace_back(PoseTask{.type = PoseTaskType::Sample, .clip = clip, .time = time});
  return static_cast<i8>(self.tasks.size() - 1);
}

auto PoseTaskSystem::register_blend(this PoseTaskSystem& self, const i8 source, const i8 target, const f32 weight)
  -> i8 {
  if (source == INVALID_POSE_TASK) {
    return target;
  }
  if (target == INVALID_POSE_TASK || self.tasks.size() >= 127) {
    return source;
  }

  auto task = PoseTask{.type = PoseTaskType::Blend, .weight = weight};
  task.dependencies.push_back(source);
  task.dependencies.push_back(target);
  self.tasks.emplace_back(std::move(task));

  return static_cast<i8>(self.tasks.size() - 1);
}

auto PoseTaskSystem::acquire_pose_buffer(this PoseTaskSystem& self, const Skeleton& skeleton) -> u32 {
  for (auto i = 0_u32; i < self.pose_buffers.size(); ++i) {
    if (!self.pose_buffer_in_use[i]) {
      self.pose_buffer_in_use[i] = true;
      auto& pose = self.pose_buffers[i];
      if (pose.skeleton != &skeleton || pose.bone_count() != skeleton.bone_count()) {
        pose.reset(&skeleton);
      }
      return i;
    }
  }

  self.pose_buffers.emplace_back().reset(&skeleton);
  self.pose_buffer_in_use.push_back(true);
  return static_cast<u32>(self.pose_buffers.size() - 1);
}

auto PoseTaskSystem::release_pose_buffer(this PoseTaskSystem& self, const u32 index) -> void {
  if (index < self.pose_buffer_in_use.size()) {
    self.pose_buffer_in_use[index] = false;
  }
}

auto PoseTaskSystem::execute(this PoseTaskSystem& self, const Skeleton& skeleton, Pose& out) -> void {
  ZoneScoped;

  if (self.tasks.empty()) {
    return;
  }

  std::fill(self.pose_buffer_in_use.begin(), self.pose_buffer_in_use.end(), false);
  self.task_results.assign(self.tasks.size(), ~0_u32);

  // tasks can only depend on lower indices, so one forward pass is a valid topological order
  for (auto i = 0_u32; i < self.tasks.size(); ++i) {
    const auto& task = self.tasks[i];
    const auto result_index = self.acquire_pose_buffer(skeleton);

    switch (task.type) {
      case PoseTaskType::Sample: {
        task.clip->sample(task.time, self.pose_buffers[result_index]);
        break;
      }
      case PoseTaskType::Blend: {
        const auto source_index = self.task_results[static_cast<usize>(task.dependencies[0])];
        const auto target_index = self.task_results[static_cast<usize>(task.dependencies[1])];
        animation::blend(
          self.pose_buffers[source_index],
          self.pose_buffers[target_index],
          task.weight,
          {},
          self.pose_buffers[result_index]
        );
        break;
      }
    }

    self.task_results[i] = result_index;
    for (const auto dependency : task.dependencies) {
      self.release_pose_buffer(self.task_results[static_cast<usize>(dependency)]);
    }
  }

  const auto root_index = self.task_results.back();
  out.skeleton = &skeleton;
  out.parent_space_transforms = self.pose_buffers[root_index].parent_space_transforms;
  out.clear_model_space_transforms();
  out.state = self.pose_buffers[root_index].state;
}
} // namespace ox
