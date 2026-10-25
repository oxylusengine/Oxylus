#pragma once

// Log, Time and the transform helpers from C#, in a bare world. Each test binary defines smoke_game() first

#include <flecs.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <gtest/gtest.h>

#include "DotNetTestHelpers.hpp"
#include "Render/BoundingVolume.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Timestep.hpp"

// SmokeGame.ApiProbe and SmokeGame.PlaceProbe, as flecs lays out their members
struct ApiProbe {
  f32 delta_time = 0.0f;
  f64 elapsed = 0.0;
  u64 frame = 0;
  glm::vec3 world_position = {};
  glm::vec3 forward = {};
  i32 runs = 0;
};

struct PlaceProbe {
  glm::vec3 world_position = {};
  glm::vec3 look_at = {};
  i32 apply = 0;
};

// SmokeGame.MathProbe, the glm columns of its matrix in the four vec4
struct MathProbe {
  i32 run = 0;
  glm::vec3 box_min = {};
  glm::vec3 box_max = {};
  glm::vec3 other_min = {};
  glm::vec3 other_max = {};
  glm::vec3 point = {};
  glm::vec3 scale = {};
  glm::vec3 direction = {};
  glm::vec3 up = {};
  glm::vec4 column0 = {};
  glm::vec4 column1 = {};
  glm::vec4 column2 = {};
  glm::vec4 column3 = {};

  glm::vec3 transformed_min = {};
  glm::vec3 transformed_max = {};
  glm::vec3 translated_min = {};
  glm::vec3 translated_max = {};
  glm::vec3 scaled_min = {};
  glm::vec3 scaled_max = {};
  glm::vec3 merged_min = {};
  glm::vec3 merged_max = {};
  glm::vec3 center = {};
  glm::vec3 extents = {};
  glm::vec3 size = {};
  glm::vec3 floored = {};
  glm::vec3 ceiled = {};
  glm::vec3 rounded = {};
  i32 intersects = 0;
  i32 classification = 0;
  i32 contains = 0;
  glm::quat look = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
};

static_assert(sizeof(MathProbe) == 360);

class DotNetApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    captured_logs.clear();
    world.import<ox::CoreComponentsModule>();

    const auto loaded = scripting.load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const auto attached = scripting.attach_world(world.c_ptr());
    ASSERT_TRUE(attached.has_value()) << attached.error();

    api_probe = ecs_lookup(world.c_ptr(), "SmokeGame.ApiProbe");
    place_probe = ecs_lookup(world.c_ptr(), "SmokeGame.PlaceProbe");
    ASSERT_NE(api_probe, 0);
    ASSERT_NE(place_probe, 0);
  }

  void TearDown() override {
    scripting.detach_world(world.c_ptr());
    scripting.unload();
  }

  template <typename T>
  auto get(flecs::entity entity, flecs::entity_t component) -> const T& {
    return *static_cast<const T*>(ecs_get_id(world.c_ptr(), entity, component));
  }

  template <typename T>
  auto set(flecs::entity entity, flecs::entity_t component, const T& value) -> void {
    ecs_set_id(world.c_ptr(), entity, component, sizeof(T), &value);
  }

  // rotated a quarter turn about y and scaled, so a child's world transform differs in every way from its local one
  auto parent_at(glm::vec3 position) -> flecs::entity {
    return world.entity("Parent").set(
      ox::TransformComponent{
        .position = position,
        .rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
        .scale = {2.0f, 2.0f, 2.0f},
      }
    );
  }

  flecs::world world;
  ox::DotNetScripting scripting = {};
  flecs::entity_t api_probe = 0;
  flecs::entity_t place_probe = 0;
};

static auto expect_near(glm::vec3 actual, glm::vec3 expected) -> void {
  EXPECT_NEAR(actual.x, expected.x, 1e-4f);
  EXPECT_NEAR(actual.y, expected.y, 1e-4f);
  EXPECT_NEAR(actual.z, expected.z, 1e-4f);
}

