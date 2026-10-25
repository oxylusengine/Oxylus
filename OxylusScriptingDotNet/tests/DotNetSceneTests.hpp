#pragma once

// the scene tests both hosts run, each test binary defines smoke_game() before including this

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <flecs.h>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "DotNetTestHelpers.hpp"
#include "Physics/Physics.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Render/Camera.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

// what ContentProbe and LeakyProbe find in the assets: a fifth of a second of tone, a text file and a scene to ask for
struct ContentAssets {
  ox::UUID beep = {};
  ox::UUID next_scene = {};
};

static auto write_file(const std::filesystem::path& path, std::string_view bytes) -> void {
  std::filesystem::create_directories(path.parent_path());
  auto file = std::ofstream(path, std::ios::binary);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static auto tone_wav(f32 seconds) -> std::string {
  constexpr auto SAMPLE_RATE = 22050_u32;
  const auto sample_count = static_cast<u32>(seconds * SAMPLE_RATE);
  auto wav = std::string();
  const auto put = [&wav](auto value) {
    wav.append(reinterpret_cast<const c8*>(&value), sizeof(value));
  };
  wav += "RIFF";
  put(36_u32 + sample_count * 2);
  wav += "WAVEfmt ";
  put(16_u32);
  put(static_cast<u16>(1));
  put(static_cast<u16>(1));
  put(SAMPLE_RATE);
  put(SAMPLE_RATE * 2);
  put(static_cast<u16>(2));
  put(static_cast<u16>(16));
  wav += "data";
  put(sample_count * 2);
  for (u32 i = 0; i < sample_count; i++) {
    put(static_cast<i16>(std::sin(static_cast<f32>(i) * 0.12f) * 8000.0f));
  }

  return wav;
}

// mounted as the assets for the rest of the suite, and registered afresh by each test that wants them, since an asset
// whose last reference goes leaves the registry
static auto content_assets() -> ContentAssets {
  const auto root = std::filesystem::temp_directory_path() / "ox_dotnet_content_test";
  write_file(root / "Audio" / "beep.wav", tone_wav(0.2f));
  write_file(root / "Config" / "hello.txt", "hello from the vfs");
  write_file(root / "Scenes" / "next.oxscene", "{}");
  ox::App::get_vfs().mount_dir(ox::VFS::ASSETS_DIR, root);

  auto& assets = ox::App::mod<ox::AssetManager>();
  auto content = ContentAssets{.beep = ox::UUID::generate_random(), .next_scene = ox::UUID::generate_random()};
  assets.register_asset(content.beep, ox::AssetType::Audio, root / "Audio" / "beep.wav", root / "Audio" / "beep.wav");
  assets.register_asset(
    content.next_scene,
    ox::AssetType::Scene,
    root / "Scenes" / "next.oxscene",
    root / "Scenes" / "next.oxscene"
  );
  return content;
}

// what MaterialProbe finds: a loaded material held by a model and by a direct reference, a material nobody loaded and
// three textures. Nothing here can reach a renderer, so the textures and the model are empty payloads
struct MaterialAssets {
  ox::UUID model = {};
  ox::UUID stone = {};
  ox::UUID idle = {};
  ox::UUID texture_a = {};
  ox::UUID texture_b = {};
  ox::UUID texture_c = {};
};

// the stone material starts on textures a and c, and two holders put two references on each of them
static auto material_assets() -> MaterialAssets {
  const auto root = std::filesystem::temp_directory_path() / "ox_dotnet_content_test";
  ox::App::get_vfs().mount_dir(ox::VFS::ASSETS_DIR, root);

  auto& assets = ox::App::mod<ox::AssetManager>();
  auto content = MaterialAssets{
    .model = ox::UUID::generate_random(),
    .stone = ox::UUID::generate_random(),
    .idle = ox::UUID::generate_random(),
    .texture_a = ox::UUID::generate_random(),
    .texture_b = ox::UUID::generate_random(),
    .texture_c = ox::UUID::generate_random(),
  };
  const auto register_asset = [&assets, &root](const ox::UUID& uuid, ox::AssetType type, const c8* path) {
    assets.register_asset(uuid, type, root / path, root / path);
  };
  register_asset(content.texture_a, ox::AssetType::Texture, "Textures/a.png");
  register_asset(content.texture_b, ox::AssetType::Texture, "Textures/b.png");
  register_asset(content.texture_c, ox::AssetType::Texture, "Textures/c.png");
  register_asset(content.stone, ox::AssetType::Material, "Materials/stone.mat");
  register_asset(content.idle, ox::AssetType::Material, "Materials/idle.mat");
  register_asset(content.model, ox::AssetType::Model, "Models/crate.glb");

  assets.publish_texture(content.texture_a, ox::Texture{});
  assets.publish_texture(content.texture_b, ox::Texture{});
  assets.publish_texture(content.texture_c, ox::Texture{});
  assets.set_pending_load_info(
    content.stone,
    ox::Material{
      .albedo_color = {0.25f, 0.5f, 0.75f, 0.5f},
      .uv_size = {2.0f, 4.0f},
      .uv_offset = {0.125f, 0.25f},
      .emissive_color = {1.0f, 2.0f, 3.0f},
      .roughness_factor = 0.625f,
      .metallic_factor = 0.375f,
      .normal_scale = 0.5f,
      .occlusion_strength = 0.75f,
      .alpha_mode = ox::AlphaMode::Mask,
      .alpha_cutoff = 0.75f,
      .sampling_mode = ox::SamplingMode::NearestClamped,
      .flip_normal_y = true,
      .albedo_texture = content.texture_a,
      .normal_texture = content.texture_c,
    }
  );
  assets.load_asset(content.stone);

  auto model = ox::Model{};
  model.materials = {content.stone, ox::UUID::generate_random()};
  assets.publish_model(content.model, std::move(model));
  assets.load_asset(content.model);
  return content;
}

// SmokeGame.Spin, as flecs lays out its members
struct Spin {
  f32 speed = 0.0f;
  f32 angle = 0.0f;
};

// SmokeGame.BodyProbe, VehicleProbe and CameraProbe, as flecs lays out their members
struct BodyProbe {
  i32 command = 0;
  f32 scalar = 0.0f;
  glm::vec3 vector = {};
  glm::vec3 point = {};
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);

  i32 threw = 0;
  i32 motion_type = 0;
  i32 object_layer = 0;
  f32 friction = 0.0f;
  f32 restitution = 0.0f;
  i32 flags = 0;
  glm::vec3 center_of_mass = {};
  glm::vec3 bounds_min = {};
  glm::vec3 bounds_max = {};
  glm::vec3 force = {};
  glm::vec3 torque = {};
  glm::vec3 point_velocity = {};
  glm::vec3 position = {};
  i32 is_active = 0;
  i32 kind = 0;
};

