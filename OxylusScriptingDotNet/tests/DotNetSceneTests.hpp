#pragma once

// the scene tests both hosts run, each test binary defines smoke_game() before including this

#include <chrono>
#include <cstdio>
#include <flecs.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "DotNetTestHelpers.hpp"
#include "Physics/Physics.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

// SmokeGame.Spin, as flecs lays out its members
struct Spin {
  f32 speed = 0.0f;
  f32 angle = 0.0f;
};

class DotNetSceneTest : public ::testing::Test {
protected:
  static constexpr auto FRAME = 1.0f / 60.0f;

  // no window and no renderer, a scene only needs physics and assets to run
  static void SetUpTestSuite() {
    static char arg0[] = "TestDotNetScene";
    static char* argv[] = {arg0, nullptr};
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;
    app = std::make_unique<ox::App>(1, argv);
    app->with<ox::AudioEngine>().with<ox::AssetManager>().with<ox::Physics>().with<ox::DotNetScripting>();
    app->init();
  }

  static void TearDownTestSuite() {
    app->stop();
    app.reset();
  }

  void SetUp() override {
    captured_logs.clear();
    const auto loaded = scripting().load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    scene = std::make_unique<ox::Scene>("DotNetSceneTest");
  }

  void TearDown() override {
    scene.reset();
    scripting().unload();
  }

  static auto scripting() -> ox::DotNetScripting& { return ox::App::mod<ox::DotNetScripting>(); }

  auto step(f32 seconds = FRAME) -> void { scene->runtime_update(ox::Timestep(static_cast<f64>(seconds) * 1000.0)); }

  auto id_of(const c8* path) -> flecs::entity_t { return ecs_lookup(scene->world.c_ptr(), path); }

  auto behaviour_entity(const std::string& name, std::string_view type_name) -> flecs::entity {
    return scene->create_entity(name).set(ox::ScriptComponent{.type_name = std::string(type_name)});
  }

  static inline std::unique_ptr<ox::App> app = nullptr;
  std::unique_ptr<ox::Scene> scene = nullptr;
};

TEST_F(DotNetSceneTest, AttachesTheSceneWorld) {
  const auto velocity = id_of("SmokeGame.Velocity");
  ASSERT_NE(velocity, 0);
  // defined through the scene's component db, so the scene serializes it and the inspector lists it
  EXPECT_TRUE(scene->component_db.is_component_known(flecs::id(scene->world, velocity)));
}

TEST_F(DotNetSceneTest, ListsScriptTypes) {
  const auto kind_of = [](std::string_view name) {
    const auto& types = scripting().script_types;
    const auto it = std::ranges::find(types, name, &ox::DotNetScriptType::name);
    return it == types.end() ? ox::OxScriptKind::None : it->kind;
  };

  EXPECT_EQ(kind_of("SmokeGame.LevelLogic"), ox::OxScriptKind::SceneScript);
  EXPECT_EQ(kind_of("SmokeGame.Spinner"), ox::OxScriptKind::Behaviour);
  EXPECT_EQ(kind_of("SmokeGame.Movement"), ox::OxScriptKind::None);
  EXPECT_TRUE(std::ranges::is_sorted(scripting().script_types, {}, &ox::DotNetScriptType::name));
}

TEST_F(DotNetSceneTest, RunsASceneScriptsHooks) {
  const auto added = scene->add_script_type("SmokeGame.LevelLogic");
  ASSERT_TRUE(added.has_value()) << added.error();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnAdd"), 1);
  EXPECT_EQ(scene->get_scripts().size(), 1);

  scene->runtime_start();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnStart"), 1);

  step();
  step();
  scene->runtime_stop();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnStop updates=2"), 1);

  scene->remove_script(ox::ScriptKey{.type_name = "SmokeGame.LevelLogic"});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnRemove"), 1);
  EXPECT_TRUE(scene->get_scripts().empty());
}

TEST_F(DotNetSceneTest, RejectsWhatIsNotASceneScript) {
  const auto missing = scene->add_script_type("SmokeGame.Nope");
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("SmokeGame.Nope"), std::string::npos);

  const auto behaviour = scene->add_script_type("SmokeGame.Spinner");
  ASSERT_FALSE(behaviour.has_value());
  EXPECT_NE(behaviour.error().find("behaviour"), std::string::npos);
  EXPECT_TRUE(scene->get_scripts().empty());
}

TEST_F(DotNetSceneTest, RegistersASceneScriptsSystemsWithIt) {
  const auto spin = id_of("SmokeGame.Spin");
  ASSERT_NE(spin, 0);
  auto spinning = scene->create_entity("Spinning");
  const auto value = Spin{.speed = 2.0f};
  ecs_set_id(scene->world.c_ptr(), spinning, spin, sizeof(Spin), &value);
  const auto angle = [&] {
    return static_cast<const Spin*>(ecs_get_id(scene->world.c_ptr(), spinning, spin))->angle;
  };

  step(0.5f);
  EXPECT_FLOAT_EQ(angle(), 0.0f);

  ASSERT_TRUE(scene->add_script_type("SmokeGame.LevelLogic").has_value());
  step(0.5f);
  EXPECT_FLOAT_EQ(angle(), 1.0f);

  scene->remove_script(ox::ScriptKey{.type_name = "SmokeGame.LevelLogic"});
  step(0.5f);
  EXPECT_FLOAT_EQ(angle(), 1.0f);
}

