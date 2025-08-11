#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../OxHelpers.hpp"
#include "Scripting/LuaSystem.hpp"

class FlecsBindingsTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_ERROR; // only stdout errors from oxylus

    app = create_test_app();

    scene = create_test_scene();
  }

  void TearDown() override {
    scene.reset();
    app.reset();
  }

  std::unique_ptr<TestApp> app = nullptr;
  std::unique_ptr<ox::Scene> scene = nullptr;
};

TEST_F(FlecsBindingsTest, CreateEntity) {
  std::string lua = R"(
function on_add()
  world:entity("test_entity");
end
)";

  ox::LuaSystem test_system = {};
  test_system.load({}, lua);

  flecs::entity entity = flecs::entity::null();
  ox::f32 delta_time = 0;
  test_system.bind_globals(scene.get(), entity, delta_time);

  test_system.on_add(scene.get(), entity);

  bool test_entity_exists = false;
  auto test_entity = scene->world.lookup("test_entity");
  test_entity_exists = test_entity.is_alive() && test_entity.is_valid();

  EXPECT_TRUE(test_entity_exists);
}