static_assert(sizeof(BodyProbe) == 164);

struct VehicleProbe {
  i32 exists = 0;
  i32 gear = 0;
  f32 engine_rpm = 0.0f;
  f32 engine_angular_velocity = 0.0f;
  f32 forward_speed = 0.0f;
  i32 wheels = 0;
  i32 contacting = 0;
  f32 max_slip = 0.0f;
  i32 threw = 0;
};

static_assert(sizeof(VehicleProbe) == 36);

struct CameraProbe {
  glm::vec2 screen_position = {};
  glm::vec2 screen_size = {};
  glm::vec3 origin = {};
  glm::vec3 direction = {};
  glm::vec3 point = {};
  i32 threw = 0;
  i32 hit_is_wall = 0;
  f32 hit_distance = 0.0f;
};

static_assert(sizeof(CameraProbe) == 64);

// BodyProbe commands, as DriveBody reads them
enum class BodyCommand : i32 {
  None = 0,
  SetMotionType = 1,
  SetObjectLayer = 2,
  SetFrictionAndRestitution = 3,
  SetFlags = 4,
  AddForceAtPoint = 5,
  AddImpulseAtPoint = 6,
  MoveKinematic = 7,
  SetPositionAndRotation = 8,
  AddLinearVelocity = 9,
  SetPosition = 10,
  SetRotation = 11,
  SetAllowedDofs = 12,
  AddTorque = 13,
  Read = 14,
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

  template <typename T>
  auto set_probe(flecs::entity entity, const c8* path, const T& value) -> void {
    ecs_set_id(scene->world.c_ptr(), entity, id_of(path), sizeof(T), &value);
  }

  template <typename T>
  auto get_probe(flecs::entity entity, const c8* path) -> T {
    return *static_cast<const T*>(ecs_get_id(scene->world.c_ptr(), entity, id_of(path)));
  }

  auto drive_body(flecs::entity entity, BodyProbe command) -> BodyProbe {
    set_probe(entity, "SmokeGame.BodyProbe", command);
    step();
    return get_probe<BodyProbe>(entity, "SmokeGame.BodyProbe");
  }

  // no gravity and no drag, so what a probe does to it is all that moves it
  auto probe_body(const c8* name, glm::vec3 position, ox::RigidBodyComponent::BodyType type) -> flecs::entity {
    auto entity = scene->create_entity(name)
                    .set(ox::TransformComponent{.position = position})
                    .set(
                      ox::RigidBodyComponent{
                        .type = type,
                        .linear_drag = 0.0f,
                        .angular_drag = 0.0f,
                        .gravity_factor = 0.0f,
                      }
                    )
                    .set(ox::SphereColliderComponent{.radius = 0.5f});
    set_probe(entity, "SmokeGame.BodyProbe", BodyProbe{});
    return entity;
  }

  auto bodies() -> JPH::BodyInterface& { return scene->get_physics_system()->GetBodyInterface(); }

  auto lock_body(flecs::entity entity) -> JPH::BodyLockRead {
    return JPH::BodyLockRead(scene->get_physics_system()->GetBodyLockInterface(), body_id(entity));
  }

