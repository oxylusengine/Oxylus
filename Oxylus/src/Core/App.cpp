#include "Core/App.hpp"

#include <mutex>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/Texture.hpp"
#include "Core/EventSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Core/VFS.hpp"
#include "Core/Version.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "Render/Window.hpp"
#include "Scene/Scene.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/UIScale.hpp"
#include "Utils/Log.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
struct GameFrame {
  Texture target = {};
  std::unique_ptr<vir::Program> program = {};
  vir::ValueID scene_image = vir::ValueID::Invalid;
  ImGuiVariables imgui_variables = {};
  u64 generation = 0;
};

App* App::instance_ = nullptr;

App::App(int argc, char** argv) {
  ZoneScoped;

  if (instance_) {
    OX_LOG_ERROR("Application already exists!");
    return;
  }

  instance_ = this;

  static auto logging_initialized = std::once_flag{};
  std::call_once(logging_initialized, [argc, argv]() -> void { Log::init(argc, argv); });

  instance_->command_line_args = AppCommandLineArgs{argc, argv};
}

App::~App() {
  is_running = false;
  game_frame.reset();
  instance_ = nullptr;
}

void App::set_instance(App* instance) { instance_ = instance; }

auto App::init(this App& self) -> void {
  if (self.command_line_args.contains("--verbose") || self.command_line_args.contains("-v")) {
    Log::set_verbose();
    OX_LOG_TRACE("Enabled verbose logging.");
  }

  const auto& build_info = engine_build_info();
  OX_LOG_INFO("Oxylus {} ({})", build_info.full_version, build_info.build_mode);

  if (self.working_directory.empty())
    self.working_directory = std::filesystem::current_path();
  else
    std::filesystem::current_path(self.working_directory);

  // a shipped game's resources and content are the same folder; the editor remounts ASSETS_DIR per project
  self.vfs.mount_dir(VFS::APP_DIR, self.assets_path);
  self.vfs.mount_dir(VFS::ASSETS_DIR, self.assets_path);
  self.vfs.mount_dir(VFS::COOKED_DIR, self.assets_path / VFS::COOKED_SUBDIR);

  if (self.window_info.has_value()) {
    self.window = Window::create(*self.window_info);
  }

  if (self.registry.has<Renderer>()) {
    self.render_context = std::make_unique<RenderContext>();
    self.render_context->context_cvar.initialize_ui_scale(
      self.window->get_content_scale(),
      self.window->get_dpi_scale()
    );

    const bool enable_validation = self.command_line_args.contains("--vulkan-validation");
    self.render_context->create_context(*self.window, enable_validation);
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

  if (!self.registry.init()) {
    OX_LOG_ERROR("Stopping the app because a module failed to initialize.");
    self.is_running = false;
  }

  self.job_manager.wait();
}

auto App::step(this App& self) -> void {
  auto frame_limit = self.frame_limit;
  if (frame_limit <= 0 && self.render_context) {
    frame_limit = self.render_context->context_cvar.cvar_frame_limit.get();
  }
  if (frame_limit > 0) {
    self.timestep.set_max_frame_time(1000.0 / static_cast<f64>(frame_limit));
  } else {
    self.timestep.reset_max_frame_time();
  }

  self.timestep.on_update();

  self.run_deferred_tasks();

  if (self.window.has_value())
    self.window->update(self.timestep);

  if (!self.is_running)
    return;

  self.registry.update(self.timestep);

  if (self.registry.has<Input>())
    self.mod<Input>().reset_pressed();
}

auto App::render_scene(this App& self, Scene& scene, std::function<auto()->void> draw_ui)
  -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;

  if (!self.render_context || !self.window.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto& context = *self.render_context;
  const auto frame = context.new_frame();
  if (!frame) {
    return std::unexpected(frame.error());
  }

  if (!self.game_frame) {
    self.game_frame = std::make_unique<GameFrame>();
  }
  auto& game = *self.game_frame;
  if (!game.target || game.generation != frame->generation) {
    game.program.reset();
    game.target = Texture::create({
      .format = frame->format,
      .extent = {frame->extent.width, frame->extent.height, 1},
      .usage = vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::Sampled |
               vir::ImageUsageFlags::TransferSrc | vir::ImageUsageFlags::TransferDst,
    });
    if (!game.target) {
      return std::unexpected(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    const auto cleared = game.target.clear(
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f),
      vir::Access::FragmentSampled
    );
    if (!cleared) {
      game.target.destroy();
      return std::unexpected(cleared.error());
    }
    game.generation = frame->generation;
  }

  const auto size = glm::ivec2(frame->extent.width, frame->extent.height);
  const auto rendered = scene.render(game.target.attachment_info(), {}, size, size, true);
  if (!rendered) {
    return std::unexpected(rendered.error());
  }

  auto* imgui = self.registry.has<ImGuiRenderer>() ? &self.registry.get<ImGuiRenderer>() : nullptr;
  if (imgui) {
    imgui->begin_frame(self.timestep.get_seconds(), self.window->get_logical_size(), self.window->get_real_size());
    if (draw_ui) {
      draw_ui();
    }
    imgui->end_frame();
  }

  if (!game.program) {
    auto module = vir::Module{};
    game.scene_image = module.declare_image_var("game scene", frame->format, vir::Access::FragmentSampled);
    auto image = module.copy_image(game.scene_image, context.acquire_next_image(module));
    module.export_to(game.scene_image, vir::Access::FragmentSampled);
    if (imgui) {
      image = imgui->record(module, image, game.imgui_variables);
    }
    module.present(image);
    auto compiled = context.compile(std::move(module));
    if (!compiled) {
      return std::unexpected(compiled.error());
    }
    game.program = std::make_unique<vir::Program>(std::move(*compiled));
  }

  game.program->set(game.scene_image, game.target.attachment_info());
  if (imgui) {
    imgui->bind(*game.program, game.imgui_variables);
  }

  const auto ticket = context.execute(*game.program);
  if (!ticket) {
    if (ticket.error() == VK_ERROR_OUT_OF_DATE_KHR) {
      game.program.reset();
      context.handle_resize();
    }
    return std::unexpected(ticket.error());
  }
  context.seal_retirements(*ticket);
  return ticket;
}

auto App::run(this App& self) -> void {
  ZoneScoped;

  self.init();

  while (self.is_running) {
    self.step();
    FrameMark;
  }

  self.stop();
}

auto App::stop(this App& self) -> void {
  ZoneScoped;

  self.is_running = false;

  // Single point where the close is announced, so it fires no matter how the loop was left.
  std::ignore = self.event_system.emit<AppCloseEvent>(AppCloseEvent{});

  // Anything queued for "next frame" never got one. Run it while every module is still alive,
  // since those callbacks are how deferred destruction is expressed.
  self.run_deferred_tasks();

  self.job_manager.wait();

  // Modules release GPU resources in their deinit/destructor, so nothing may still be in flight.
  if (self.render_context != nullptr) {
    self.render_context->wait();
  }

  self.game_frame.reset();

  self.registry.deinit();
  self.job_manager.wait();
  self.run_deferred_tasks();

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

  // The surface outlives the swapchain, and both outlive the window they were created from.
  if (self.render_context != nullptr) {
    self.render_context->destroy_context();
  }
  if (self.window.has_value()) {
    self.window->destroy();
  }
}

auto App::should_stop(this App& self) -> void {
  self.is_running = false; //
}

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

auto App::with_workers(this App& self, const u32 count) -> App& {
  self.job_manager.set_thread_count(count);
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

auto App::get_rendercontext() -> RenderContext& {
  return *instance_->render_context; //
}

auto App::get_ui_scale() -> f32 {
  return normalize_ui_scale_multiplier(get_rendercontext().context_cvar.cvar_ui_scale.get());
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
} // namespace ox
