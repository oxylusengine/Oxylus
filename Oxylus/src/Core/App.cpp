#include "Core/App.hpp"

#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/EventSystem.hpp"
#include "Core/FileSystem.hpp"
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

App::App() {
  ZoneScoped;
  if (instance_) {
    OX_LOG_ERROR("Application already exists!");
    return;
  }

  instance_ = this;
}

App::~App() { is_running = false; }

void App::set_instance(App* instance) { instance_ = instance; }

auto App::with_name(this App& self, std::string name) -> App& {
  self.name = name;
  return self;
}

auto App::with_args(this App& self, AppCommandLineArgs args) -> App& {
  self.command_line_args = args;
  return self;
}

auto App::with_window(this App& self, WindowInfo window_info) -> App& {
  self.window_info = window_info;
  return self;
}

auto App::with_working_directory(this App& self, std::string dir) -> App& {
  self.working_directory = dir;
  return self;
}

auto App::with_assets_directory(this App& self, std::string dir) -> App& {
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

auto App::get_window(this const App& self) -> const Window& {
  OX_ASSERT(self.window.has_value());
  return self.window.value();
}

auto App::get_swapchain_extent(this const App& self) -> glm::vec2 {
  return self.swapchain_extent; //
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
    self.working_directory = std::filesystem::current_path().string();
  else
    std::filesystem::current_path(self.working_directory);

  self.vfs.mount_dir(VFS::APP_DIR, fs::absolute(self.assets_path));

  if (self.window_info.has_value()) {
    self.window = Window::create(*self.window_info);
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

  WindowCallbacks window_callbacks = {};
  window_callbacks.user_data = &self;
  window_callbacks.on_resize = [](void* user_data, const glm::uvec2 size) {
    const auto app = static_cast<App*>(user_data);
    app->vk_context->handle_resize(size.x, size.y);
  };
  window_callbacks.on_close = [](void* user_data) {
    const auto app = static_cast<App*>(user_data);
    app->is_running = false;
  };
  window_callbacks.on_mouse_pos = [](void* user_data, const glm::vec2 position, glm::vec2 relative) {
    auto* app = static_cast<App*>(user_data);
    app->mod<ImGuiRenderer>().on_mouse_pos(position);

    auto& input_system = app->mod<Input>();
    input_system.input_data.mouse_offset_x = input_system.input_data.mouse_pos.x - position.x;
    input_system.input_data.mouse_offset_y = input_system.input_data.mouse_pos.y - position.y;
    input_system.input_data.mouse_pos = position;
    input_system.input_data.mouse_pos_rel = relative;
    input_system.input_data.mouse_moved = true;
  };
  window_callbacks.on_mouse_button = [](void* user_data, const u8 button, const bool down) {
    auto* app = static_cast<App*>(user_data);
    auto& imgui_renderer = app->mod<ImGuiRenderer>();
    imgui_renderer.on_mouse_button(button, down);

    auto& input_system = app->mod<Input>();
    const auto ox_button = Input::to_mouse_code(button);
    if (down) {
      input_system.set_mouse_clicked(ox_button, true);
      input_system.set_mouse_released(ox_button, false);
      input_system.set_mouse_held(ox_button, true);
    } else {
      input_system.set_mouse_clicked(ox_button, false);
      input_system.set_mouse_released(ox_button, true);
      input_system.set_mouse_held(ox_button, false);
    }
  };
  window_callbacks.on_mouse_scroll = [](void* user_data, const glm::vec2 offset) {
    const auto* app = static_cast<App*>(user_data);
    auto& imgui_renderer = app->mod<ImGuiRenderer>();
    imgui_renderer.on_mouse_scroll(offset);

    auto& input_system = app->mod<Input>();
    input_system.input_data.scroll_offset_y = offset.y;
  };
  window_callbacks.on_key =
    [](void* user_data, const u32 key_code, const u32 scan_code, const u16 mods, const bool down, const bool repeat) {
      const auto* app = static_cast<App*>(user_data);
      auto& imgui_renderer = app->mod<ImGuiRenderer>();
      imgui_renderer.on_key(key_code, scan_code, mods, down);

      auto& input_system = app->mod<Input>();
      const auto ox_key_code = Input::to_keycode(key_code, scan_code);
      if (down) {
        input_system.set_key_pressed(ox_key_code, !repeat);
        input_system.set_key_released(ox_key_code, false);
        input_system.set_key_held(ox_key_code, true);
      } else {
        input_system.set_key_pressed(ox_key_code, false);
        input_system.set_key_released(ox_key_code, true);
        input_system.set_key_held(ox_key_code, false);
      }
    };
  window_callbacks.on_text_input = [](void* user_data, const c8* text) {
    const auto* app = static_cast<App*>(user_data);
    auto& imgui_renderer = app->mod<ImGuiRenderer>();
    imgui_renderer.on_text_input(text);
  };

  auto& imgui_renderer = self.mod<ImGuiRenderer>();

  while (self.is_running) {
    const i32 frame_limit = RendererCVar::cvar_frame_limit.get();
    if (frame_limit > 0) {
      self.timestep.set_max_frame_time(1000.0 / static_cast<f64>(frame_limit));
    } else {
      self.timestep.reset_max_frame_time();
    }

    self.timestep.on_update();

    vuk::Value<vuk::ImageAttachment> swapchain_attachment = {};
    vuk::Format format = {};
    vuk::Extent3D extent = {};
    if (self.window.has_value()) {
      self.window->poll(window_callbacks);

      swapchain_attachment = self.vk_context->new_frame();
      swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);

      format = swapchain_attachment->format;
      extent = swapchain_attachment->extent;
      self.swapchain_extent = glm::vec2{extent.width, extent.height};

      imgui_renderer.begin_frame(self.timestep.get_seconds(), extent);
    }

    self.run_deferred_tasks();

    self.registry.update(self.timestep);
    self.registry.render(extent, format);

    if (self.window_info.has_value()) {
      swapchain_attachment = imgui_renderer.end_frame(*self.vk_context, std::move(swapchain_attachment));

      self.vk_context->end_frame(swapchain_attachment);

      App::mod<Input>().reset_pressed();
    }

    if (self.registry.has<AssetManager>())
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
    self.vk_context->destroy_context();
  }
}

auto App::should_stop(this App& self) -> void { //
  self.is_running = false;
}
} // namespace ox