  static auto body_id(flecs::entity entity) -> JPH::BodyID {
    return static_cast<const JPH::Body*>(entity.get<ox::RigidBodyComponent>().runtime_body)->GetID();
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

TEST_F(DotNetSceneTest, SkipsLuaScriptEntriesInSceneJson) {
  const auto lua_uuid = ox::UUID::generate_random().str();
  const auto json = fmt::format(
    R"({{"name":"Legacy","scripts":[{{"uuid":"{}"}},{{"type":"SmokeGame.LevelLogic"}}],"entities":[]}})",
    lua_uuid
  );

  auto warnings = std::vector<std::string>{};
  loguru::add_callback(
    "lua_migration",
    [](void* user_data, const loguru::Message& message) {
      static_cast<std::vector<std::string>*>(user_data)->push_back(message.message);
    },
    &warnings,
    loguru::Verbosity_WARNING
  );

  auto loaded = std::make_shared<ox::Scene>("Loaded");
  const auto is_loaded = loaded->from_json(json);
  loguru::remove_callback("lua_migration");

  EXPECT_TRUE(is_loaded);
  EXPECT_EQ(loaded->get_scripts().size(), 1_sz);
  EXPECT_TRUE(loaded->get_scripts().contains(ox::ScriptKey{.type_name = "SmokeGame.LevelLogic"}));

  const auto is_warned = std::ranges::any_of(warnings, [&](const std::string& warning) {
    return warning.find(lua_uuid) != std::string::npos &&
           warning.find("Lua scene scripts were removed") != std::string::npos;
  });
  EXPECT_TRUE(is_warned);

  EXPECT_EQ(loaded->to_json().stream.str().find(lua_uuid), std::string::npos);
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

TEST_F(DotNetSceneTest, ReachesAssetsAudioScenesAndFilesFromCSharp) {
  const auto content = content_assets();
  auto& assets = ox::App::mod<ox::AssetManager>();
  auto requested = ox::UUID(nullptr);
  const auto handler = ox::App::get_event_system()
                         .subscribe<ox::SceneLoadRequestEvent>([&requested](const ox::SceneLoadRequestEvent& event) {
                           requested = event.scene;
                         })
                         .value();

  scene->create_entity("Speaker").set(ox::AudioSourceComponent{});
  ASSERT_TRUE(scene->add_script_type("SmokeGame.ContentProbe").has_value());
  scene->runtime_start();

  const auto& vfs = ox::App::get_vfs();
  const auto beep_virtual = vfs.to_virtual(vfs.resolve_physical_dir(ox::VFS::ASSETS_DIR, "Audio/beep.wav")).string();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content found True missing False"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content acquired Audio loaded True refs 1"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content spawned True transform True parent True"), 1);
  // the speaker's component took a reference of its own
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content speaker playing True length 0.20 refs 2"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content vfs True hello from the vfs"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content virtual " + beep_virtual), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content refused a sound as a model"), 1);
  EXPECT_EQ(requested, content.next_scene);
  EXPECT_EQ(ox::App::mod<ox::AudioEngine>().playing_one_shot_count(), 2);

  // deferred to the scene's next update, not run inside OnStart
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content deferred ran"), 0);
  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content deferred ran"), 1);

  scene->runtime_stop();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "content released, refs 1"), 1);

  // the speaker going with the scene gives back the last reference
  scene.reset();
  EXPECT_FALSE(assets.is_loaded(content.beep));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
  std::ignore = ox::App::get_event_system().unsubscribe<ox::SceneLoadRequestEvent>(handler);
}

TEST_F(DotNetSceneTest, ReleasesHandlesTheScriptsForgot) {
  const auto content = content_assets();
  auto& assets = ox::App::mod<ox::AssetManager>();
  ASSERT_TRUE(scene->add_script_type("SmokeGame.LeakyProbe").has_value());
  scene->runtime_start();
  ASSERT_TRUE(assets.is_loaded(content.beep));

  scripting().unload();
  EXPECT_EQ(
    count_logs(
      ox::OxLogLevel::Warn,
      "1 asset handles were still held when the scripts unloaded, they are released now"
    ),
    1
  ) << logs_text();
  EXPECT_FALSE(assets.is_loaded(content.beep));
}

