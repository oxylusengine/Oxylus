#include "Core/App.hpp"

#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/EventSystem.hpp"
#include "Core/FileSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Core/Layer.hpp"
#include "Core/VFS.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "UI/ImGuiLayer.hpp"
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

auto App::push_imgui_layer(this App& self) -> App& {
  auto imgui = std::make_unique<ImGuiLayer>();
  self.imgui_layer = imgui.get();
  self.push_layer(std::move(imgui));

  return self;
}

App& App::push_layer(this App& self, std::unique_ptr<Layer>&& layer) {
  self.layer_stack.emplace_back(std::move(layer));
  return self;
}

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

auto App::get_command_line_args(this const App& self) -> const AppCommandLineArgs& {
  return self.command_line_args; //
}

auto App::get_imgui_layer(this const App& self) -> ImGuiLayer* {
  OX_CHECK_NULL(self.imgui_layer);
  return self.imgui_layer;
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

  // Internal modules
  self.with<EventSystem>().with<JobManager>();
  if (self.window.has_value()) {
    self.with<RendererConfig>().with<Renderer>(self.vk_context.get());
  }

  self.mod<JobManager>().wait();

  // Optional modules
  self.registry.init();

  self.mod<JobManager>().wait();

  for (auto& layer : self.layer_stack) {
    layer->on_attach();
  }

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
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_mouse_pos(position);

    auto& input_system = app->mod<Input>();
    input_system.input_data.mouse_offset_x = input_system.input_data.mouse_pos.x - position.x;
    input_system.input_data.mouse_offset_y = input_system.input_data.mouse_pos.y - position.y;
    input_system.input_data.mouse_pos = position;
    input_system.input_data.mouse_pos_rel = relative;
    input_system.input_data.mouse_moved = true;
  };
  window_callbacks.on_mouse_button = [](void* user_data, const u8 button, const bool down) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_mouse_button(button, down);

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
    app->imgui_layer->on_mouse_scroll(offset);

    auto& input_system = app->mod<Input>();
    input_system.input_data.scroll_offset_y = offset.y;
  };
  window_callbacks.on_key = [](
                              void* user_data,
                              const u32 key_code,
                              const u32 scan_code,
                              const u16 mods,
                              const bool down,
                              const bool repeat
                            ) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_key(key_code, scan_code, mods, down);

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
    app->imgui_layer->on_text_input(text);
  };

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

      self.imgui_layer->begin_frame(self.timestep.get_seconds(), extent);
    }

    {
      ZoneNamedN(z, "LayerStackUpdate", true);
      for (const auto& layer : self.layer_stack) {
        layer->on_update(self.timestep);
        layer->on_render(extent, format);
      }
    }

    self.registry.update(self.timestep.get_millis());
    self.registry.render(extent, format);

    if (self.window_info.has_value()) {
      swapchain_attachment = self.imgui_layer->end_frame(*self.vk_context, std::move(swapchain_attachment));

      self.vk_context->end_frame(swapchain_attachment);

      App::mod<Input>().reset_pressed();
    }

    self.mod<AssetManager>().load_deferred_assets();

    FrameMark;
  }

  self.close();
}

void App::close(this App& self) {
  ZoneScoped;

  self.is_running = false;
  {
    ZoneNamedN(z, "LayerStackOnDetach", true);
    for (const auto& layer : self.layer_stack) {
      layer->on_detach();
    }

    self.layer_stack.clear();
  }

  auto& job_man = self.mod<JobManager>();
  job_man.wait();

  self.registry.deinit();

  if (self.window.has_value()) {
    self.window->destroy();
    self.vk_context->destroy_context();
  }
}
} // namespace ox
