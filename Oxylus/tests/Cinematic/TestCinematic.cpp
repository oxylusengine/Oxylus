#include <array>
#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>

#include "Cinematic/Cinematic.hpp"
#include "Cinematic/Easing.hpp"

using namespace ox;

static auto make_key(const f32 time, const glm::vec4& value, const Easing easing = Easing::Linear) -> CinematicKey {
  return CinematicKey{.time = time, .value = value, .easing = easing};
}

static auto make_waypoint(const f32 time, const glm::vec3& position) -> CameraWaypoint {
  return CameraWaypoint{.time = time, .position = position};
}

TEST(Easing, EndpointsAreExact) {
  for (auto raw = 0_u8; raw < std::to_underlying(Easing::Count); raw++) {
    const auto kind = static_cast<Easing>(raw);
    EXPECT_NEAR(ease(kind, 0.f), 0.f, 1e-5f) << easing_name(kind);
    EXPECT_NEAR(ease(kind, 1.f), 1.f, 1e-5f) << easing_name(kind);
  }
}

TEST(Easing, ClampsOutsideTheUnitRange) {
  EXPECT_FLOAT_EQ(ease(Easing::Linear, -1.f), 0.f);
  EXPECT_FLOAT_EQ(ease(Easing::Linear, 2.f), 1.f);
}

TEST(Easing, StepHoldsUntilTheEnd) {
  EXPECT_FLOAT_EQ(ease(Easing::Step, 0.99f), 0.f);
  EXPECT_FLOAT_EQ(ease(Easing::Step, 1.f), 1.f);
}

TEST(Easing, SmoothStepIsSymmetric) {
  EXPECT_NEAR(ease(Easing::SmoothStep, 0.5f), 0.5f, 1e-5f);
  EXPECT_NEAR(ease(Easing::SmoothStep, 0.25f) + ease(Easing::SmoothStep, 0.75f), 1.f, 1e-5f);
}

TEST(SampleProperty, EmptyTrackIsZero) {
  const auto value = cinematic::sample_property({}, CinematicValueKind::Float, 0.5f);
  EXPECT_FLOAT_EQ(value.x, 0.f);
}

TEST(SampleProperty, ClampsOutsideTheKeyedRange) {
  const auto keys = std::array{make_key(1.f, glm::vec4(10.f)), make_key(3.f, glm::vec4(30.f))};

  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Float, 0.f).x, 10.f);
  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Float, 99.f).x, 30.f);
}

TEST(SampleProperty, HitsKeysExactly) {
  const auto keys = std::array{
    make_key(0.f, glm::vec4(0.f)),
    make_key(1.f, glm::vec4(5.f)),
    make_key(2.f, glm::vec4(-3.f)),
  };

  for (const auto& key : keys) {
    EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Float, key.time).x, key.value.x);
  }
}

TEST(SampleProperty, LerpsTheMidpoint) {
  const auto keys = std::array{make_key(0.f, glm::vec4(0.f)), make_key(2.f, glm::vec4(0.f, 8.f, 0.f, 0.f))};

  const auto value = cinematic::sample_property(keys, CinematicValueKind::Float3, 1.f);
  EXPECT_NEAR(value.y, 4.f, 1e-5f);
}

TEST(SampleProperty, EasingIntoTheNextKeyIsWhatApplies) {
  const auto keys = std::array{
    make_key(0.f, glm::vec4(0.f), Easing::Step),
    make_key(2.f, glm::vec4(10.f), Easing::InQuad),
  };

  // ease(InQuad, 0.5) == 0.25
  EXPECT_NEAR(cinematic::sample_property(keys, CinematicValueKind::Float, 1.f).x, 2.5f, 1e-4f);
}

TEST(SampleProperty, StepEasingHoldsThePreviousValue) {
  const auto keys = std::array{
    make_key(0.f, glm::vec4(1.f)),
    make_key(2.f, glm::vec4(7.f), Easing::Step),
  };

  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Float, 1.9f).x, 1.f);
  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Float, 2.f).x, 7.f);
}

TEST(SampleProperty, DiscreteKindsNeverBlend) {
  const auto keys = std::array{make_key(0.f, glm::vec4(0.f)), make_key(2.f, glm::vec4(1.f))};

  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Bool, 1.f).x, 0.f);
  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Bool, 2.f).x, 1.f);
  EXPECT_FLOAT_EQ(cinematic::sample_property(keys, CinematicValueKind::Int, 1.99f).x, 0.f);
}

TEST(SampleProperty, QuatTakesTheShortestPath) {
  const auto from = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
  // 180 degrees around Y, written with a negated scalar so the naive lerp would take the long way
  const auto to = -glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f));

  const auto keys = std::array{
    make_key(0.f, glm::vec4(from.x, from.y, from.z, from.w)),
    make_key(1.f, glm::vec4(to.x, to.y, to.z, to.w)),
  };

  const auto sampled = cinematic::sample_property(keys, CinematicValueKind::Quat, 0.5f);
  const auto result = glm::quat::wxyz(sampled.w, sampled.x, sampled.y, sampled.z);
  const auto expected = glm::angleAxis(glm::quarter_pi<f32>(), glm::vec3(0.f, 1.f, 0.f));

  EXPECT_NEAR(glm::abs(glm::dot(result, expected)), 1.f, 1e-4f);
}