TEST_F(DotNetSceneTest, ReadsAndWritesMaterialsAndKeepsTheirTexturesBalanced) {
  auto& assets = ox::App::mod<ox::AssetManager>();
  const auto content = material_assets();
  const auto refs = [&assets](const ox::UUID& uuid) -> u64 {
    return assets.get_asset(uuid)->ref_count;
  };

  EXPECT_EQ(refs(content.stone), 2);
  EXPECT_EQ(refs(content.texture_a), 2);
  EXPECT_EQ(refs(content.texture_b), 0);
  EXPECT_EQ(refs(content.texture_c), 2);
  std::ignore = assets.get_dirty_material_ids();

  ASSERT_TRUE(scene->add_script_type("SmokeGame.MaterialProbe").has_value());
  scene->runtime_start();

  const auto expect_log = [](std::string_view message) {
    EXPECT_EQ(count_logs(ox::OxLogLevel::Info, message), 1) << message << "\n" << logs_text();
  };
  expect_log("material list 2 stone first True");
  expect_log("material short list 2 stone first True");
  expect_log("material of a material 0");
  expect_log("material read True color 0.25 0.50 0.75 0.50 uv 2.00 4.00 offset 0.125 0.25 emissive 1.0 2.0 3.0");
  expect_log(
    "material read roughness 0.625 metallic 0.375 normal 0.50 occlusion 0.75 alpha Mask cutoff 0.75 sampling "
    "NearestClamped flip True"
  );
  expect_log("material read albedo a True normal c True emissive False");
  expect_log("material refs a 2 b 0 c 2");
  expect_log("material written color 0.50 0.25 0.125 1.00 uv 3.00 5.00 offset 0.50 0.75 emissive 4.0 5.0 6.0");
  expect_log(
    "material written roughness 0.500 metallic 0.875 normal 1.50 occlusion 0.25 alpha Blend cutoff 0.50 sampling "
    "LinearClamped flip False"
  );
  expect_log("material written albedo b True normal a True emissive c True");
  expect_log("material refs a 2 b 2 c 2");
  expect_log("material refused idle True texture True unknown True slot True mode True");
  expect_log("material unread idle False texture False unknown False");
  expect_log("material refs after refusals a 2 b 2 c 2");

  // every texture moved with the material's two references: b gained both, a and c only changed slots
  EXPECT_EQ(refs(content.texture_a), 2);
  EXPECT_EQ(refs(content.texture_b), 2);
  EXPECT_EQ(refs(content.texture_c), 2);
  {
    const auto stone = assets.get_material(content.stone);
    ASSERT_TRUE(static_cast<bool>(stone));
    EXPECT_EQ(stone->albedo_texture, content.texture_b);
    EXPECT_EQ(stone->normal_texture, content.texture_a);
    EXPECT_EQ(stone->emissive_texture, content.texture_c);
    EXPECT_EQ(stone->alpha_mode, ox::AlphaMode::Blend);
    EXPECT_EQ(stone->sampling_mode, ox::SamplingMode::LinearClamped);
    EXPECT_FALSE(stone->flip_normal_y);
    EXPECT_EQ(stone->albedo_color, glm::vec4(0.5f, 0.25f, 0.125f, 1.0f));
  }

  // the renderer re-uploads it once, and the refused writes marked nothing
  const auto dirty = assets.get_dirty_material_ids();
  ASSERT_EQ(dirty.size(), 1);
  EXPECT_EQ(dirty.front(), assets.get_asset(content.stone)->material_id);

  // dropping every texture gives back all the references they held for the material
  scene->runtime_stop();
  expect_log("material cleared refs a 0 b 0 c 0");
  for (const auto& texture : {content.texture_a, content.texture_b, content.texture_c}) {
    EXPECT_EQ(refs(texture), 0);
    EXPECT_FALSE(assets.is_loaded(texture));
  }

  {
    const auto stone = assets.get_material(content.stone);
    ASSERT_TRUE(static_cast<bool>(stone));
    EXPECT_FALSE(static_cast<bool>(stone->albedo_texture));
    EXPECT_FALSE(static_cast<bool>(stone->normal_texture));
    EXPECT_FALSE(static_cast<bool>(stone->emissive_texture));
  }

  EXPECT_EQ(assets.get_dirty_material_ids().size(), 1);

  // letting go of the material afterwards must not take anything below zero
  assets.unload_asset(content.model);
  assets.unload_asset(content.stone);
  EXPECT_FALSE(assets.is_loaded(content.stone));
  EXPECT_FALSE(assets.is_loaded(content.model));
  for (const auto& texture : {content.texture_a, content.texture_b, content.texture_c}) {
    EXPECT_EQ(refs(texture), 0);
  }

  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetSceneTest, ControlsAnEmitterFromCSharp) {
  auto emitter = scene->create_entity("Emitter").set(ox::ParticleSystemComponent{});
  scene->create_entity("Plain");
  behaviour_entity("Probe", "SmokeGame.ParticleProbe");

  scene->runtime_start();

  // the component's observer starts it from the default play_on_awake, before the set value lands
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "particles awake True"), 1) << logs_text();
  // there is no particle asset to name a parameter in, so the by-name overload has nothing to find
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "particles played True stopped True named False"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "particles restarted True"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "particles plain False"), 1) << logs_text();

  const auto* state = scene->particle_emitter_state(emitter);
  ASSERT_NE(state, nullptr);
  EXPECT_TRUE(state->playing);
  EXPECT_EQ(state->pending_burst, 8);
  const auto& component = emitter.get<ox::ParticleSystemComponent>();
  EXPECT_TRUE(component.override_parameters);
  EXPECT_EQ(component.param2, glm::vec4(1.0f, 2.0f, 3.0f, 4.0f));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetSceneTest, ControlsACinematicPlayerFromCSharp) {
  // no cinematic asset to give them, so the players have an instance but no duration to seek within
  auto running = scene->create_entity("PlayerA").set(ox::CinematicPlayerComponent{});
  auto stopped = scene->create_entity("PlayerB").set(ox::CinematicPlayerComponent{});
  scene->create_entity("Plain");
  behaviour_entity("Probe", "SmokeGame.CinematicProbe");

  scene->runtime_start();

  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "cinematic idle 0 0"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "cinematic seeked 0 0"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "cinematic plain 0 0"), 1) << logs_text();
  EXPECT_TRUE(running.get<ox::CinematicPlayerComponent>().playing);
  EXPECT_FALSE(stopped.get<ox::CinematicPlayerComponent>().playing);
  ASSERT_NE(scene->cinematic_instance(running), nullptr);
  EXPECT_TRUE(scene->cinematic_instance(running)->seek_pending);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetSceneTest, MirrorsUuidFieldsOfComponents) {
  behaviour_entity("Sprites", "SmokeGame.SpriteProbe");
  scene->runtime_start();

  const auto first = scene->world.lookup("SpriteA");
  const auto second = scene->world.lookup("SpriteB");
  ASSERT_TRUE(first.is_valid());
  ASSERT_TRUE(second.is_valid());

  // C# swapped the two materials after reading them, so what C++ sees now is the other's
  const auto& a = second.get<ox::SpriteComponent>().material;
  const auto& b = first.get<ox::SpriteComponent>().material;
  EXPECT_TRUE(a);
  EXPECT_TRUE(b);
  EXPECT_NE(a, b);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, std::format("sprite materials {} {} valid True", a.str(), b.str())), 1)
    << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "sprite swapped"), 1) << logs_text();
}

TEST_F(DotNetSceneTest, DestroysASpriteInOnStop) {
  ASSERT_TRUE(scene->add_script_type("SmokeGame.SpriteTeardownProbe").has_value());
  scene->runtime_start();
  step();
  scene->runtime_stop();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "doomed sprite material True"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "doomed sprite destroyed"), 1) << logs_text();
  scene.reset();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetSceneTest, ReadsTheScenesStateFromCSharp) {
  behaviour_entity("Info", "SmokeGame.SceneInfoProbe");
  scene->runtime_start();

  const auto uuid = scene->get_uuid().str();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "scene uuid " + uuid + " valid True"), 1) << logs_text();
  // the fixture has no RmlUi view, so the DPI calls are the no-op path
  EXPECT_TRUE(scene->get_rml_context_name().empty());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "scene dpi set and cleared"), 1) << logs_text();

  scene->input_focused = true;
  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "scene focused True"), 1) << logs_text();

  scene->input_focused = false;
  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "scene focused False"), 1) << logs_text();
}

