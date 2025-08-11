#include "Core/App.hpp"

#include <ranges>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/FileSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Core/Layer.hpp"
#include "Core/VFS.hpp"
#include "Modules/ModuleRegistry.hpp"
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
auto engine_system_to_sv(EngineSystems type) -> std::string_view {
  switch (type) {
    case EngineSystems::JobManager    : return "JobManager";
    case EngineSystems::AssetManager  : return "AssetManager";
    case EngineSystems::VFS           : return "VFS";
    case EngineSystems::Random        : return "Random";
    case EngineSystems::AudioEngine   : return "AudioEngine";
    case EngineSystems::LuaManager    : return "LuaManager";
    case EngineSystems::ModuleRegistry: return "ModuleRegistry";
    case EngineSystems::RendererConfig: return "RendererConfig";
    case EngineSystems::Physics       : return "Physics";
    case EngineSystems::Input         : return "Input";
    case EngineSystems::Renderer      : return "Renderer";
    case EngineSystems::Count         : return "";
    default                           : return {};
  }
}

App* App::_instance = nullptr;

App::App(const AppSpec& spec) : app_spec(spec) {
  ZoneScoped;
  if (_instance) {
    OX_LOG_ERROR("Application already exists!");
    return;
  }

  _instance = this;

  if (app_spec.working_directory.empty())
    app_spec.working_directory = std::filesystem::current_path().string();
  else
    std::filesystem::current_path(app_spec.working_directory);

  if (!app_spec.headless) {
    window = Window::create(app_spec.window_info);
    vk_context = std::make_unique<VkContext>();

    const bool enable_validation = app_spec.command_line_args.contains("--vulkan-validation");
    vk_context->create_context(window, enable_validation);

    DebugRenderer::init();
  }

  register_system<JobManager>(EngineSystems::JobManager);
  register_system<AssetManager>(EngineSystems::AssetManager);
  register_system<VFS>(EngineSystems::VFS);
  register_system<Random>(EngineSystems::Random);
  register_system<AudioEngine>(EngineSystems::AudioEngine);
  register_system<LuaManager>(EngineSystems::LuaManager);
  register_system<ModuleRegistry>(EngineSystems::ModuleRegistry);
  register_system<RendererConfig>(EngineSystems::RendererConfig);
  register_system<Physics>(EngineSystems::Physics);
  register_system<Input>(EngineSystems::Input);

  auto* vfs = get_system<VFS>(EngineSystems::VFS);
  vfs->mount_dir(VFS::APP_DIR, fs::absolute(app_spec.assets_path));

  if (!app_spec.headless) {
    register_system<Renderer>(EngineSystems::Renderer, vk_context.get());
  }

  for (const auto& [type, system] : system_registry) {
    Timer timer{};
    auto result = system->init();
    if (!result) {
      OX_LOG_ERROR("{} System failed to initialize: {}", engine_system_to_sv(type), result.error());
    } else {
      OX_LOG_INFO("{} System initialized. {}ms", engine_system_to_sv(type), timer.get_elapsed_ms());
    }
  }

  // Shortcut for commonly used Systems
  Input::set_instance();

  if (!app_spec.headless) {
    auto imgui = std::make_unique<ImGuiLayer>();
    imgui_layer = imgui.get();
    push_layer(std::move(imgui));
  }

  auto* job_man = get_system<JobManager>(EngineSystems::JobManager);
  job_man->wait();
}

App::~App() { close(); }

void App::set_instance(App* instance) {
  _instance = instance;
  get_system<Input>(EngineSystems::Input)->set_instance();
}

App& App::push_layer(std::unique_ptr<Layer>&& layer) {
  layer->on_attach();
  layer_stack.emplace_back(std::move(layer));

  return *this;
}

