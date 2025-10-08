#include "Core/App.hpp"

#include <ranges>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/EventSystem.hpp"
#include "Core/FileSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Core/Layer.hpp"
#include "Core/VFS.hpp"
#include "Physics/Physics.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "Scripting/LuaManager.hpp"
#include "UI/ImGuiLayer.hpp"
#include "Utils/Profiler.hpp"
#include "Utils/Random.hpp"
#include "Utils/Timer.hpp"

namespace ox {
App* App::instance_ = nullptr;

App::App(const AppSpec& spec) : app_spec(spec) {
  ZoneScoped;
  if (instance_) {
    OX_LOG_ERROR("Application already exists!");
    return;
  }

  instance_ = this;

  this->init();
}

App::~App() { is_running = false; }

void App::set_instance(App* instance) { instance_ = instance; }

auto App::init(this App& self) -> void {
  ZoneScoped;

  if (self.app_spec.working_directory.empty())
    self.app_spec.working_directory = std::filesystem::current_path().string();
  else
    std::filesystem::current_path(self.app_spec.working_directory);

  self.vfs.mount_dir(VFS::APP_DIR, fs::absolute(self.app_spec.assets_path));

  if (!self.app_spec.headless) {
    self.window = Window::create(self.app_spec.window_info);
    self.vk_context = std::make_unique<VkContext>();

    const bool enable_validation = self.app_spec.command_line_args.contains("--vulkan-validation");
    self.vk_context->create_context(self.window, enable_validation);
  }

  // Internal modules
  self.with<EventSystem>().with<JobManager>();
  if (!self.app_spec.headless) {
    self.with<RendererConfig>().with<Renderer>(self.vk_context.get());
  }

  self.mod<JobManager>().wait();
}

auto App::push_imgui_layer(this App& self) -> App& {
  auto imgui = std::make_unique<ImGuiLayer>();
  self.imgui_layer = imgui.get();
  self.push_layer(std::move(imgui));

  return self;
}

App& App::push_layer(std::unique_ptr<Layer>&& layer) {
  layer_stack.emplace_back(std::move(layer));

  return *this;
}

void App::run(this App& self) {
  ZoneScoped;

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
                              const SDL_Keycode key_code,
                              const SDL_Scancode scan_code,
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
    if (!self.app_spec.headless) {
      self.window.poll(window_callbacks);

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

    if (!self.app_spec.headless) {
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

  if (!self.app_spec.headless) {
    self.window.destroy();
    self.vk_context->destroy_context();
  }
}

glm::vec2 App::get_swapchain_extent() const { return this->swapchain_extent; }

bool App::asset_directory_exists() const { return std::filesystem::exists(app_spec.assets_path); }
} // namespace ox