TEST_F(DotNetSceneTest, ForwardsWindowResizesAndTheFrameLimit) {
  behaviour_entity("App", "SmokeGame.AppProbe");
  scene->runtime_start();

  // the app has no window
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "window size 0 0 logical 0 0"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "time default -1"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "time limited 8"), 1) << logs_text();
  EXPECT_DOUBLE_EQ(ox::App::get_timestep().get_max_frame_time(), 8.0);

  std::ignore = ox::App::get_event_system().emit(ox::WindowResizeEvent{.width = 1280, .height = 720});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "window resized 1280 720"), 1) << logs_text();

  behaviour_entity("Reset", "SmokeGame.AppResetProbe");
  step();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "time reset -1"), 1) << logs_text();
  EXPECT_DOUBLE_EQ(ox::App::get_timestep().get_max_frame_time(), -1.0);

  // the handlers go with the scripts
  scripting().unload();
  std::ignore = ox::App::get_event_system().emit(ox::WindowResizeEvent{.width = 640, .height = 480});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "window resized 640 480"), 0) << logs_text();
}

static auto command_of(BodyCommand command) -> i32 { return static_cast<i32>(command); }

static auto expect_near(glm::vec3 actual, glm::vec3 expected, f32 tolerance = 1.0e-4f) -> void {
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
  EXPECT_NEAR(actual.z, expected.z, tolerance);
}

static auto to_glm(JPH::Vec3Arg v) -> glm::vec3 { return {v.GetX(), v.GetY(), v.GetZ()}; }

TEST_F(DotNetSceneTest, ChangesABodysMotionTypeFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  auto statue = probe_body("Statue", {4.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Static);
  scene->runtime_start();

  auto result = drive_body(ball, {.command = command_of(BodyCommand::Read)});
  EXPECT_EQ(result.kind, 4);
  EXPECT_EQ(bodies().GetMotionType(body_id(ball)), JPH::EMotionType::Dynamic);

  result = drive_body(ball, {.command = command_of(BodyCommand::SetMotionType), .scalar = 1.0f});
  EXPECT_EQ(result.threw, 0) << logs_text();
  EXPECT_EQ(result.kind, 2);
  EXPECT_EQ(bodies().GetMotionType(body_id(ball)), JPH::EMotionType::Kinematic);

  result = drive_body(ball, {.command = command_of(BodyCommand::SetMotionType), .scalar = 2.0f});
  EXPECT_EQ(result.threw, 0);
  EXPECT_EQ(result.kind, 4);
  EXPECT_EQ(bodies().GetMotionType(body_id(ball)), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(bodies().IsActive(body_id(ball)));

  // a body made static never got the motion properties a kinematic or dynamic one needs
  result = drive_body(statue, {.command = command_of(BodyCommand::SetMotionType), .scalar = 2.0f});
  EXPECT_EQ(result.threw, 1);
  EXPECT_EQ(result.kind, 1);
  EXPECT_EQ(bodies().GetMotionType(body_id(statue)), JPH::EMotionType::Static);
}

TEST_F(DotNetSceneTest, MovesABodyBetweenObjectLayersFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();

  auto result = drive_body(ball, {.command = command_of(BodyCommand::Read)});
  EXPECT_EQ(result.object_layer, 2);
  EXPECT_EQ(bodies().GetObjectLayer(body_id(ball)), PhysicsLayers::MOVING);

  result = drive_body(ball, {.command = command_of(BodyCommand::SetObjectLayer), .scalar = 1.0f});
  EXPECT_EQ(result.threw, 0);
  EXPECT_EQ(result.object_layer, 1);
  EXPECT_EQ(bodies().GetObjectLayer(body_id(ball)), PhysicsLayers::NON_MOVING);

  result = drive_body(ball, {.command = command_of(BodyCommand::SetObjectLayer), .scalar = 2.0f});
  EXPECT_EQ(result.object_layer, 2);
  EXPECT_EQ(bodies().GetObjectLayer(body_id(ball)), PhysicsLayers::MOVING);

  // a layer the engine doesn't have
  result = drive_body(ball, {.command = command_of(BodyCommand::SetObjectLayer), .scalar = 4.0f});
  EXPECT_EQ(result.threw, 1);
  EXPECT_EQ(bodies().GetObjectLayer(body_id(ball)), PhysicsLayers::MOVING);
}

TEST_F(DotNetSceneTest, RoundTripsFrictionAndRestitutionFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();

  auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetFrictionAndRestitution), .scalar = 0.75f, .vector = {0.4f, 0.0f, 0.0f}}
  );
  EXPECT_EQ(result.threw, 0);
  EXPECT_FLOAT_EQ(result.friction, 0.75f);
  EXPECT_FLOAT_EQ(result.restitution, 0.4f);
  EXPECT_FLOAT_EQ(bodies().GetFriction(body_id(ball)), 0.75f);
  EXPECT_FLOAT_EQ(bodies().GetRestitution(body_id(ball)), 0.4f);

  result = drive_body(ball, {.command = command_of(BodyCommand::SetFrictionAndRestitution), .scalar = -1.0f});
  EXPECT_EQ(result.threw, 1);
  EXPECT_FLOAT_EQ(bodies().GetFriction(body_id(ball)), 0.75f);
}

