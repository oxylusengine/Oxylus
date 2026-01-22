#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/VirtualController.hpp"
#include "Core/VirtualKeyboard.hpp"
#include "Core/VirtualMouse.hpp"

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

    v_controller->connect();
  }

  void TearDown() override {
    app->stop();
    app.reset();
    v_keyboard.reset();
    v_mouse.reset();
    v_controller.reset();
  }

  std::unique_ptr<ox::App> app = nullptr;
  std::unique_ptr<ox::VirtualKeyboard> v_keyboard = std::make_unique<ox::VirtualKeyboard>();
  std::unique_ptr<ox::VirtualMouse> v_mouse = std::make_unique<ox::VirtualMouse>();
  std::unique_ptr<ox::VirtualController> v_controller = std::make_unique<ox::VirtualController>();
};

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

  v_keyboard->press(ox::KeyCode::A);
  v_keyboard->press(ox::KeyCode::A, ox::ModCode::AnyControl);

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

  v_keyboard->release(ox::KeyCode::A);
  v_keyboard->release(ox::KeyCode::A, ox::ModCode::AnyControl);

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

  v_keyboard->press(ox::KeyCode::A);
  v_keyboard->press(ox::KeyCode::A, ox::ModCode::AnyControl);

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

TEST_F(InputManagerTest, MouseTestPressed) {
  bool pressed = false;
  bool pressed_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "mouse_test_pressed",
      .primary_inputs = {ox::InputCode(ox::MouseCode::Left)},
      .on_pressed_callback = [&pressed_callback](const ox::ActionContext&) { pressed_callback = true; }
    }
  );

  v_mouse->press(ox::MouseCode::Left);

  app->get_window().update(app->get_timestep());

  if (input.get_action_pressed("mouse_test_pressed")) {
    pressed = true;
  }

  EXPECT_TRUE(pressed);
  EXPECT_TRUE(pressed_callback);
}

TEST_F(InputManagerTest, MouseTestReleased) {
  bool released = false;
  bool released_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "mouse_test_released",
      .primary_inputs = {ox::InputCode(ox::MouseCode::Left)},
      .on_released_callback = [&released_callback](const ox::ActionContext&) { released_callback = true; }
    }
  );

  v_mouse->release(ox::MouseCode::Left);

  app->get_window().update(app->get_timestep());

  if (input.get_action_released("mouse_test_released")) {
    released = true;
  }

  EXPECT_TRUE(released);
  EXPECT_TRUE(released_callback);
}

TEST_F(InputManagerTest, MouseTestHeld) {
  bool held = false;
  bool held_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "mouse_test_held",
      .primary_inputs = {ox::InputCode(ox::MouseCode::Left)},
      .on_held_callback = [&held_callback](const ox::ActionContext&) { held_callback = true; }
    }
  );

  v_mouse->press(ox::MouseCode::Left);

  app->get_window().update(app->get_timestep());
  app->get_window().update(app->get_timestep());

  if (input.get_action_held("mouse_test_held")) {
    held = true;
  }

  EXPECT_TRUE(held);
  EXPECT_TRUE(held_callback);
}

TEST_F(InputManagerTest, GamepadTestPressed) {
  bool pressed = false;
  bool pressed_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "gamepad_test_pressed",
      .primary_inputs = {ox::InputCode(ox::GamepadButtonCode::East)},
      .on_pressed_callback = [&pressed_callback](const ox::ActionContext&) { pressed_callback = true; }
    }
  );

  v_controller->simulate_button(ox::GamepadButtonCode::East, true);

  app->get_window().update(app->get_timestep());

  if (input.get_action_pressed("gamepad_test_pressed", v_controller->get_instance_id())) {
    pressed = true;
  }

  EXPECT_TRUE(pressed);
  EXPECT_TRUE(pressed_callback);
}

TEST_F(InputManagerTest, GamepadTestReleased) {
  bool released = false;
  bool released_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "gamepad_test_released",
      .primary_inputs = {ox::InputCode(ox::GamepadButtonCode::East)},
      .on_released_callback = [&released_callback](const ox::ActionContext&) { released_callback = true; }
    }
  );

  v_controller->simulate_button(ox::GamepadButtonCode::East, true);
  app->get_window().update(app->get_timestep());
  v_controller->simulate_button(ox::GamepadButtonCode::East, false);
  app->get_window().update(app->get_timestep());

  if (input.get_action_released("gamepad_test_released", v_controller->get_instance_id())) {
    released = true;
  }

  EXPECT_TRUE(released);
  EXPECT_TRUE(released_callback);
}

TEST_F(InputManagerTest, GamepadTestHeld) {
  bool held = false;
  bool held_callback = false;

  app->step();

  auto& input = app->mod<ox::Input>();
  std::ignore = input.bind_action(
    ox::ActionBinding{
      .action_id = "gamepad_test_held",
      .primary_inputs = {ox::InputCode(ox::GamepadButtonCode::East)},
      .on_held_callback = [&held_callback](const ox::ActionContext&) { held_callback = true; }
    }
  );

  v_controller->simulate_button(ox::GamepadButtonCode::East, true);
  app->get_window().update(app->get_timestep());
  app->get_window().update(app->get_timestep());

  if (input.get_action_held("gamepad_test_held", v_controller->get_instance_id())) {
    held = true;
  }

  EXPECT_TRUE(held);
  EXPECT_TRUE(held_callback);
}
