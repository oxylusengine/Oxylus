#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <random>

#include "Animation/AnimationClip.hpp"
#include "Animation/Blender.hpp"
#include "Animation/BoneTransform.hpp"
#include "Animation/Pose.hpp"
#include "Animation/PoseTaskSystem.hpp"
#include "Animation/Quantization.hpp"
#include "Animation/Skeleton.hpp"

using namespace ox;

static auto quat_angle_between(const glm::quat& a, const glm::quat& b) -> f32 {
  return 2.f * glm::acos(glm::clamp(glm::abs(glm::dot(glm::normalize(a), glm::normalize(b))), 0.f, 1.f));
}

// two bones: a root and a child offset along +X
static auto make_test_skeleton() -> Skeleton {
  auto skeleton = Skeleton{};
  skeleton.bone_names = {"root", "child"};
  skeleton.parent_indices = {-1, 0};
  skeleton.parent_space_reference_pose = {
    BoneTransform::from_trs({0.f, 0.f, 0.f}, glm::quat::wxyz(1.f, 0.f, 0.f, 0.f), 1.f),
    BoneTransform::from_trs({1.f, 0.f, 0.f}, glm::quat::wxyz(1.f, 0.f, 0.f, 0.f), 1.f),
  };
  skeleton.inverse_bind_pose = {BoneTransform{}, BoneTransform{}};
  EXPECT_TRUE(skeleton.finalize());
  return skeleton;
}

// the child bone slides from x=1 to x=3 while the root spins a quarter turn about Y
static auto make_test_clip(const Skeleton& skeleton, const u32 frame_count, const f32 duration) -> AnimationClip {
  const auto bone_count = skeleton.bone_count();
  auto sampled = std::vector<BoneTransform>(static_cast<usize>(frame_count) * bone_count);

  for (auto frame = 0_u32; frame < frame_count; ++frame) {
    const auto t = static_cast<f32>(frame) / static_cast<f32>(frame_count - 1);
    sampled[frame * bone_count + 0] = BoneTransform::from_trs(
      {0.f, 0.f, 0.f},
      glm::angleAxis(t * glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f)),
      1.f
    );
    sampled[frame * bone_count + 1] = BoneTransform::from_trs(
      {1.f + 2.f * t, 0.f, 0.f},
      glm::quat::wxyz(1.f, 0.f, 0.f, 0.f),
      1.f
    );
  }

  auto clip = AnimationClip{};
  clip.frame_count = frame_count;
  clip.duration = duration;
  animation::compress_tracks(clip, sampled, bone_count, frame_count);
  return clip;
}

TEST(Quantization, FloatRangeRoundTrip) {
  auto rng = std::mt19937(1234);
  auto dist = std::uniform_real_distribution<f32>(-50.f, 50.f);

  for (auto i = 0; i < 2000; ++i) {
    const auto a = dist(rng);
    const auto b = dist(rng);
    const auto range = animation::FloatRange::from_bounds(glm::min(a, b), glm::max(a, b));
    if (range.length <= 0.f) {
      continue;
    }

    const auto value = glm::mix(range.start, range.start + range.length, dist(rng) / 100.f + 0.5f);
    const auto decoded = range.decode(range.encode(value));

    // 16 bits across the range, so the worst case is half a step
    EXPECT_NEAR(decoded, glm::clamp(value, range.start, range.start + range.length), range.length / 65535.f);
  }
}

TEST(Quantization, FloatRangeEndpointsAreExact) {
  const auto range = animation::FloatRange::from_bounds(-3.5f, 7.25f);
  EXPECT_NEAR(range.decode(range.encode(-3.5f)), -3.5f, 1e-4f);
  EXPECT_NEAR(range.decode(range.encode(7.25f)), 7.25f, 1e-4f);
}