TEST_F(DotNetSceneTest, RoundTripsABodysFlagsFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();

  const auto flags_in_jolt = [&] {
    const auto lock = lock_body(ball);
    const auto& body = lock.GetBody();
    return (body.IsSensor() ? 1 : 0) | (body.GetCollideKinematicVsNonDynamic() ? 2 : 0) |
           (body.GetUseManifoldReduction() ? 4 : 0) | (body.GetApplyGyroscopicForce() ? 8 : 0) |
           (body.GetEnhancedInternalEdgeRemoval() ? 16 : 0);
  };

  auto result = drive_body(ball, {.command = command_of(BodyCommand::SetFlags), .scalar = 0.0f});
  EXPECT_EQ(result.flags, 0);
  EXPECT_EQ(flags_in_jolt(), 0);
  EXPECT_FALSE(bodies().IsSensor(body_id(ball)));

  result = drive_body(ball, {.command = command_of(BodyCommand::SetFlags), .scalar = 31.0f});
  EXPECT_EQ(result.threw, 0);
  EXPECT_EQ(result.flags, 31);
  EXPECT_EQ(flags_in_jolt(), 31);
  EXPECT_TRUE(bodies().IsSensor(body_id(ball)));

  for (const auto flags : {1, 2, 4, 8, 16, 21}) {
    result = drive_body(ball, {.command = command_of(BodyCommand::SetFlags), .scalar = static_cast<f32>(flags)});
    EXPECT_EQ(result.flags, flags);
    EXPECT_EQ(flags_in_jolt(), flags);
  }
}

TEST_F(DotNetSceneTest, SpinsABodyWithAForceAtAPointFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  auto floor = probe_body("Floor", {0.0f, -5.0f, 0.0f}, ox::RigidBodyComponent::Static);
  scene->runtime_start();

  // the point is a metre off the centre, so a push up there is a torque around z
  auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::AddForceAtPoint), .vector = {0.0f, 10.0f, 0.0f}, .point = {1.0f, 3.0f, 0.0f}}
  );
  EXPECT_EQ(result.threw, 0) << logs_text();
  expect_near(result.force, {0.0f, 10.0f, 0.0f});
  EXPECT_NEAR(result.torque.z, 10.0f, 1.0e-4f);
  EXPECT_EQ(bodies().GetAngularVelocity(body_id(ball)).GetZ(), 0.0f);

  for (auto i = 0; i < 3 && bodies().GetAngularVelocity(body_id(ball)).GetZ() == 0.0f; i++) {
    step();
  }

  EXPECT_GT(bodies().GetAngularVelocity(body_id(ball)).GetZ(), 0.0f);
  EXPECT_GT(bodies().GetLinearVelocity(body_id(ball)).GetY(), 0.0f);

  result = drive_body(floor, {.command = command_of(BodyCommand::AddForceAtPoint), .vector = {0.0f, 1.0f, 0.0f}});
  EXPECT_EQ(result.threw, 1);
  expect_near(result.force, {});
}

TEST_F(DotNetSceneTest, SpinsABodyWithAnImpulseAtAPointFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();

  auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::AddImpulseAtPoint), .vector = {0.0f, 1.0f, 0.0f}, .point = {1.0f, 3.0f, 0.0f}}
  );
  EXPECT_EQ(result.threw, 0) << logs_text();
  EXPECT_GT(bodies().GetAngularVelocity(body_id(ball)).GetZ(), 0.0f);
  EXPECT_GT(bodies().GetLinearVelocity(body_id(ball)).GetY(), 0.0f);

  const auto before = to_glm(bodies().GetLinearVelocity(body_id(ball)));
  result = drive_body(ball, {.command = command_of(BodyCommand::AddLinearVelocity), .vector = {2.0f, 0.0f, 0.0f}});
  EXPECT_EQ(result.threw, 0);
  expect_near(to_glm(bodies().GetLinearVelocity(body_id(ball))), before + glm::vec3(2.0f, 0.0f, 0.0f));
}

TEST_F(DotNetSceneTest, ReadsTheVelocityOfAPointOnABodyFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();
  bodies().SetLinearVelocity(body_id(ball), JPH::Vec3(1.0f, 0.0f, 0.0f));
  bodies().SetAngularVelocity(body_id(ball), JPH::Vec3(0.0f, 0.0f, 2.0f));

  const auto point = glm::vec3(0.0f, 4.0f, 0.5f);
  const auto result = drive_body(ball, {.command = command_of(BodyCommand::Read), .point = point});
  const auto lock = lock_body(ball);
  const auto& body = lock.GetBody();
  const auto expected = to_glm(body.GetLinearVelocity()) +
                        glm::cross(to_glm(body.GetAngularVelocity()), point - to_glm(body.GetCenterOfMassPosition()));
  expect_near(result.point_velocity, expected);
  expect_near(result.point_velocity, to_glm(body.GetPointVelocity(JPH::RVec3(point.x, point.y, point.z))));
  EXPECT_GT(glm::length(result.point_velocity - to_glm(body.GetLinearVelocity())), 0.5f);
}

TEST_F(DotNetSceneTest, MovesAKinematicBodyToATargetFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  scene->runtime_start();
  drive_body(ball, {.command = command_of(BodyCommand::SetMotionType), .scalar = 1.0f});

  const auto target = glm::vec3(2.0f, 3.0f, -1.0f);
  const auto turn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::MoveKinematic), .scalar = FRAME, .point = target, .rotation = turn}
  );
  EXPECT_EQ(result.threw, 0) << logs_text();

  auto steps = 0;
  while (steps < 3 && glm::length(to_glm(bodies().GetPosition(body_id(ball))) - target) > 1.0e-3f) {
    step();
    steps++;
  }

  EXPECT_LE(steps, 2);
  expect_near(to_glm(bodies().GetPosition(body_id(ball))), target, 1.0e-3f);
  const auto rotation = bodies().GetRotation(body_id(ball));
  EXPECT_NEAR(std::abs(rotation.GetY()), std::abs(turn.y), 1.0e-3f);
  EXPECT_NEAR(std::abs(rotation.GetW()), std::abs(turn.w), 1.0e-3f);
}