TEST(SampleCamera, PassesThroughEveryWaypoint) {
  auto track = CinematicCameraTrack{};
  track.waypoints = {
    make_waypoint(0.f, {0.f, 0.f, 0.f}),
    make_waypoint(1.f, {2.f, 1.f, 0.f}),
    make_waypoint(2.f, {5.f, -1.f, 3.f}),
    make_waypoint(3.f, {6.f, 0.f, 4.f}),
  };

  for (const auto& waypoint : track.waypoints) {
    const auto sampled = cinematic::sample_camera_raw(track, waypoint.time);
    EXPECT_NEAR(glm::distance(sampled.position, waypoint.position), 0.f, 1e-4f) << "at t=" << waypoint.time;
  }
}

TEST(SampleCamera, ClampsEndTangents) {
  auto track = CinematicCameraTrack{};
  track.waypoints = {
    make_waypoint(0.f, {0.f, 0.f, 0.f}),
    make_waypoint(1.f, {1.f, 0.f, 0.f}),
    make_waypoint(2.f, {2.f, 0.f, 0.f}),
  };

  // a straight, evenly spaced line stays straight even where the missing neighbour is substituted
  const auto sampled = cinematic::sample_camera_raw(track, 0.5f);
  EXPECT_NEAR(sampled.position.x, 0.5f, 1e-4f);
  EXPECT_NEAR(sampled.position.y, 0.f, 1e-4f);
}

TEST(SampleCamera, LerpsFov) {
  auto track = CinematicCameraTrack{};
  track.interp = CameraInterp::Linear;
  track.waypoints = {make_waypoint(0.f, {}), make_waypoint(2.f, {})};
  track.waypoints[0].fov = 30.f;
  track.waypoints[1].fov = 90.f;

  EXPECT_NEAR(cinematic::sample_camera_raw(track, 1.f).fov, 60.f, 1e-4f);
}

TEST(SampleCamera, SingleWaypointHoldsStill) {
  auto track = CinematicCameraTrack{};
  track.waypoints = {make_waypoint(1.f, {3.f, 4.f, 5.f})};

  const auto sampled = cinematic::sample_camera_raw(track, 99.f);
  EXPECT_NEAR(glm::distance(sampled.position, glm::vec3(3.f, 4.f, 5.f)), 0.f, 1e-5f);
}

TEST(ArcLengthLut, IsMonotonicAndNormalized) {
  auto track = CinematicCameraTrack{};
  track.interp = CameraInterp::Linear;
  track.constant_speed = true;
  // deliberately uneven: the second half covers ten times the distance in the same time
  track.waypoints = {
    make_waypoint(0.f, {0.f, 0.f, 0.f}),
    make_waypoint(1.f, {1.f, 0.f, 0.f}),
    make_waypoint(2.f, {11.f, 0.f, 0.f}),
  };

  auto lut = std::array<f32, Cinematic::ARC_LUT_SIZE>{};
  cinematic::build_arc_length_lut(track, lut);

  EXPECT_FLOAT_EQ(lut.front(), 0.f);
  EXPECT_NEAR(lut.back(), 1.f, 1e-5f);
  for (usize i = 1; i < lut.size(); i++) {
    EXPECT_GE(lut[i], lut[i - 1]);
  }
}

TEST(SampleCamera, ConstantSpeedEvensOutUnevenSpacing) {
  auto track = CinematicCameraTrack{};
  track.interp = CameraInterp::Linear;
  track.constant_speed = true;
  track.waypoints = {
    make_waypoint(0.f, {0.f, 0.f, 0.f}),
    make_waypoint(1.f, {1.f, 0.f, 0.f}),
    make_waypoint(2.f, {11.f, 0.f, 0.f}),
  };

  auto lut = std::array<f32, Cinematic::ARC_LUT_SIZE>{};
  cinematic::build_arc_length_lut(track, lut);

  // without the remap, half the time would only cover 1 of the 11 units
  const auto midpoint = cinematic::sample_camera(track, lut, 1.f);
  EXPECT_NEAR(midpoint.position.x, 5.5f, 0.1f);

  const auto quarter = cinematic::sample_camera(track, lut, 0.5f);
  EXPECT_NEAR(quarter.position.x, 2.75f, 0.1f);
}

