#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "Utils/Timestep.hpp"

constexpr auto SLEEP = std::chrono::milliseconds(20);

TEST(TimestepTest, ReportsTheWholeStepWithoutACap) {
  auto timestep = ox::Timestep();
  timestep.on_update();

  std::this_thread::sleep_for(SLEEP);
  timestep.on_update();
  EXPECT_GE(timestep.get_millis(), 10.0);
}

TEST(TimestepTest, CapsTheStepAndKeepsTheClockCurrent) {
  auto timestep = ox::Timestep();
  timestep.on_update();
  timestep.max_step_millis = 1.0;

  std::this_thread::sleep_for(SLEEP);
  timestep.on_update();
  EXPECT_DOUBLE_EQ(timestep.get_millis(), 1.0);

  // the time a stop took isn't owed to the next step either
  timestep.max_step_millis = ox::Timestep::NO_MAX_STEP;
  timestep.on_update();
  EXPECT_LT(timestep.get_millis(), 10.0);
}