TEST_F(DotNetSceneTest, TeleportsABodyAndWakesItFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  auto statue = probe_body("Statue", {4.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Static);
  scene->runtime_start();
  const auto turn = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

  bodies().DeactivateBody(body_id(ball));
  ASSERT_FALSE(bodies().IsActive(body_id(ball)));
  auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetPositionAndRotation), .point = {5.0f, 3.0f, 1.0f}, .rotation = turn}
  );
  EXPECT_EQ(result.threw, 0) << logs_text();
  EXPECT_EQ(result.is_active, 1);
  EXPECT_TRUE(bodies().IsActive(body_id(ball)));
  expect_near(to_glm(bodies().GetPosition(body_id(ball))), {5.0f, 3.0f, 1.0f});
  EXPECT_NEAR(std::abs(bodies().GetRotation(body_id(ball)).GetX()), std::abs(turn.x), 1.0e-4f);

  bodies().DeactivateBody(body_id(ball));
  result = drive_body(ball, {.command = command_of(BodyCommand::SetPosition), .point = {-2.0f, 3.0f, 0.0f}});
  EXPECT_EQ(result.is_active, 1);
  expect_near(to_glm(bodies().GetPosition(body_id(ball))), {-2.0f, 3.0f, 0.0f});

  bodies().DeactivateBody(body_id(ball));
  result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetRotation), .rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f)}
  );
  EXPECT_EQ(result.is_active, 1);
  EXPECT_NEAR(std::abs(bodies().GetRotation(body_id(ball)).GetW()), 1.0f, 1.0e-4f);

  // a zero quaternion has no direction to turn to
  result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetRotation), .rotation = glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f)}
  );
  EXPECT_EQ(result.threw, 1);

  // a static body can be placed, it just can't move on its own
  result = drive_body(statue, {.command = command_of(BodyCommand::SetPosition), .point = {6.0f, 3.0f, 0.0f}});
  EXPECT_EQ(result.threw, 0);
  expect_near(to_glm(bodies().GetPosition(body_id(statue))), {6.0f, 3.0f, 0.0f});
}

TEST_F(DotNetSceneTest, ReadsABodysCentreBoundsAndForcesFromCSharp) {
  auto ball = scene->create_entity("Ball")
                .set(ox::TransformComponent{.position = {0.0f, 3.0f, 0.0f}})
                .set(
                  ox::RigidBodyComponent{
                    .type = ox::RigidBodyComponent::Dynamic,
                    .gravity_factor = 0.0f,
                    .center_of_mass_offset = {0.0f, 0.25f, 0.0f},
                  }
                )
                .set(ox::SphereColliderComponent{.radius = 0.5f});
  set_probe(ball, "SmokeGame.BodyProbe", BodyProbe{});
  scene->runtime_start();

  auto result = drive_body(ball, {.command = command_of(BodyCommand::Read)});
  const auto centre = bodies().GetCenterOfMassPosition(body_id(ball));
  expect_near(result.center_of_mass, to_glm(centre));
  expect_near(result.position, to_glm(bodies().GetPosition(body_id(ball))));
  EXPECT_GT(glm::length(result.center_of_mass - result.position), 0.2f);

  {
    const auto lock = lock_body(ball);
    const auto& bounds = lock.GetBody().GetWorldSpaceBounds();
    expect_near(result.bounds_min, to_glm(bounds.mMin));
    expect_near(result.bounds_max, to_glm(bounds.mMax));
  }

  EXPECT_TRUE(glm::all(glm::lessThanEqual(result.bounds_min, result.position)));
  EXPECT_TRUE(glm::all(glm::greaterThanEqual(result.bounds_max, result.position)));
  EXPECT_GT(result.bounds_max.y - result.bounds_min.y, 0.9f);
  expect_near(result.force, {});

  result = drive_body(ball, {.command = command_of(BodyCommand::AddTorque), .vector = {0.0f, 3.0f, 0.0f}});
  expect_near(result.torque, {0.0f, 3.0f, 0.0f});
}

TEST_F(DotNetSceneTest, WritesAllowedDofsFromCSharp) {
  auto ball = probe_body("Ball", {0.0f, 3.0f, 0.0f}, ox::RigidBodyComponent::Dynamic);
  EXPECT_EQ(ball.get<ox::RigidBodyComponent>().allowed_dofs, ox::RigidBodyComponent::All);
  scene->runtime_start();

  constexpr auto translation_x = ox::RigidBodyComponent::TranslationX;
  constexpr auto rotation_y = ox::RigidBodyComponent::RotationY;
  auto result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetAllowedDofs), .scalar = static_cast<f32>(translation_x | rotation_y)}
  );
  EXPECT_EQ(result.threw, 0);
  EXPECT_EQ(ball.get<ox::RigidBodyComponent>().allowed_dofs, translation_x | rotation_y);

  result = drive_body(
    ball,
    {.command = command_of(BodyCommand::SetAllowedDofs), .scalar = static_cast<f32>(ox::RigidBodyComponent::Plane2D)}
  );
  EXPECT_EQ(ball.get<ox::RigidBodyComponent>().allowed_dofs, 0b100011_u32);
}

