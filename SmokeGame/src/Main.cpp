#include <SDL3/SDL_filesystem.h>
#include <filesystem>
#include <flecs.h>
#include <utility>
#include <vir/Module.hpp>
#include <vir/Program.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/DefaultModules.hpp"
#include "Core/VFS.hpp"
#include "Render/RenderContext.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/DotNetScripting.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr auto START_SCENE = "Scenes/Main.oxscene";
constexpr auto SMOKE_TEST_FRAMES = 120;

// the whole game: open the cooked start scene, then run and draw it every frame
struct SmokeGame {
  constexpr static auto MODULE_NAME = "SmokeGame";
  // DotNetScripting loads the scripts in its own init, so they are in before the scene names any
  using module_dependencies = std::tuple<AssetManager, Renderer, DotNetScripting>;

  // `--smoke-test` stops after a fixed number of frames and fails unless the C# scripts ran each one
  bool is_smoke_test = false;
  i32 frame = 0;
  std::unique_ptr<Scene> scene = nullptr;
  // what a script asked to load with Scene.Load, swapped in before the next frame runs
  option<UUID> requested_scene = nullopt;
  HandlerId scene_load_handler = {};
  // what the scene renders into, and the program that copies it onto the swapchain image, both made again whenever
  // the swapchain is replaced
  Texture target = {};
  std::unique_ptr<vir::Program> frame_program = nullptr;
  u64 frame_program_generation = 0;

  // stays a failure when init doesn't get far enough for deinit to run
  static inline auto exit_code = 1;

  explicit SmokeGame(bool is_smoke_test_) : is_smoke_test(is_smoke_test_) {}

  auto init(this SmokeGame& self) -> std::expected<void, std::string> {
    ZoneScoped;

    if (!App::mod<DotNetScripting>().is_loaded()) {
      return std::unexpected(std::string("The game's C# scripts didn't load"));
    }

    const auto scene_path = App::get_vfs().resolve_physical_dir(VFS::ASSETS_DIR, START_SCENE);
    const auto scene_uuid = App::mod<AssetManager>().find_asset(scene_path);
    if (!scene_uuid) {
      return std::unexpected(fmt::format("{} isn't among the cooked assets", START_SCENE));
    }

    self.scene = std::make_unique<Scene>("Main");
    if (!self.scene->load_from_asset(scene_uuid)) {
      return std::unexpected(fmt::format("Couldn't load {}", START_SCENE));
    }

    self.scene->runtime_start();
    self.scene_load_handler = App::get_event_system()
                                .subscribe<SceneLoadRequestEvent>([&self](const SceneLoadRequestEvent& event) {
                                  self.requested_scene = event.scene;
                                })
                                .value_or(HandlerId{});
    return {};
  }

  auto deinit(this SmokeGame& self) -> std::expected<void, std::string> {
    ZoneScoped;

    std::ignore = App::get_event_system().unsubscribe<SceneLoadRequestEvent>(self.scene_load_handler);

    exit_code = !self.is_smoke_test || self.check_smoke_test() ? 0 : 1;

    // before DotNetScripting deinits, the scene's scripts live in it
    self.scene.reset();
    self.frame_program.reset();
    self.target = {};
    return {};
  }

  auto update(this SmokeGame& self, const Timestep& timestep) -> void {
    ZoneScoped;

    if (self.requested_scene.has_value()) {
      self.load_requested_scene();
    }

    self.scene->runtime_update(timestep);

    self.render_frame();

    self.frame++;
    if (self.is_smoke_test && self.frame >= SMOKE_TEST_FRAMES) {
      App::get()->should_stop();
    }
  }