void App::run() {
  ZoneScoped;

  const auto input_sys = get_system<Input>(EngineSystems::Input);
  const auto asset_man = get_system<AssetManager>(EngineSystems::AssetManager);

  WindowCallbacks window_callbacks = {};
  window_callbacks.user_data = this;
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

    const auto input_system = get_system<Input>(EngineSystems::Input);
    input_system->input_data.mouse_offset_x = input_system->input_data.mouse_pos.x - position.x;
    input_system->input_data.mouse_offset_y = input_system->input_data.mouse_pos.y - position.y;
    input_system->input_data.mouse_pos = position;
    input_system->input_data.mouse_pos_rel = relative;
    input_system->input_data.mouse_moved = true;
  };
  window_callbacks.on_mouse_button = [](void* user_data, const u8 button, const bool down) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_mouse_button(button, down);

    const auto input_system = get_system<Input>(EngineSystems::Input);
    const auto ox_button = Input::to_mouse_code(button);
    if (down) {
      input_system->set_mouse_clicked(ox_button, true);
      input_system->set_mouse_held(ox_button, true);
    } else {
      input_system->set_mouse_clicked(ox_button, false);
      input_system->set_mouse_held(ox_button, false);
    }
  };
  window_callbacks.on_mouse_scroll = [](void* user_data, const glm::vec2 offset) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_mouse_scroll(offset);

    const auto input_system = get_system<Input>(EngineSystems::Input);
    input_system->input_data.scroll_offset_y = offset.y;
  };
  window_callbacks.on_key = [](void* user_data,
                               const SDL_Keycode key_code,
                               const SDL_Scancode scan_code,
                               const u16 mods,
                               const bool down,
                               const bool repeat) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_key(key_code, scan_code, mods, down);

    const auto input_system = get_system<Input>(EngineSystems::Input);
    const auto ox_key_code = Input::to_keycode(key_code, scan_code);
    if (down) {
      input_system->set_key_pressed(ox_key_code, !repeat);
      input_system->set_key_released(ox_key_code, false);
      input_system->set_key_held(ox_key_code, true);
    } else {
      input_system->set_key_pressed(ox_key_code, false);
      input_system->set_key_released(ox_key_code, true);
      input_system->set_key_held(ox_key_code, false);
    }
  };
  window_callbacks.on_text_input = [](void* user_data, const c8* text) {
    const auto* app = static_cast<App*>(user_data);
    app->imgui_layer->on_text_input(text);
  };

  while (is_running) {
    const i32 frame_limit = RendererCVar::cvar_frame_limit.get();
    if (frame_limit > 0) {
      timestep.set_max_frame_time(1000.0 / static_cast<f64>(frame_limit));
    } else {
      timestep.reset_max_frame_time();
    }

    timestep.on_update();

    vuk::Value<vuk::ImageAttachment> swapchain_attachment = {};
    vuk::Format format = {};
    vuk::Extent3D extent = {};
    if (!app_spec.headless) {
      window.poll(window_callbacks);

      swapchain_attachment = vk_context->new_frame();
      swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);

      format = swapchain_attachment->format;
      extent = swapchain_attachment->extent;
      this->swapchain_extent = glm::vec2{extent.width, extent.height};

      imgui_layer->begin_frame(timestep.get_seconds(), extent);
    }

    {
      ZoneNamedN(z, "LayerStackUpdate", true);
      for (const auto& layer : layer_stack) {
        layer->on_update(timestep);
        layer->on_render(extent, format);
      }
    }

    {
      ZoneNamedN(z, "EngineSystemUpdates", true);
      for (const auto& system : system_registry | std::views::values) {
        system->on_update();
        system->on_render(extent, format);
      }
    }

    if (!app_spec.headless) {
      swapchain_attachment = imgui_layer->end_frame(*vk_context, std::move(swapchain_attachment));

      vk_context->end_frame(swapchain_attachment);

      input_sys->reset_pressed();
    }

    asset_man->load_deferred_assets();

    FrameMark;
  }

  close();
}

void App::close() {
  ZoneScoped;

  is_running = false;
  {
    ZoneNamedN(z, "LayerStackOnDetach", true);
    for (const auto& layer : layer_stack) {
      layer->on_detach();
    }

    layer_stack.clear();
  }

  {
    ZoneNamedN(z, "SystemRegistryDeinit", true);
    for (auto& [type, system] : system_registry) {
      auto result = system->deinit();
      if (!result) {
        OX_LOG_ERROR("{} System failed to deinitalize: {}", engine_system_to_sv(type), result.error());
      } else {
        OX_LOG_INFO("{} System deinitialized.", engine_system_to_sv(type));
      }
      system.reset();
    }

    system_registry.clear();
  }

  if (!app_spec.headless) {
    DebugRenderer::release();

    window.destroy();
  }
}

glm::vec2 App::get_swapchain_extent() const { return this->swapchain_extent; }

bool App::asset_directory_exists() const { return std::filesystem::exists(app_spec.assets_path); }

AssetManager* App::get_asset_manager() { return _instance->get_system<AssetManager>(EngineSystems::AssetManager); }

VFS* App::get_vfs() { return _instance->get_system<VFS>(EngineSystems::VFS); }

JobManager* App::get_job_manager() { return _instance->get_system<JobManager>(EngineSystems::JobManager); }
} // namespace ox