TEST(Value, RoundTripsThroughTheVec4Representation) {
  auto storage = std::array<u8, 32>{};

  cinematic::write_value(storage.data(), CinematicValueKind::Float, glm::vec4(1.5f));
  EXPECT_FLOAT_EQ(cinematic::read_value(storage.data(), CinematicValueKind::Float).x, 1.5f);

  cinematic::write_value(storage.data(), CinematicValueKind::Float3, {1.f, 2.f, 3.f, 4.f});
  const auto vec3 = cinematic::read_value(storage.data(), CinematicValueKind::Float3);
  EXPECT_FLOAT_EQ(vec3.x, 1.f);
  EXPECT_FLOAT_EQ(vec3.y, 2.f);
  EXPECT_FLOAT_EQ(vec3.z, 3.f);
  EXPECT_FLOAT_EQ(vec3.w, 0.f);

  cinematic::write_value(storage.data(), CinematicValueKind::Bool, glm::vec4(1.f));
  auto boolean = false;
  std::memcpy(&boolean, storage.data(), sizeof(boolean));
  EXPECT_TRUE(boolean);

  cinematic::write_value(storage.data(), CinematicValueKind::Int, glm::vec4(6.7f));
  auto integer = 0_i32;
  std::memcpy(&integer, storage.data(), sizeof(integer));
  EXPECT_EQ(integer, 7);
}

TEST(CinematicIO, RoundTripsThroughDisk) {
  auto source = Cinematic::make_default();
  source.name = "Intro";
  source.duration = 12.5f;
  source.loop = true;

  auto camera_track = CinematicCameraTrack{};
  camera_track.name = "Dolly";
  camera_track.entity_path = "world::camera";
  camera_track.interp = CameraInterp::Linear;
  camera_track.constant_speed = true;
  camera_track.drive_fov = false;
  camera_track.waypoints = {make_waypoint(0.f, {1.f, 2.f, 3.f}), make_waypoint(4.f, {4.f, 5.f, 6.f})};
  camera_track.waypoints[1].easing = Easing::InOutCubic;
  camera_track.waypoints[1].fov = 35.f;
  camera_track.waypoints[1].rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f));
  source.camera_tracks.emplace_back(std::move(camera_track));

  auto property_track = CinematicPropertyTrack{};
  property_track.name = "Sun";
  property_track.entity_path = "world::sun";
  property_track.component_path = "Core.TransformComponent";
  property_track.member_path = "position.y";
  property_track.kind = CinematicValueKind::Float;
  property_track.enabled = false;
  property_track.keys = {make_key(0.f, glm::vec4(0.f)), make_key(2.f, glm::vec4(9.f), Easing::OutBack)};
  source.property_tracks.emplace_back(std::move(property_track));

  const auto path = std::filesystem::temp_directory_path() / "ox_test_cinematic.oxcine";
  ASSERT_TRUE(source.write(path));

  auto loaded = Cinematic::read(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(loaded.has_value());

  EXPECT_EQ(loaded->name, source.name);
  EXPECT_FLOAT_EQ(loaded->duration, source.duration);
  EXPECT_TRUE(loaded->loop);

  ASSERT_EQ(loaded->camera_tracks.size(), 1u);
  const auto& camera = loaded->camera_tracks.front();
  EXPECT_EQ(camera.name, "Dolly");
  EXPECT_EQ(camera.entity_path, "world::camera");
  EXPECT_EQ(camera.interp, CameraInterp::Linear);
  EXPECT_TRUE(camera.constant_speed);
  EXPECT_FALSE(camera.drive_fov);
  ASSERT_EQ(camera.waypoints.size(), 2u);
  EXPECT_EQ(camera.waypoints[1].easing, Easing::InOutCubic);
  EXPECT_FLOAT_EQ(camera.waypoints[1].fov, 35.f);
  EXPECT_NEAR(glm::distance(camera.waypoints[1].position, glm::vec3(4.f, 5.f, 6.f)), 0.f, 1e-5f);
  EXPECT_NEAR(
    glm::abs(glm::dot(camera.waypoints[1].rotation, glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0.f, 1.f, 0.f)))),
    1.f,
    1e-5f
  );

  ASSERT_EQ(loaded->property_tracks.size(), 1u);
  const auto& property = loaded->property_tracks.front();
  EXPECT_EQ(property.component_path, "Core.TransformComponent");
  EXPECT_EQ(property.member_path, "position.y");
  EXPECT_EQ(property.kind, CinematicValueKind::Float);
  EXPECT_FALSE(property.enabled);
  ASSERT_EQ(property.keys.size(), 2u);
  EXPECT_EQ(property.keys[1].easing, Easing::OutBack);
  EXPECT_FLOAT_EQ(property.keys[1].value.x, 9.f);
}

TEST(CinematicIO, KeyedExtentSpansEveryTrack) {
  auto cinematic = Cinematic::make_default();

  auto camera_track = CinematicCameraTrack{};
  camera_track.waypoints = {make_waypoint(0.f, {}), make_waypoint(3.f, {})};
  cinematic.camera_tracks.emplace_back(std::move(camera_track));

  auto property_track = CinematicPropertyTrack{};
  property_track.keys = {make_key(0.f, {}), make_key(7.25f, {})};
  cinematic.property_tracks.emplace_back(std::move(property_track));

  EXPECT_FLOAT_EQ(cinematic.keyed_extent(), 7.25f);
}
