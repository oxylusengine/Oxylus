#pragma once

// C# input through a real window, driven the way TestInputSystem drives it: SDL events from the virtual devices,
// pumped by the window. Each test binary defines smoke_game() first

#include <SDL3/SDL_hints.h>
#include <expected>
#include <flecs.h>
#include <glm/vec2.hpp>
#include <gtest/gtest.h>
#include <string>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/VirtualController.hpp"
#include "Core/VirtualKeyboard.hpp"
#include "Core/VirtualMouse.hpp"
#include "DotNetTestHelpers.hpp"
#include "Scene/Components.hpp"

// SmokeGame.InputProbe, as flecs lays out its members
struct InputProbe {
  i32 bound = 0;
  i32 key_held = 0;
  i32 key_pressed = 0;
  i32 key_released = 0;
  i32 mouse_held = 0;
  i32 action_pressed = 0;
  i32 action_held = 0;
  i32 gamepads = 0;
  i32 gamepad_held = 0;
  f32 gamepad_axis = 0.0f;
  glm::vec2 mouse_position = {};
};

struct UpdateCounter {
  static constexpr auto MODULE_NAME = "UpdateCounter";

  auto init(this UpdateCounter&) -> std::expected<void, std::string> { return {}; }

  auto deinit(this UpdateCounter&) -> std::expected<void, std::string> { return {}; }

  auto update(this UpdateCounter& self, const ox::Timestep&) -> void { self.updates++; }

  i32 updates = 0;
};

class DotNetInputTest : public ::testing::Test {
protected:
  void SetUp() override {
    static char arg0[] = "TestDotNetInput";
    static char* argv[] = {arg0, nullptr};
    captured_logs.clear();

    // SDL drops gamepad input while none of the app's windows has focus, which a test window often doesn't
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    app = std::make_unique<ox::App>(1, argv);
    app->with_name("TestDotNetInput")
      .with_window(ox::WindowInfo{.title = "TestDotNetInput", .width = 10, .height = 10})
      .with<ox::Input>()
      .with<UpdateCounter>();
    app->init();

    world.import<ox::CoreComponentsModule>();
    const auto loaded = scripting.load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const auto attached = scripting.attach_world(world.c_ptr());
    ASSERT_TRUE(attached.has_value()) << attached.error();

    input_probe = ecs_lookup(world.c_ptr(), "SmokeGame.InputProbe");
    ASSERT_NE(input_probe, 0);
    probe = world.entity();
    const auto initial = InputProbe{};
    ecs_set_id(world.c_ptr(), probe, input_probe, sizeof(InputProbe), &initial);

    // the first frame binds the probe's action from C#
    frame();
  }

  void TearDown() override {
    scripting.detach_world(world.c_ptr());
    scripting.unload();
    app->stop();
    app.reset();
  }

  // pumps the events the virtual devices pushed, runs the probe system, and ends the frame as App::step does
  auto frame() -> const InputProbe& {
    app->get_window().update(app->get_timestep());
    world.progress(1.0f / 60.0f);
    app->mod<ox::Input>().reset_pressed();
    return *static_cast<const InputProbe*>(ecs_get_id(world.c_ptr(), probe, input_probe));
  }

  std::unique_ptr<ox::App> app = nullptr;
  flecs::world world;
  ox::DotNetScripting scripting = {};
  flecs::entity_t input_probe = 0;
  flecs::entity probe = {};

  ox::VirtualKeyboard keyboard = {};
  ox::VirtualMouse mouse = {};
};

TEST_F(DotNetInputTest, BindsAnActionFromCSharp) {
  EXPECT_NE(app->mod<ox::Input>().get_binding("probe_fire"), nullptr);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0);
}

TEST_F(DotNetInputTest, ReadsKeysAndActions) {
  keyboard.press(ox::ScanCode::W);
  keyboard.press(ox::ScanCode::F);
  const auto pressed = frame();
  EXPECT_EQ(pressed.key_held, 1);
  EXPECT_EQ(pressed.key_pressed, 1);
  EXPECT_EQ(pressed.action_pressed, 1);
  EXPECT_EQ(pressed.action_held, 1);

  // still down a frame later, but no longer pressed this frame
  const auto held = frame();
  EXPECT_EQ(held.key_held, 1);
  EXPECT_EQ(held.key_pressed, 0);
  EXPECT_EQ(held.action_pressed, 0);
  EXPECT_EQ(held.action_held, 1);

  keyboard.release(ox::ScanCode::W);
  const auto released = frame();
  EXPECT_EQ(released.key_held, 0);
  EXPECT_EQ(released.key_released, 1);
}

TEST_F(DotNetInputTest, ReadsTheMouse) {
  mouse.press(ox::MouseCode::Left);
  EXPECT_EQ(frame().mouse_held, 1);

  mouse.release(ox::MouseCode::Left);
  EXPECT_EQ(frame().mouse_held, 0);
}

TEST_F(DotNetInputTest, ReadsAGamepad) {
  auto controller = ox::VirtualController{};
  controller.connect();
  frame();

  controller.simulate_button(ox::GamepadButtonCode::East, true);
  controller.simulate_axis(ox::GamepadAxisCode::AxisLeftX, 0.5f);
  const auto& seen = frame();
  EXPECT_GE(seen.gamepads, 1);
  EXPECT_NEAR(seen.gamepad_axis, 0.5f, 0.01f);
  EXPECT_EQ(seen.gamepad_held, 1);

  controller.disconnect();
}

TEST_F(DotNetInputTest, QuitsTheAppFromCSharp) {
  auto& counter = app->mod<UpdateCounter>();
  app->step();
  const auto before = counter.updates;
  EXPECT_GT(before, 0);

  world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.QuitProbe"});
  scripting.start_world(world.c_ptr());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "app quit called"), 1) << logs_text();

  app->step();
  EXPECT_EQ(counter.updates, before);
}