TEST(Quantization, EncodedQuaternionRoundTrip) {
  auto rng = std::mt19937(4321);
  auto dist = std::uniform_real_distribution<f32>(-1.f, 1.f);

  auto worst = 0.f;
  for (auto i = 0; i < 5000; ++i) {
    const auto quat = glm::normalize(glm::quat::wxyz(dist(rng), dist(rng), dist(rng), dist(rng)));
    const auto decoded = animation::EncodedQuaternion::encode(quat).decode();
    worst = glm::max(worst, quat_angle_between(quat, decoded));
  }

  // smallest-three with 15 bits per component, so anything above a thousandth of a radian means
  // the encoding is wrong rather than merely lossy
  EXPECT_LT(worst, 1e-3f);
}

TEST(Quantization, EncodedQuaternionHandlesIdentity) {
  const auto identity = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
  EXPECT_LT(quat_angle_between(identity, animation::EncodedQuaternion::encode(identity).decode()), 1e-5f);
}

TEST(BoneTransform, InverseCancels) {
  const auto transform = BoneTransform::from_trs(
    {3.f, -2.f, 7.f},
    glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.f, 2.f, -3.f))),
    2.5f
  );

  const auto identity = transform * transform.inverse();
  EXPECT_LT(quat_angle_between(identity.rotation, glm::quat::wxyz(1.f, 0.f, 0.f, 0.f)), 1e-4f);
  EXPECT_NEAR(glm::length(identity.translation()), 0.f, 1e-4f);
  EXPECT_NEAR(identity.scale(), 1.f, 1e-4f);
}

TEST(BoneTransform, ComposeMatchesMatrixMultiply) {
  const auto a = BoneTransform::from_trs({1.f, 2.f, 3.f}, glm::angleAxis(0.4f, glm::vec3(0.f, 1.f, 0.f)), 2.f);
  const auto b = BoneTransform::from_trs({-4.f, 0.5f, 1.f}, glm::angleAxis(-1.1f, glm::vec3(1.f, 0.f, 0.f)), 0.5f);

  const auto composed = (a * b).to_mat4();
  const auto expected = a.to_mat4() * b.to_mat4();

  for (auto column = 0; column < 4; ++column) {
    for (auto row = 0; row < 4; ++row) {
      EXPECT_NEAR(composed[column][row], expected[column][row], 1e-4f) << "column " << column << " row " << row;
    }
  }
}

TEST(Skeleton, RejectsChildBeforeParent) {
  auto skeleton = Skeleton{};
  skeleton.bone_names = {"child", "root"};
  skeleton.parent_indices = {1, -1};
  skeleton.parent_space_reference_pose = {BoneTransform{}, BoneTransform{}};
  skeleton.inverse_bind_pose = {BoneTransform{}, BoneTransform{}};

  EXPECT_FALSE(skeleton.finalize());
}

TEST(Skeleton, ModelSpaceReferencePoseAccumulates) {
  const auto skeleton = make_test_skeleton();
  EXPECT_NEAR(skeleton.model_space_reference_pose[1].translation().x, 1.f, 1e-5f);
}

TEST(AnimationClip, SamplesKeyframesExactly) {
  const auto skeleton = make_test_skeleton();
  const auto clip = make_test_clip(skeleton, 5, 1.f);

  auto pose = Pose{};
  pose.reset(&skeleton, Pose::State::ReferencePose);

  clip.sample(0.f, pose);
  EXPECT_NEAR(pose.parent_space_transforms[1].translation().x, 1.f, 1e-3f);
  EXPECT_LT(quat_angle_between(pose.parent_space_transforms[0].rotation, glm::quat::wxyz(1.f, 0.f, 0.f, 0.f)), 1e-3f);

  clip.sample(1.f, pose);
  EXPECT_NEAR(pose.parent_space_transforms[1].translation().x, 3.f, 1e-3f);
  EXPECT_LT(
    quat_angle_between(
      pose.parent_space_transforms[0].rotation,
      glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f))
    ),
    1e-3f
  );
}

TEST(AnimationClip, InterpolatesBetweenKeyframes) {
  const auto skeleton = make_test_skeleton();
  // two frames only, so the midpoint is pure interpolation rather than a stored keyframe
  const auto clip = make_test_clip(skeleton, 2, 1.f);

  auto pose = Pose{};
  pose.reset(&skeleton, Pose::State::ReferencePose);
  clip.sample(0.5f, pose);

  EXPECT_NEAR(pose.parent_space_transforms[1].translation().x, 2.f, 1e-3f);
  EXPECT_LT(
    quat_angle_between(
      pose.parent_space_transforms[0].rotation,
      glm::angleAxis(glm::quarter_pi<f32>(), glm::vec3(0.f, 1.f, 0.f))
    ),
    1e-3f
  );
}