TEST_F(DotNetSceneTest, RunsFixedUpdateOncePerPhysicsStep) {
  ASSERT_TRUE(scene->add_script_type("SmokeGame.LevelLogic").has_value());
  scene->runtime_start();
  for (auto i = 0; i < 5; i++) {
    step();
  }

  // a frame three ticks long owes three physics steps, and each gets its own fixed update with the fixed length
  captured_logs.clear();
  step(3.0f * FRAME);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnFixedUpdate 0.0167"), 3);
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Info, "LevelLogic.OnFixedUpdate"), 3);
}

TEST_F(DotNetSceneTest, KeepsBehavioursWhileTheSceneRuns) {
  auto spun = behaviour_entity("Spun", "SmokeGame.Spinner");
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Spun"), 0);

  scene->runtime_start();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Spun"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnStart Spun"), 1);

  step();
  step();
  step();
  EXPECT_FLOAT_EQ(spun.get<ox::TransformComponent>().position.x, 3.0f);

  scene->runtime_stop();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnDestroy Spun"), 1);

  step();
  EXPECT_FLOAT_EQ(spun.get<ox::TransformComponent>().position.x, 3.0f);
}

TEST_F(DotNetSceneTest, FollowsTheScriptComponent) {
  scene->runtime_start();

  auto entity = scene->create_entity("Follower");
  entity.set(ox::ScriptComponent{.type_name = "SmokeGame.Spinner"});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Follower"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnStart Follower"), 0);

  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnStart Follower"), 1);
  EXPECT_FLOAT_EQ(entity.get<ox::TransformComponent>().position.x, 1.0f);

  // setting the same type again leaves the instance alone
  entity.set(ox::ScriptComponent{.type_name = "SmokeGame.Spinner"});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Follower"), 1);

  entity.remove<ox::ScriptComponent>();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnDestroy Follower"), 1);
  step();
  EXPECT_FLOAT_EQ(entity.get<ox::TransformComponent>().position.x, 1.0f);

  entity.set(ox::ScriptComponent{.type_name = "SmokeGame.Spinner"});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Follower"), 2);
  entity.destruct();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnDestroy Follower"), 2);

  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetSceneTest, DisablesABehaviourThatThrows) {
  behaviour_entity("Broken", "SmokeGame.Faulty");
  behaviour_entity("Fine", "SmokeGame.Spinner");
  behaviour_entity("Unknown", "SmokeGame.Nope");
  scene->runtime_start();
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Error, "There is no C# behaviour SmokeGame.Nope"), 1);

  step();
  step();
  step();
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Error, "SmokeGame.Faulty.OnUpdate"), 1);
  EXPECT_FLOAT_EQ(scene->world.lookup("Fine").get<ox::TransformComponent>().position.x, 3.0f);
}

TEST_F(DotNetSceneTest, AddsABehaviourFromCSharp) {
  auto late = scene->create_entity("Late");
  behaviour_entity("Spawner", "SmokeGame.Spawner");
  scene->runtime_start();

  // added mid dispatch, so it waits for the next frame to start and update
  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Late"), 1);
  EXPECT_TRUE(late.has<ox::ScriptComponent>());
  EXPECT_FLOAT_EQ(late.get<ox::TransformComponent>().position.x, 0.0f);

  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnStart Late"), 1);
  EXPECT_FLOAT_EQ(late.get<ox::TransformComponent>().position.x, 1.0f);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetSceneTest, HandsContactsOverOnTheMainThread) {
  ASSERT_TRUE(scene->add_script_type("SmokeGame.LevelLogic").has_value());
  scene->create_entity("Floor")
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {5.0f, 0.5f, 5.0f}});
  behaviour_entity("Ball", "SmokeGame.Bumper")
    .set(ox::TransformComponent{.position = {0.0f, 2.0f, 0.0f}})
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Dynamic})
    .set(ox::SphereColliderComponent{.radius = 0.5f});
  scene->runtime_start();

  for (auto i = 0; i < 120 && count_logs_containing(ox::OxLogLevel::Info, "Bumper.OnContact") == 0; i++) {
    step();
  }

  // seen from the ball, the normal points down into the floor
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Bumper.OnContact Ball Floor -1.0 main=True"), 1);
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Info, "LevelLogic.OnContact"), 1);
}