  auto render_frame(this SmokeGame& self) -> void {
    ZoneScoped;

    auto& render_context = App::get_rendercontext();
    const auto frame = render_context.new_frame();
    if (!frame) {
      // a minimized window has no swapchain to draw into
      if (frame.error() != VK_NOT_READY) {
        OX_LOG_ERROR("Couldn't start a frame ({})", std::to_underlying(frame.error()));
      }

      return;
    }

    if ((!self.frame_program || self.frame_program_generation != frame->generation) && !self.prepare_frame(*frame)) {
      return;
    }

    const auto size = glm::ivec2(static_cast<i32>(frame->extent.width), static_cast<i32>(frame->extent.height));
    if (const auto rendered = self.scene->render(self.target.attachment_info(), {0, 0}, size, size); !rendered) {
      OX_LOG_ERROR("Couldn't render the scene ({})", std::to_underlying(rendered.error()));
      return;
    }

    const auto ticket = render_context.execute(*self.frame_program);
    if (!ticket) {
      if (ticket.error() == VK_ERROR_OUT_OF_DATE_KHR) {
        self.frame_program.reset();
        render_context.handle_resize();
        return;
      }

      OX_LOG_ERROR("Couldn't present the frame ({})", std::to_underlying(ticket.error()));
      return;
    }

    // anything released this frame waits for everything the frame submitted
    render_context.seal_retirements(*ticket);
  }

  auto prepare_frame(this SmokeGame& self, const SwapchainInfo& swapchain) -> bool {
    ZoneScoped;

    auto& render_context = App::get_rendercontext();
    self.frame_program.reset();
    self.target = Texture::create({
      .format = swapchain.format,
      .extent = vir::Extent3D{swapchain.extent.width, swapchain.extent.height, 1},
      .usage = vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::Sampled |
               vir::ImageUsageFlags::TransferSrc | vir::ImageUsageFlags::TransferDst,
    });
    // the program reads it at rest, so it has to be there before the scene first draws into it
    const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
    if (const auto cleared = self.target.clear(black, vir::Access::FragmentSampled); !cleared) {
      OX_LOG_ERROR("Couldn't clear the scene's target ({})", std::to_underlying(cleared.error()));
      return false;
    }

    auto module = vir::Module{};
    auto image = render_context.acquire_next_image(module);
    image = module.blit(self.target.import(module, vir::Access::FragmentSampled), image);
    module.present(image);

    auto compiled = render_context.compile(std::move(module));
    if (!compiled) {
      OX_LOG_ERROR("Couldn't compile the frame ({})", std::to_underlying(compiled.error()));
      return false;
    }

    self.frame_program = std::make_unique<vir::Program>(std::move(*compiled));
    self.frame_program_generation = swapchain.generation;
    return true;
  }

  // the next scene loads before the current one goes, so the assets both use stay loaded through the swap
  auto load_requested_scene(this SmokeGame& self) -> void {
    ZoneScoped;

    const auto uuid = *self.requested_scene;
    self.requested_scene = nullopt;
    auto next = std::make_unique<Scene>("Main");
    if (!next->load_from_asset(uuid)) {
      OX_LOG_ERROR("Couldn't load scene {}, the current one keeps running", uuid.str());
      return;
    }

    self.scene = std::move(next);
    self.scene->runtime_start();
  }

  auto check_smoke_test(this const SmokeGame& self) -> bool {
    auto* world = self.scene->world.c_ptr();
    const auto stats = ecs_lookup(world, "Stats");
    const auto frames_component = ecs_lookup(world, "SmokeGame.Frames");
    const auto* frames = stats && frames_component ? static_cast<const i32*>(ecs_get_id(world, stats, frames_component))
                                                   : nullptr;
    if (!frames || *frames < SMOKE_TEST_FRAMES / 2) {
      OX_LOG_ERROR("Smoke test failed, the C# scene script counted {} frames", frames ? *frames : 0);
      return false;
    }

    OX_LOG_INFO("Smoke test passed, the C# scene script counted {} of {} frames", *frames, self.frame);
    return true;
  }
};
} // namespace ox

auto main(int argc, char** argv) -> int {
  const auto base_dir = std::filesystem::path(SDL_GetBasePath());

  auto app = ox::App(argc, argv);
  app.with_name("SmokeGame")
    .with_window(
      ox::WindowInfo{
        .title = "SmokeGame",
        .width = 1280,
        .height = 720,
        .flags = ox::WindowFlag::Centered | ox::WindowFlag::Resizable | ox::WindowFlag::HighPixelDensity,
      }
    )
    // next to the executable whatever directory the game was started from
    .with_assets_directory(base_dir / "Assets")
    .with_scripts("scripts/SmokeGame")
    .with(ox::DefaultModules{});

  const auto is_smoke_test = app.get_command_line_args().contains("--smoke-test");
  app.with<ox::SmokeGame>(is_smoke_test).run();

  return ox::SmokeGame::exit_code;
}
