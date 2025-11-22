#include "Core/App.hpp"

#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/EventSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Core/VFS.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
App* App::instance_ = nullptr;

App::App(int argc, char** argv) {
  ZoneScoped;

  if (instance_) {
    OX_LOG_ERROR("Application already exists!");
    return;
  }

  instance_ = this;

  Log::init(argc, argv);

  instance_->command_line_args = AppCommandLineArgs{argc, argv};
}

App::~App() { is_running = false; }

void App::set_instance(App* instance) { instance_ = instance; }

auto App::with_name(this App& self, std::string name) -> App& {
  self.name = name;
  return self;
}

auto App::with_window(this App& self, WindowInfo window_info) -> App& {
  self.window_info = window_info;
  return self;
}

auto App::with_working_directory(this App& self, const std::filesystem::path& dir) -> App& {
  self.working_directory = dir;
  return self;
}

auto App::with_assets_directory(this App& self, const std::filesystem::path& dir) -> App& {
  self.assets_path = dir;
  return self;
}

auto App::run_deferred_tasks(this App& self) -> void {
  {
    auto lock = std::unique_lock(self.mutex);
    std::swap(self.pending_tasks, self.processing_tasks);
  }

  for (auto& task : self.processing_tasks) {
    if (task) {
      task();
    }
  }

  self.processing_tasks.clear();
}

auto App::get_command_line_args(this const App& self) -> const AppCommandLineArgs& {
  return self.command_line_args; //
}

auto App::get_window() -> const Window& {
  OX_ASSERT(instance_->window.has_value());
  return instance_->window.value();
}

auto App::get_vkcontext() -> VkContext& {
  return *instance_->vk_context; //
}

auto App::get_timestep() -> const Timestep& {
  return instance_->timestep; //
}

auto App::get_vfs() -> VFS& {
  return instance_->vfs; //
}

auto App::get_job_manager() -> JobManager& {
  return instance_->job_manager; //
}

auto App::get_event_system() -> EventSystem& {
  return instance_->event_system; //
}

void App::run(this App& self) {
  ZoneScoped;

  if (self.command_line_args.contains("--verbose") || self.command_line_args.contains("-v")) {
    Log::set_verbose();
    OX_LOG_TRACE("Enabled verbose logging.");
  }

  if (self.working_directory.empty())
    self.working_directory = std::filesystem::current_path();
  else
    std::filesystem::current_path(self.working_directory);

  self.vfs.mount_dir(VFS::APP_DIR, std::filesystem::absolute(self.assets_path));

  if (self.window_info.has_value()) {
    self.window = Window::create(*self.window_info);
  }

  if (self.registry.has<Renderer>()) {
    self.vk_context = std::make_unique<VkContext>();

    const bool enable_validation = self.command_line_args.contains("--vulkan-validation");
    self.vk_context->create_context(*self.window, enable_validation);
  }

  auto job_manager_init_result = self.job_manager.init();
  if (job_manager_init_result.has_value())
    OX_LOG_INFO("Initalized JobManager.");
  else
    OX_LOG_ERROR("Failed to initalize JobManager: {}", job_manager_init_result.error());

  auto event_system_init_result = self.event_system.init();
  if (event_system_init_result.has_value())
    OX_LOG_INFO("Initalized EventSystem.");
  else
    OX_LOG_ERROR("Failed to initalize EventSystem: {}", event_system_init_result.error());

  self.registry.init();

  self.job_manager.wait();

  auto has_input_mod = self.registry.has<Input>();
  auto has_asset_manager_mod = self.registry.has<AssetManager>();

  while (self.is_running) {
    const i32 frame_limit = RendererCVar::cvar_frame_limit.get();
    if (frame_limit > 0) {
      self.timestep.set_max_frame_time(1000.0 / static_cast<f64>(frame_limit));
    } else {
      self.timestep.reset_max_frame_time();
    }

    self.timestep.on_update();

    self.run_deferred_tasks();

    if (self.window.has_value())
      self.window->update(self.timestep);

    self.registry.update(self.timestep);

    if (has_input_mod)
      self.mod<Input>().reset_pressed();

    if (has_asset_manager_mod)
      self.mod<AssetManager>().load_deferred_assets();

    FrameMark;
  }

  self.stop();
}

void App::stop(this App& self) {
  ZoneScoped;

  self.is_running = false;

  self.job_manager.wait();
  self.registry.deinit();
  self.job_manager.wait();

  auto job_manager_deinit_result = self.job_manager.deinit();
  if (job_manager_deinit_result.has_value())
    OX_LOG_INFO("Deinitalized JobManager.");
  else
    OX_LOG_ERROR("Failed to deinitalize JobManager: {}", job_manager_deinit_result.error());

  auto event_system_deinit_result = self.event_system.deinit();
  if (event_system_deinit_result.has_value())
    OX_LOG_INFO("Deinitalized EventSystem.");
  else
    OX_LOG_ERROR("Failed to deinitalize EventSystem: {}", event_system_deinit_result.error());

  if (self.window.has_value()) {
    self.window->destroy();
  }
  if (self.vk_context != nullptr) {
    self.vk_context->destroy_context();
  }
}

auto App::should_stop(this App& self) -> void { //
  self.is_running = false;
}
} // namespace ox