TEST_F(DotNetSceneTest, KeepsTypeEntriesInSceneJson) {
  auto source = std::make_shared<ox::Scene>("Source");
  ASSERT_TRUE(source->add_script_type("SmokeGame.LevelLogic").has_value());
  source->create_entity("Spun").set(ox::ScriptComponent{.type_name = "SmokeGame.Spinner"});

  const auto json = source->to_json().stream.str();
  EXPECT_NE(json.find("\"type\""), std::string::npos);
  EXPECT_NE(json.find("SmokeGame.LevelLogic"), std::string::npos);

  const auto copy = ox::Scene::copy(source);
  EXPECT_TRUE(copy->get_scripts().contains(ox::ScriptKey{.type_name = "SmokeGame.LevelLogic"}));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnAdd"), 2);

  const auto spun = copy->world.lookup("Spun");
  ASSERT_TRUE(spun.is_valid());
  ASSERT_TRUE(spun.has<ox::ScriptComponent>());
  EXPECT_EQ(spun.get<ox::ScriptComponent>().type_name, "SmokeGame.Spinner");
}

TEST_F(DotNetSceneTest, RecreatesScriptsAndBehavioursOnReload) {
  ASSERT_TRUE(scene->add_script_type("SmokeGame.LevelLogic").has_value());
  auto spun = behaviour_entity("Spun", "SmokeGame.Spinner");
  scene->runtime_start();
  step();

  const auto reloaded = scripting().load(smoke_game(), capturing_native_api());
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error();
  // under hostfxr nothing may keep the unloaded assembly alive, no script, behaviour or handle to one
  EXPECT_EQ(count_logs(ox::OxLogLevel::Warn), 0);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnRemove"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnDestroy Spun"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnAdd"), 2);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "LevelLogic.OnStart"), 2);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Spinner.OnCreate Spun"), 2);

  step();
  EXPECT_FLOAT_EQ(spun.get<ox::TransformComponent>().position.x, 2.0f);
}

TEST_F(DotNetSceneTest, UpdatesManyBehavioursWithoutAllocating) {
  constexpr auto BEHAVIOUR_COUNT = 10'000;
  constexpr auto FRAMES = 100;

  for (auto i = 0; i < BEHAVIOUR_COUNT; i++) {
    scene->world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.Ticker"});
  }

  scene->runtime_start();
  auto* world = scene->world.c_ptr();
  const auto& managed_api = scripting().managed_api;
  for (auto i = 0; i < 30; i++) {
    managed_api.world_update(world, FRAME);
  }

  const auto allocated_before = managed_api.allocated_bytes();
  const auto start = std::chrono::steady_clock::now();
  for (auto i = 0; i < FRAMES; i++) {
    managed_api.world_update(world, FRAME);
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(managed_api.allocated_bytes(), allocated_before);

  const auto frame_us = std::chrono::duration<f64, std::micro>(elapsed).count() / FRAMES;
  std::printf(
    "[ perf     ] %d behaviours: %.1f us/frame, %.1f ns each\n",
    BEHAVIOUR_COUNT,
    frame_us,
    frame_us * 1000.0 / BEHAVIOUR_COUNT
  );
}

TEST_F(DotNetSceneTest, DrivesABodyFromCSharp) {
  scene->create_entity("Floor")
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {50.0f, 0.5f, 50.0f}});
  auto ball = behaviour_entity("Ball", "SmokeGame.Pusher")
                .set(ox::TransformComponent{.position = {0.0f, 1.0f, 0.0f}})
                .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Dynamic, .friction = 0.0f})
                .set(ox::SphereColliderComponent{.radius = 0.5f});
  scene->runtime_start();

  for (auto i = 0; i < 30; i++) {
    step();
  }

  EXPECT_GT(ball.get<ox::RigidBodyComponent>().translation.x, 1.0f);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Pusher saw the floor refuse a force"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetSceneTest, RaycastsAgainstShapes) {
  ASSERT_TRUE(scene->add_script_type("SmokeGame.RaycastProbe").has_value());
  scene->create_entity("Floor")
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {5.0f, 0.5f, 5.0f}});
  scene->runtime_start();

  // the box's top face is at 0.5, so a ray down from 5 hits it 4.5 along
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "raycast hit Floor at 0.50 normal 1.00 distance 4.50"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "raycast misses: ignored True short True filtered True"), 1);
}

TEST_F(DotNetSceneTest, WalksACharacterFromCSharp) {
  scene->create_entity("Floor")
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {50.0f, 0.5f, 50.0f}});
  auto hero = behaviour_entity("Hero", "SmokeGame.Walker")
                .set(ox::TransformComponent{.position = {0.0f, 2.0f, 0.0f}})
                .set(ox::CharacterControllerComponent{});
  scene->runtime_start();

  for (auto i = 0; i < 120; i++) {
    step();
  }

  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "Walker grounded, normal 1.0"), 1) << logs_text();
  EXPECT_GT(hero.get<ox::CharacterControllerComponent>().translation.x, 1.0f);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}