TEST_F(DotNetApiTest, StoresQuaternionsAsSystemNumericsReadsThem) {
  // C# reads TransformComponent::rotation as a System.Numerics.Quaternion, x y z w in that order
  const auto rotation = glm::quat::wxyz(4.0f, 1.0f, 2.0f, 3.0f);
  const auto* raw = reinterpret_cast<const f32*>(&rotation);
  EXPECT_EQ(raw[0], 1.0f);
  EXPECT_EQ(raw[3], 4.0f);
}

TEST_F(DotNetApiTest, ReadsTheFrameClock) {
  auto probe = world.entity();
  set(probe, api_probe, ApiProbe{});

  scripting.update(ox::Timestep(16.0));
  scripting.update(ox::Timestep(20.0));
  world.progress(1.0f / 60.0f);

  const auto* time = ox::make_native_api().time;
  const auto& seen = get<ApiProbe>(probe, api_probe);
  EXPECT_FLOAT_EQ(seen.delta_time, 0.02f);
  EXPECT_EQ(seen.frame, time->frame);
  EXPECT_DOUBLE_EQ(seen.elapsed, time->elapsed);
}

TEST_F(DotNetApiTest, LogsInterpolatedStringsWithoutAllocating) {
  auto probe = world.entity();
  set(probe, api_probe, ApiProbe{});
  scripting.update(ox::Timestep(16.0));

  // warm up, the JIT tiers the loop before anything is measured
  for (auto i = 0; i < 30; i++) {
    world.progress(1.0f / 60.0f);
  }

  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "api probe 1 saw 0.016"), 1) << logs_text();

  const auto allocated_before = scripting.managed_api.allocated_bytes();
  for (auto i = 0; i < 100; i++) {
    world.progress(1.0f / 60.0f);
  }

  EXPECT_EQ(scripting.managed_api.allocated_bytes() - allocated_before, 0);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "api probe 130 saw 0.016"), 1);
}

TEST_F(DotNetApiTest, ComputesWorldTransformsAsTheEngineDoes) {
  auto parent = parent_at({1.0f, 2.0f, 3.0f});
  auto child = world.entity("Child").child_of(parent).set(
    ox::TransformComponent{
      .position = {1.0f, 0.0f, 0.0f},
      .rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
    }
  );
  set(child, api_probe, ApiProbe{});

  world.progress(1.0f / 60.0f);

  const auto& seen = get<ApiProbe>(child, api_probe);
  expect_near(seen.world_position, ox::Scene::get_world_position(child));
  expect_near(seen.world_position, glm::vec3(ox::Scene::get_world_transform(child)[3]));
  expect_near(seen.forward, child.get<ox::TransformComponent>().rotation * glm::vec3(0.0f, 0.0f, -1.0f));
}

