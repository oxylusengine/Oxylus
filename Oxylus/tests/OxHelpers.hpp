#pragma once

#include "Core/App.hpp"
#include "Scene/Scene.hpp"

class TestApp : public ox::App {
public:
  TestApp() : ox::App() {}
};

inline auto create_test_app() -> std::unique_ptr<TestApp> {
  auto app = std::make_unique<TestApp>();
  app->with_name("OxylusTestApp");

  return std::move(app);
}

inline auto create_test_scene() -> std::unique_ptr<ox::Scene> { return std::make_unique<ox::Scene>("TestScene"); }

inline auto log_test_end() -> void { OX_LOG_INFO("============================"); }
