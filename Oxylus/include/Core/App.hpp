#pragma once

#include "Core/AppCommandLineArgs.hpp"
#include "Core/EventSystem.hpp"
#include "Core/JobManager.hpp"
#include "Core/ModuleRegistry.hpp"
#include "Core/VFS.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
class ImGuiLayer;
class VkContext;

struct WindowResizeEvent {
  u32 width = 0;
  u32 height = 0;
};

class App {
public:
  App(int argc, char** argv);
  ~App();

  static App* get() { return instance_; }
  static void set_instance(App* instance);

  auto run(this App& self) -> void;
  auto stop(this App& self) -> void;
  auto should_stop(this App& self) -> void;

  auto with_name(this App& self, std::string name) -> App&;
  auto with_window(this App& self, WindowInfo window_info) -> App&;
  auto with_working_directory(this App& self, const std::filesystem::path& dir) -> App&;
  auto with_assets_directory(this App& self, const std::filesystem::path& dir) -> App&;

  template <typename F>
  void defer_to_next_frame(this App& self, F&& func) {
    std::function<void()> task = std::forward<F>(func);

    auto lock = std::unique_lock(self.mutex);
    self.pending_tasks.push_back(std::move(task));
  }

  template <typename T, typename... Args>
  auto with(this App& self, Args&&... args) -> App& {
    ZoneScoped;

    self.registry.add<T>(std::forward<Args>(args)...);

    return self;
  }

  template <typename... Modules>
  auto with(this App& self, std::tuple<Modules...>) -> App& {
    (..., [&] {
      self.with<Modules>();
    }());

    return self;
  }

  template <typename T>
  static auto mod() -> T& {
    return get()->registry.get<T>();
  }

  template <typename T>
  static auto has_mod() -> bool {
    return get()->registry.has<T>();
  }

  auto get_command_line_args(this const App& self) -> const AppCommandLineArgs&;

  static auto get_window() -> const Window&;
  static auto get_vkcontext() -> VkContext&;
  static auto get_timestep() -> const Timestep&;
  static auto get_vfs() -> VFS&;
  static auto get_job_manager() -> JobManager&;
  static auto get_event_system() -> EventSystem&;

private:
  static App* instance_;

  std::shared_mutex mutex;
  std::vector<std::function<void()>> pending_tasks;
  std::vector<std::function<void()>> processing_tasks;

  std::string name = "Oxylus App";
  std::filesystem::path assets_path = "Resources";
  std::filesystem::path working_directory = {};
  AppCommandLineArgs command_line_args = {};
  option<WindowInfo> window_info = nullopt;

  std::unique_ptr<VkContext> vk_context = nullptr;
  option<Window> window = nullopt;

  VFS vfs = {};
  JobManager job_manager = {};
  EventSystem event_system = {};
  ModuleRegistry registry = {};

  Timestep timestep = {};

  bool is_running = true;
  float last_frame_time = 0.0f;

  auto run_deferred_tasks(this App& self) -> void;
};

App* create_application(const AppCommandLineArgs& args);
} // namespace ox
