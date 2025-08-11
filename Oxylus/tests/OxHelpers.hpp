#pragma once

#include "Core/App.hpp"
#include "Scene/Scene.hpp"

class TestApp : public ox::App {
public:
  TestApp(const ox::AppSpec& spec) : ox::App(spec) {}
};

inline auto create_test_app() -> std::unique_ptr<TestApp> {
  ox::AppSpec app_spec = {};
  app_spec.name = "OxylusTestApp";
  app_spec.headless = true;
  return std::make_unique<TestApp>(app_spec);
}

inline auto create_test_scene() -> std::unique_ptr<ox::Scene> {
  return std::make_unique<ox::Scene>("TestScene", ox::Scene::no_renderer());
}