TEST_F(DotNetApiTest, PlacesAnEntityInWorldSpace) {
  auto parent = parent_at({1.0f, 2.0f, 3.0f});
  auto child = world.entity("Child").child_of(parent).set(ox::TransformComponent{});
  const auto target = glm::vec3(5.0f, 1.0f, -2.0f);
  set(
    child,
    place_probe,
    PlaceProbe{.world_position = target, .look_at = target + glm::vec3(3.0f, 0.0f, 0.0f), .apply = 1}
  );

  world.progress(1.0f / 60.0f);

  expect_near(glm::vec3(ox::Scene::get_world_transform(child)[3]), target);

  // looking along +x from wherever it stands, whatever its parent's rotation
  auto scale = glm::vec3();
  auto rotation = glm::quat();
  auto translation = glm::vec3();
  auto skew = glm::vec3();
  auto perspective = glm::vec4();
  glm::decompose(ox::Scene::get_world_transform(child), scale, rotation, translation, skew, perspective);
  expect_near(rotation * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST_F(DotNetApiTest, ComputesBoxesAndLookRotationsAsGlmDoes) {
  const auto math_probe = ecs_lookup(world.c_ptr(), "SmokeGame.MathProbe");
  ASSERT_NE(math_probe, 0);
  auto probe = world.entity();

  const auto box = ox::AABB({-1.0f, -2.0f, -3.0f}, {2.0f, 1.0f, 4.0f});
  // a turn, a mirrored axis and a translation, so every part of the matrix reaches the new bounds
  const auto matrix = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, -2.0f, 3.0f)) *
                      glm::rotate(glm::mat4(1.0f), glm::radians(40.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.5f, -1.5f));

  struct Case {
    ox::AABB other;
    glm::vec3 point;
    glm::vec3 direction;
  };

  const auto cases = std::array{
    // overlaps, a point in the box and a look straight down -Z
    Case{ox::AABB({1.0f, 0.0f, 0.0f}, {5.0f, 3.0f, 2.0f}), {0.5f, -0.5f, 3.0f}, {0.0f, 0.0f, -1.0f}},
    // inside, a point on a face
    Case{
      ox::AABB({0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 1.0f}),
      {2.0f, 0.0f, 0.0f},
      glm::normalize(glm::vec3(1.0f, 2.0f, -3.0f))
    },
    // apart, a point out of the box
    Case{
      ox::AABB({3.0f, 0.0f, 0.0f}, {5.0f, 3.0f, 2.0f}),
      {-1.5f, 2.5f, 0.49f},
      glm::normalize(glm::vec3(0.3f, -0.8f, 0.5f))
    },
    // touching on a face, looking along +X
    Case{ox::AABB({2.0f, -2.0f, -3.0f}, {4.0f, 1.0f, 4.0f}), {-0.5f, -2.5f, 7.5f}, {1.0f, 0.0f, 0.0f}},
  };

  for (const auto& test_case : cases) {
    const auto scale = glm::vec3(2.0f, -1.0f, 0.5f);
    set(
      probe,
      math_probe,
      MathProbe{
        .run = 1,
        .box_min = box.min,
        .box_max = box.max,
        .other_min = test_case.other.min,
        .other_max = test_case.other.max,
        .point = test_case.point,
        .scale = scale,
        .direction = test_case.direction,
        .up = {0.0f, 1.0f, 0.0f},
        .column0 = matrix[0],
        .column1 = matrix[1],
        .column2 = matrix[2],
        .column3 = matrix[3],
      }
    );
    world.progress(1.0f / 60.0f);

    const auto& seen = get<MathProbe>(probe, math_probe);
    EXPECT_EQ(seen.run, 0);

    const auto transformed = box.get_transformed(matrix);
    expect_near(seen.transformed_min, transformed.min);
    expect_near(seen.transformed_max, transformed.max);

    auto translated = box;
    translated.translate(test_case.point);
    expect_near(seen.translated_min, translated.min);
    expect_near(seen.translated_max, translated.max);

    auto scaled = box;
    scaled.scale(scale);
    expect_near(seen.scaled_min, scaled.min);
    expect_near(seen.scaled_max, scaled.max);

    auto merged = box;
    merged.merge(test_case.other);
    expect_near(seen.merged_min, merged.min);
    expect_near(seen.merged_max, merged.max);

    expect_near(seen.center, box.get_center());
    expect_near(seen.extents, box.get_extents());
    expect_near(seen.size, box.get_size());

    EXPECT_EQ(seen.intersects, box.intersects_fast(test_case.other) ? 1 : 0);
    EXPECT_EQ(seen.classification, static_cast<i32>(box.intersects(test_case.other)));
    EXPECT_EQ(seen.contains, box.intersects(test_case.point) ? 1 : 0);

    expect_near(seen.floored, glm::floor(test_case.point));
    expect_near(seen.ceiled, glm::ceil(test_case.point));
    expect_near(seen.rounded, glm::round(test_case.point));

    // the same rotation, which a quaternion and its negation both are
    const auto expected_look = glm::quatLookAt(test_case.direction, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(std::abs(glm::dot(seen.look, expected_look)), 1.0f, 1e-5f);
    expect_near(seen.look * glm::vec3(0.0f, 0.0f, -1.0f), test_case.direction);
  }
}

TEST_F(DotNetApiTest, ReadsNoWindowAndNoFrameLimitWithoutAnApp) {
  world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.AppProbe"});
  scripting.start_world(world.c_ptr());

  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "window size 0 0 logical 0 0"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "time default -1"), 1) << logs_text();
  // setting it has nowhere to land
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "time limited -1"), 1) << logs_text();
}

TEST_F(DotNetApiTest, QuitsNothingWithoutAnApp) {
  world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.QuitProbe"});
  scripting.start_world(world.c_ptr());

  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "app quit called"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}