TEST_F(DotNetSceneTest, CastsRaysThroughTheScreenFromCSharp) {
  auto camera = scene->create_entity("Camera")
                  .set(ox::TransformComponent{.position = {1.0f, 2.0f, 3.0f}})
                  .set(ox::CameraComponent{});
  // camera_update only runs with a renderer, so the camera is brought up to date here
  ox::Camera::update(camera.get_mut<ox::CameraComponent>(), camera.get<ox::TransformComponent>(), {1280.0f, 720.0f});
  const auto forward = camera.get<ox::CameraComponent>().forward;
  const auto position = camera.get<ox::TransformComponent>().position;
  expect_near(forward, {0.0f, 0.0f, -1.0f});

  scene->create_entity("Wall")
    .set(ox::TransformComponent{.position = position + forward * 10.0f})
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {3.0f, 3.0f, 0.5f}});
  auto nowhere = scene->create_entity("Nowhere");
  auto stale = scene->create_entity("Stale").set(ox::CameraComponent{});

  const auto probe = CameraProbe{.screen_position = {640.0f, 360.0f}, .screen_size = {1280.0f, 720.0f}};
  set_probe(camera, "SmokeGame.CameraProbe", probe);
  set_probe(nowhere, "SmokeGame.CameraProbe", probe);
  set_probe(stale, "SmokeGame.CameraProbe", probe);
  scene->runtime_start();
  step();

  auto result = get_probe<CameraProbe>(camera, "SmokeGame.CameraProbe");
  EXPECT_EQ(result.threw, 0) << logs_text();
  expect_near(result.direction, forward);
  expect_near(result.origin, position + forward * 0.01f, 1.0e-3f);
  expect_near(result.point, result.origin + forward * 2.0f, 1.0e-3f);
  // the wall's near face is 9.5 from the camera, and the ray starts on the near plane
  EXPECT_EQ(result.hit_is_wall, 1);
  EXPECT_NEAR(result.hit_distance, 9.49f, 0.02f);

  auto corner = probe;
  corner.screen_position = {0.0f, 0.0f};
  set_probe(camera, "SmokeGame.CameraProbe", corner);
  step();
  result = get_probe<CameraProbe>(camera, "SmokeGame.CameraProbe");
  EXPECT_NEAR(glm::length(result.direction), 1.0f, 1.0e-4f);
  EXPECT_LT(glm::dot(result.direction, forward), 0.95f);
  EXPECT_GT(glm::dot(result.direction, forward), 0.5f);

  EXPECT_EQ(get_probe<CameraProbe>(nowhere, "SmokeGame.CameraProbe").threw, 1);

  // its matrices were never computed, which is a ray that goes nowhere rather than NaNs
  const auto stale_result = get_probe<CameraProbe>(stale, "SmokeGame.CameraProbe");
  EXPECT_EQ(stale_result.threw, 0);
  expect_near(stale_result.direction, {});
  EXPECT_EQ(stale_result.hit_is_wall, 0);
}

TEST_F(DotNetSceneTest, ReadsAVehicleAndItsWheelsFromCSharp) {
  scene->create_entity("Floor")
    .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Static})
    .set(ox::BoxColliderComponent{.size = {50.0f, 0.5f, 50.0f}});

  auto car = scene->create_entity("Car")
               .set(ox::TransformComponent{.position = {0.0f, 1.4f, 0.0f}})
               .set(ox::RigidBodyComponent{.type = ox::RigidBodyComponent::Dynamic, .mass = 1200.0f})
               .set(ox::BoxColliderComponent{.size = {0.8f, 0.2f, 1.8f}})
               .set(ox::VehicleComponent{.input_forward = 1.0f});
  const auto wheels = std::array{
    std::pair{"WheelFL", glm::vec3(-0.9f, -0.2f, 1.2f)},
    std::pair{"WheelFR", glm::vec3(0.9f, -0.2f, 1.2f)},
    std::pair{"WheelRL", glm::vec3(-0.9f, -0.2f, -1.2f)},
    std::pair{"WheelRR", glm::vec3(0.9f, -0.2f, -1.2f)},
  };
  for (const auto& [name, position] : wheels) {
    scene->create_entity(name)
      .child_of(car)
      .set(ox::TransformComponent{.position = position})
      .set(ox::VehicleWheelComponent{.radius = 0.35f, .width = 0.25f});
  }

  auto plain = scene->create_entity("Plain");
  set_probe(car, "SmokeGame.VehicleProbe", VehicleProbe{});
  set_probe(plain, "SmokeGame.VehicleProbe", VehicleProbe{});

  // the constraint is created when the scene starts
  step();
  EXPECT_EQ(get_probe<VehicleProbe>(car, "SmokeGame.VehicleProbe").exists, 0);

  scene->runtime_start();
  step();
  auto result = get_probe<VehicleProbe>(car, "SmokeGame.VehicleProbe");
  EXPECT_EQ(result.exists, 1) << logs_text();
  EXPECT_EQ(result.threw, 0);
  EXPECT_EQ(result.wheels, 4);
  // up in the air
  EXPECT_EQ(result.contacting, 0);
  EXPECT_FLOAT_EQ(result.max_slip, 0.0f);

  for (auto i = 0; i < 180; i++) {
    step();
  }

  result = get_probe<VehicleProbe>(car, "SmokeGame.VehicleProbe");
  EXPECT_EQ(result.contacting, 4) << logs_text();
  EXPECT_GE(result.gear, 1);
  EXPECT_GE(result.engine_rpm, 1000.0f);
  EXPECT_LE(result.engine_rpm, 6000.0f);
  EXPECT_GT(result.engine_angular_velocity, 0.0f);
  EXPECT_GT(result.forward_speed, 0.5f);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();

  const auto absent = get_probe<VehicleProbe>(plain, "SmokeGame.VehicleProbe");
  EXPECT_EQ(absent.exists, 0);
  EXPECT_EQ(absent.threw, 1);
}
