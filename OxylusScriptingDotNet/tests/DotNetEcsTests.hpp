#pragma once

// the ECS tests both hosts run, each test binary defines smoke_game() before including this

#include <chrono>
#include <cstdio>
#include <flecs.h>
#include <glm/vec3.hpp>
#include <gtest/gtest.h>
#include <string_view>

#include "DotNetTestHelpers.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"

// SmokeGame's C# components, as flecs lays out their members
struct Velocity {
  glm::vec3 value = {};
};

struct Countdown {
  f32 seconds = 0.0f;
  i32 ticks = 0;
};

struct Census {
  i32 movers = 0;
};

class DotNetEcsTest : public ::testing::Test {
protected:
  void SetUp() override {
    captured_logs.clear();
    world.import<ox::CoreComponentsModule>();

    const auto loaded = scripting.load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const auto attached = scripting.attach_world(world.c_ptr(), &component_db);
    ASSERT_TRUE(attached.has_value()) << attached.error();

    velocity = id_of("SmokeGame.Velocity");
    countdown = id_of("SmokeGame.Countdown");
    expired = id_of("SmokeGame.Expired");
    census = id_of("SmokeGame.Census");
  }

  void TearDown() override {
    scripting.detach_world(world.c_ptr());
    scripting.unload();
  }

  auto id_of(const c8* path) -> flecs::entity_t { return ecs_lookup(world.c_ptr(), path); }

  template <typename T>
  auto get(flecs::entity entity, flecs::entity_t component) -> T* {
    return static_cast<T*>(ecs_get_mut_id(world.c_ptr(), entity, component));
  }

  template <typename T>
  auto set(flecs::entity entity, flecs::entity_t component, const T& value) -> void {
    ecs_set_id(world.c_ptr(), entity, component, sizeof(T), &value);
  }

  auto mover(glm::vec3 position, glm::vec3 speed) -> flecs::entity {
    auto entity = world.entity().set(ox::TransformComponent{.position = position});
    set(entity, velocity, Velocity{.value = speed});
    return entity;
  }

  // destroyed last, the scripts detach from it first
  flecs::world world;
  ox::ComponentDB component_db = {};
  ox::DotNetScripting scripting = {};

  flecs::entity_t velocity = 0;
  flecs::entity_t countdown = 0;
  flecs::entity_t expired = 0;
  flecs::entity_t census = 0;
};

TEST_F(DotNetEcsTest, DefinesCSharpComponentsWithReflection) {
  ASSERT_NE(velocity, 0);
  ASSERT_NE(countdown, 0);
  ASSERT_NE(expired, 0);

  const auto* info = static_cast<const EcsComponent*>(ecs_get_id(world.c_ptr(), velocity, ecs_id(EcsComponent)));
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->size, sizeof(Velocity));
  const auto* members = static_cast<const EcsStruct*>(ecs_get_id(world.c_ptr(), velocity, ecs_id(EcsStruct)));
  ASSERT_NE(members, nullptr);
  ASSERT_EQ(members->members.count, 1);
  const auto& member = *ecs_vec_first_t(&members->members, ecs_member_t);
  EXPECT_STREQ(member.name, "Value");
  EXPECT_EQ(member.type, world.component<glm::vec3>().id());

  // a fieldless C# struct is a tag, flecs gives it no storage
  const auto* tag_info = static_cast<const EcsComponent*>(ecs_get_id(world.c_ptr(), expired, ecs_id(EcsComponent)));
  EXPECT_TRUE(!tag_info || tag_info->size == 0);

  // what lets a scene serialize them and the inspector list them
  EXPECT_TRUE(component_db.is_component_known(flecs::id(world, velocity)));
  EXPECT_TRUE(component_db.is_component_known(flecs::id(world, countdown)));
}