TEST(AnimationClip, StaticTracksCostNothingPerFrame) {
  const auto skeleton = make_test_skeleton();
  const auto clip = make_test_clip(skeleton, 8, 1.f);

  // the child never rotates or scales, and the root never translates or scales
  EXPECT_TRUE(clip.track_defs[1].is_rotation_static);
  EXPECT_TRUE(clip.track_defs[1].is_scale_static);
  EXPECT_TRUE(clip.track_defs[0].is_translation_static);
  EXPECT_TRUE(clip.track_defs[0].is_scale_static);

  EXPECT_EQ(clip.track_defs[0].per_frame_u16_count(), 3_u32);
  EXPECT_EQ(clip.track_defs[1].per_frame_u16_count(), 3_u32);
}

TEST(Pose, ModelSpaceFollowsParentChain) {
  const auto skeleton = make_test_skeleton();
  auto pose = Pose{};
  pose.reset(&skeleton, Pose::State::ReferencePose);

  // quarter turn about Y takes the child's local +X offset onto -Z
  pose.parent_space_transforms[0].rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f));
  pose.calculate_model_space_transforms();

  const auto child = pose.model_space_transforms[1].translation();
  EXPECT_NEAR(child.x, 0.f, 1e-4f);
  EXPECT_NEAR(child.z, -1.f, 1e-4f);
}

TEST(Blender, WeightEndpointsPickOperandsExactly) {
  const auto skeleton = make_test_skeleton();

  auto source = Pose{};
  source.reset(&skeleton, Pose::State::ReferencePose);
  source.parent_space_transforms[1].translation_scale.x = 1.f;

  auto target = Pose{};
  target.reset(&skeleton, Pose::State::ReferencePose);
  target.parent_space_transforms[1].translation_scale.x = 5.f;

  auto out = Pose{};
  animation::blend(source, target, 0.f, {}, out);
  EXPECT_NEAR(out.parent_space_transforms[1].translation().x, 1.f, 1e-5f);

  animation::blend(source, target, 1.f, {}, out);
  EXPECT_NEAR(out.parent_space_transforms[1].translation().x, 5.f, 1e-5f);

  animation::blend(source, target, 0.25f, {}, out);
  EXPECT_NEAR(out.parent_space_transforms[1].translation().x, 2.f, 1e-5f);
}

TEST(PoseTaskSystem, CrossfadeBlendsTwoSamples) {
  const auto skeleton = make_test_skeleton();
  const auto clip = make_test_clip(skeleton, 5, 1.f);

  auto task_system = PoseTaskSystem{};
  const auto from = task_system.register_sample(&clip, 0.f);
  const auto to = task_system.register_sample(&clip, 1.f);
  const auto blended = task_system.register_blend(from, to, 0.5f);
  ASSERT_NE(blended, INVALID_POSE_TASK);

  auto out = Pose{};
  out.reset(&skeleton, Pose::State::ReferencePose);
  task_system.execute(skeleton, out);

  // halfway between x=1 and x=3
  EXPECT_NEAR(out.parent_space_transforms[1].translation().x, 2.f, 1e-3f);
}

TEST(PoseTaskSystem, BlendWithOneMissingInputPassesTheOtherThrough) {
  const auto skeleton = make_test_skeleton();
  const auto clip = make_test_clip(skeleton, 5, 1.f);

  auto task_system = PoseTaskSystem{};
  const auto missing = task_system.register_sample(nullptr, 0.f);
  EXPECT_EQ(missing, INVALID_POSE_TASK);

  const auto present = task_system.register_sample(&clip, 1.f);
  EXPECT_EQ(task_system.register_blend(missing, present, 0.5f), present);
  EXPECT_EQ(task_system.register_blend(present, missing, 0.5f), present);
}
