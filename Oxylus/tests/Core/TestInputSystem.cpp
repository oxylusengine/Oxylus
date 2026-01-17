#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../TestHelpers.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/VirtualKeyboard.hpp"

class InputManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    static char arg0[] = "testarg";
    static char* test_argv[] = {arg0, nullptr};
    int argc = 1;
    char** argv = test_argv;

    app = std::make_unique<ox::App>(argc, argv);
    app->with_name("InputManagerTestApp")
      .with_window(
        ox::WindowInfo{
          .title = "InputManagerTestApp",
          .width = 10,
          .height = 10,
          .flags = ox::WindowFlag::Centered | ox::WindowFlag::Resizable | ox::WindowFlag::HighPixelDensity,
        }
      );
    app->with<ox::Input>();

    app->init();
  }

  void TearDown() override {
    app->stop();
    app.reset();
    vk_keyboard.reset();
  }

  std::unique_ptr<ox::App> app = nullptr;
  std::unique_ptr<ox::VirtualKeyboard> vk_keyboard = std::make_unique<ox::VirtualKeyboard>();
};

// --- Basic Functionality Tests ---

TEST_F(InputManagerTest, KeyboardTestPressed) {
  bool pressed = false;
  bool pressed_callback = false;

  bool pressed_mod = false;
  bool pressed_callback_mod = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_pressed",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A)},
      .on_pressed_callback = [&pressed_callback](const ox::ActionContext&) { pressed_callback = true; }
    }
  );
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_pressed_mod",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A, ox::ModCode::AnyControl)},
      .on_pressed_callback = [&pressed_callback_mod](const ox::ActionContext&) { pressed_callback_mod = true; }
    }
  );

  vk_keyboard->press(ox::KeyCode::A);
  vk_keyboard->press(ox::KeyCode::A, ox::ModCode::AnyControl);

  app->get_window().update(app->get_timestep());

  if (input.get_action_pressed("test_pressed")) {
    pressed = true;
  }

  if (input.get_action_pressed("test_pressed_mod")) {
    pressed_mod = true;
  }

  EXPECT_TRUE(pressed);
  EXPECT_TRUE(pressed_callback);
  EXPECT_TRUE(pressed_mod);
  EXPECT_TRUE(pressed_callback_mod);
}

TEST_F(InputManagerTest, KeyboardTestReleased) {
  bool released = false;
  bool released_callback = false;

  bool released_mod = false;
  bool released_callback_mod = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_released",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A)},
      .on_released_callback = [&released_callback](const ox::ActionContext&) { released_callback = true; }
    }
  );
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_released_mod",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A, ox::ModCode::AnyControl)},
      .on_released_callback = [&released_callback_mod](const ox::ActionContext&) { released_callback_mod = true; }
    }
  );

  vk_keyboard->release(ox::KeyCode::A);
  vk_keyboard->release(ox::KeyCode::A, ox::ModCode::AnyControl);

  app->get_window().update(app->get_timestep());

  if (input.get_action_released("test_released")) {
    released = true;
  }

  if (input.get_action_released("test_released_mod")) {
    released_mod = true;
  }

  EXPECT_TRUE(released);
  EXPECT_TRUE(released_callback);
  EXPECT_TRUE(released_mod);
  EXPECT_TRUE(released_callback_mod);
}

TEST_F(InputManagerTest, KeyboardTestHeld) {
  bool held = false;
  bool held_callback = false;

  bool held_mod = false;
  bool held_callback_mod = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_held",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A)},
      .on_held_callback = [&held_callback](const ox::ActionContext&) { held_callback = true; }
    }
  );
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "test_held_mod",
      .primary_inputs = {ox::InputCode(ox::KeyCode::A, ox::ModCode::AnyControl)},
      .on_held_callback = [&held_callback_mod](const ox::ActionContext&) { held_callback_mod = true; }
    }
  );

  vk_keyboard->press(ox::KeyCode::A);
  vk_keyboard->press(ox::KeyCode::A, ox::ModCode::AnyControl);

  app->get_window().update(app->get_timestep());
  app->get_window().update(app->get_timestep());

  if (input.get_action_held("test_held")) {
    held = true;
  }

  if (input.get_action_held("test_held_mod")) {
    held_mod = true;
  }

  EXPECT_TRUE(held);
  EXPECT_TRUE(held_callback);
  EXPECT_TRUE(held_mod);
  EXPECT_TRUE(held_callback_mod);
}
