#pragma once

#include "Core/ESystem.hpp"
#include "Core/Layer.hpp"
#include "Core/VFS.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Timestep.hpp"

int main(int argc, char** argv);

namespace ox {
class AssetManager;
class JobManager;
class Layer;
class ImGuiLayer;
class VkContext;

struct AppCommandLineArgs {
  struct Arg {
    std::string arg_str;
    u32 arg_index;
  };

  std::vector<Arg> args = {};

  AppCommandLineArgs() = default;
  AppCommandLineArgs(const int argc, char** argv) {
    for (int i = 0; i < argc; i++)
      args.emplace_back(Arg{.arg_str = argv[i], .arg_index = (u32)i});
  }

  bool contains(const std::string_view arg) const {
    for (const auto& a : args) {
      if (a.arg_str == arg) {
        return true;
      }
    }
    return false;
  }

  option<Arg> get(const u32 index) const {
    try {
      return args.at(index);
    } catch ([[maybe_unused]]
             std::exception& exception) {
      return ox::nullopt;
    }
  }

  option<u32> get_index(const std::string_view arg) const {
    for (const auto& a : args) {
      if (a.arg_str == arg) {
        return a.arg_index;
      }
    }
    return ox::nullopt;
  }
};

struct AppSpec {
  std::string name = "Oxylus App";
  std::string working_directory = {};
  std::string assets_path = "Resources";
  bool headless = false;
  AppCommandLineArgs command_line_args = {};
  WindowInfo window_info = {};
};

enum class EngineSystems {
  JobManager = 0,
  AssetManager,
  VFS,
  Random,
  AudioEngine,
  LuaManager,
  ModuleRegistry,
  RendererConfig,
  Physics,
  Input,
  Renderer,

  Count,
};

using SystemRegistry = ankerl::unordered_dense::map<EngineSystems, std::shared_ptr<ESystem>>;
class App {
public:
  App(const AppSpec& spec);
  virtual ~App();

  static App* get() { return _instance; }
  static void set_instance(App* instance);

  void run();
  void close();

  App& push_layer(std::unique_ptr<Layer>&& layer);

  const AppSpec& get_specification() const { return app_spec; }
  const AppCommandLineArgs& get_command_line_args() const { return app_spec.command_line_args; }

  ImGuiLayer* get_imgui_layer() const { return imgui_layer; }

  const Window& get_window() const { return window; }
  static VkContext& get_vkcontext() { return *_instance->vk_context; }
  glm::vec2 get_swapchain_extent() const;

  static const Timestep& get_timestep() { return _instance->timestep; }

  bool asset_directory_exists() const;

  static SystemRegistry& get_system_registry() { return _instance->system_registry; }

  static AssetManager* get_asset_manager();
  static VFS* get_vfs();
  static JobManager* get_job_manager();

  template <typename T, typename... Args>
  void register_system(const EngineSystems type, Args&&... args) {
    if (system_registry.contains(type)) {
      OX_LOG_ERROR("Registering system more than once.");
      return;
    }

    std::shared_ptr<T> system = std::make_shared<T>(std::forward<Args>(args)...);
    system->app = this;
    system_registry.emplace(type, std::move(system));
  }

  void unregister_system(const EngineSystems type) {
    if (system_registry.contains(type)) {
      system_registry.erase(type);
    }
  }

  template <typename T>
  static T* get_system(const EngineSystems type) {
    if (_instance->system_registry.contains(type)) {
      return dynamic_cast<T*>(_instance->system_registry[type].get());
    }

    return nullptr;
  }

  static bool has_system(const EngineSystems type) { return _instance->system_registry.contains(type); }

private:
  static App* _instance;
  AppSpec app_spec = {};
  std::vector<std::unique_ptr<Layer>> layer_stack = {};
  ImGuiLayer* imgui_layer = nullptr;
  std::unique_ptr<VkContext> vk_context = nullptr;
  Window window = {};
  glm::vec2 swapchain_extent = {};

  SystemRegistry system_registry = {};

  Timestep timestep = {};

  bool is_running = true;
  float last_frame_time = 0.0f;

  friend int ::main(int argc, char** argv);
};

App* create_application(const AppCommandLineArgs& args);
} // namespace ox