TEST_F(DotNetEcsTest, MovesEntitiesThroughACSharpSystem) {
  const auto a = mover({0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f});
  const auto b = mover({10.0f, 0.0f, 0.0f}, {-2.0f, 0.0f, 0.0f});
  const auto still = world.entity().set(ox::TransformComponent{.position = {5.0f, 5.0f, 5.0f}});

  world.progress(0.5f);

  EXPECT_EQ(a.get<ox::TransformComponent>().position, glm::vec3(0.5f, 1.0f, 1.5f));
  EXPECT_EQ(b.get<ox::TransformComponent>().position, glm::vec3(9.0f, 0.0f, 0.0f));
  EXPECT_EQ(still.get<ox::TransformComponent>().position, glm::vec3(5.0f, 5.0f, 5.0f));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetEcsTest, DrivesEntitiesThroughTheEntityApi) {
  const auto entity = world.entity().set(ox::TransformComponent{.scale = {2.0f, 2.0f, 2.0f}});
  set(entity, countdown, Countdown{.seconds = 0.25f});

  world.progress(0.5f);

  ASSERT_NE(get<Countdown>(entity, countdown), nullptr);
  EXPECT_EQ(get<Countdown>(entity, countdown)->ticks, 1);
  EXPECT_TRUE(ecs_has_id(world.c_ptr(), entity, expired));
  EXPECT_EQ(entity.get<ox::TransformComponent>().scale, glm::vec3(0.0f));
  ASSERT_NE(get<Velocity>(entity, velocity), nullptr);
  EXPECT_EQ(get<Velocity>(entity, velocity)->value, glm::vec3(0.0f));

  // already expired, so the second frame only ticks
  entity.get_mut<ox::TransformComponent>().scale = glm::vec3(1.0f);
  world.progress(0.5f);
  EXPECT_EQ(get<Countdown>(entity, countdown)->ticks, 2);
  EXPECT_EQ(entity.get<ox::TransformComponent>().scale, glm::vec3(1.0f));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetEcsTest, IteratesAQueryFromCSharp) {
  for (auto i = 0; i < 5; i++) {
    mover({}, {1.0f, 0.0f, 0.0f});
  }

  const auto taker = world.entity();
  set(taker, census, Census{});

  world.progress(0.1f);

  EXPECT_EQ(get<Census>(taker, census)->movers, 5);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetEcsTest, LayoutCheckRejectsAMismatchedStruct) {
  const auto api = ox::make_native_api();
  constexpr auto path = std::string_view("ox.Core.TransformComponent");
  constexpr auto position = std::string_view("position");

  auto member = ox::OxMemberLayout{.name = {position.data(), position.size()}, .offset = 4, .size = 12};
  auto layout = ox::OxComponentLayout{
    .path = {path.data(), path.size()},
    .members = &member,
    .member_count = 1,
    .size = sizeof(ox::TransformComponent),
    .alignment = alignof(ox::TransformComponent),
  };
  auto id = u64{0};

  EXPECT_EQ(api.bind_components(world.c_ptr(), &layout, 1, &id), ox::OxStatus::LayoutMismatch);
  EXPECT_EQ(id, 0);

  member.offset = 0;
  EXPECT_EQ(api.bind_components(world.c_ptr(), &layout, 1, &id), ox::OxStatus::Ok);
  EXPECT_EQ(id, world.component<ox::TransformComponent>().id());

  layout.size += 4;
  EXPECT_EQ(api.bind_components(world.c_ptr(), &layout, 1, &id), ox::OxStatus::LayoutMismatch);
}

TEST_F(DotNetEcsTest, KeepsComponentDataAcrossAReload) {
  const auto entity = mover({}, {1.0f, 0.0f, 0.0f});

  scripting.unload();
  ASSERT_NE(get<Velocity>(entity, velocity), nullptr);
  EXPECT_EQ(get<Velocity>(entity, velocity)->value.x, 1.0f);

  // the world stays attached across the unload, loading again re-registers the systems in it
  const auto loaded = scripting.load(smoke_game(), capturing_native_api());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(id_of("SmokeGame.Velocity"), velocity);

  world.progress(1.0f);
  EXPECT_EQ(entity.get<ox::TransformComponent>().position.x, 1.0f);
}

TEST_F(DotNetEcsTest, MovesManyEntitiesWithoutAllocating) {
  constexpr auto ENTITY_COUNT = 100'000;
  constexpr auto FRAMES = 100;
  constexpr auto DELTA = 1.0f / 60.0f;

  for (auto i = 0; i < ENTITY_COUNT; i++) {
    mover({}, {1.0f, 0.0f, 0.0f});
  }

  // warm up, the JIT tiers the loop up before anything is measured
  for (auto i = 0; i < 30; i++) {
    world.progress(DELTA);
  }

  const auto allocated_before = scripting.managed_api.allocated_bytes();
  const auto csharp_start = std::chrono::steady_clock::now();
  for (auto i = 0; i < FRAMES; i++) {
    world.progress(DELTA);
  }

  const auto csharp_time = std::chrono::steady_clock::now() - csharp_start;
  EXPECT_EQ(scripting.managed_api.allocated_bytes(), allocated_before);

  ecs_enable(world.c_ptr(), id_of("SmokeGame.Movement.Move"), false);
  const auto velocity_id = velocity;
  world.system("CppMove")
    .with<ox::TransformComponent>()
    .with(velocity_id)
    .kind(flecs::OnUpdate)
    .run([](flecs::iter& it) {
      while (it.next()) {
        auto* transforms = static_cast<ox::TransformComponent*>(
          ecs_field_w_size(it.c_ptr(), sizeof(ox::TransformComponent), 0)
        );
        const auto* velocities = static_cast<const Velocity*>(ecs_field_w_size(it.c_ptr(), sizeof(Velocity), 1));
        for (const auto i : it) {
          transforms[i].position += velocities[i].value * it.delta_time();
        }
      }
    });

  for (auto i = 0; i < 30; i++) {
    world.progress(DELTA);
  }

  const auto cpp_start = std::chrono::steady_clock::now();
  for (auto i = 0; i < FRAMES; i++) {
    world.progress(DELTA);
  }

  const auto cpp_time = std::chrono::steady_clock::now() - cpp_start;
  const auto csharp_ms = std::chrono::duration<f64, std::milli>(csharp_time).count() / FRAMES;
  const auto cpp_ms = std::chrono::duration<f64, std::milli>(cpp_time).count() / FRAMES;
  std::printf(
    "[ perf     ] %d entities: C# %.3f ms/frame, C++ %.3f ms/frame, %.2fx\n",
    ENTITY_COUNT,
    csharp_ms,
    cpp_ms,
    csharp_ms / cpp_ms
  );
}
