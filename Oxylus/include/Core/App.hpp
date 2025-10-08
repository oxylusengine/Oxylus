#pragma once

#include "Core/AppCommandLineArgs.hpp"
#include "Core/Layer.hpp"
#include "Core/ModuleRegistry.hpp"
#include "Core/VFS.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Timestep.hpp"

int main(int argc, char** argv);

namespace ox {
class Layer;
class ImGuiLayer;
class VkContext;

struct AppSpec {
  std::string name = "Oxylus App";
  std::string working_directory = {};
  std::string assets_path = "Resources";
  bool headless = false;
  AppCommandLineArgs command_line_args = {};
  WindowInfo window_info = {};
};

class App {
public:
  App(const AppSpec& spec);
  virtual ~App();

  static App* get() { return instance_; }
  static void set_instance(App* instance);

  void run(this App& self);
  void close(this App& self);

  auto push_imgui_layer(this App& self) -> App&;
  App& push_layer(std::unique_ptr<Layer>&& layer);

  template <typename T, typename... Args>
  auto with(this App& self, Args&&... args) -> App& {
    ZoneScoped;

    self.registry.add<T>(std::forward<Args>(args)...);

    return self;
  }

  template <typename T>
  static auto mod() -> T& {
    return get()->registry.get<T>();
  }

  const AppSpec& get_specification() const { return app_spec; }
  const AppCommandLineArgs& get_command_line_args() const { return app_spec.command_line_args; }

  ImGuiLayer* get_imgui_layer() const { return imgui_layer; }

  const Window& get_window() const { return window; }
  static VkContext& get_vkcontext() { return *instance_->vk_context; }
  glm::vec2 get_swapchain_extent() const;

  static const Timestep& get_timestep() { return instance_->timestep; }

  bool asset_directory_exists() const;

  static auto get_vfs() -> VFS& { return get()->vfs; }

private:
  static App* instance_;
  AppSpec app_spec = {};
  std::vector<std::unique_ptr<Layer>> layer_stack = {};
  ImGuiLayer* imgui_layer = nullptr;
  std::unique_ptr<VkContext> vk_context = nullptr;
  Window window = {};
  glm::vec2 swapchain_extent = {};

  VFS vfs = {};
  ModuleRegistry registry = {};

  Timestep timestep = {};

  bool is_running = true;
  float last_frame_time = 0.0f;

  friend int ::main(int argc, char** argv);

  auto init(this App& self) -> void;
};

App* create_application(const AppCommandLineArgs& args);
} // namespace ox
