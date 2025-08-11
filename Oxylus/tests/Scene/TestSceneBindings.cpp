#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../OxHelpers.hpp"

class SceneTest : public ::testing::Test {
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

TEST_F(SceneTest, DidRun) {
  bool did_run = false;
  scene->runtime_start();
  did_run = scene->is_running();
  scene->runtime_stop();

  EXPECT_TRUE(did_run);
}
