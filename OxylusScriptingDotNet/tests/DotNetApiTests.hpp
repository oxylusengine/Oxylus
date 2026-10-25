#pragma once

// Log, Time and the transform helpers from C#, in a bare world. Each test binary defines smoke_game() first

#include <flecs.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <gtest/gtest.h>

#include "DotNetTestHelpers.hpp"
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
