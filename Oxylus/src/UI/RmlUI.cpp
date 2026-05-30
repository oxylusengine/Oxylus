#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include "Asset/EmbedAsset.hpp"
#include "Core/App.hpp"

namespace ox {
auto RmlUI::init() -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::SetSystemInterface(&this->rml_system);
  Rml::SetRenderInterface(&this->rml_renderer);

  if (!Rml::Initialise()) {
    return std::unexpected("Failed to initalize RmlUI!");
  }

  auto window_size = App::get_window().get_real_size();

  auto main_context = Rml::CreateContext("main", Rml::Vector2i(window_size.x, window_size.y));
  if (!main_context) {
    return std::unexpected("Failed to create the main context of RmlUI!");
  }

  this->contexts.emplace_back(main_context);

  Rml::Debugger::Initialise(main_context);

  auto& vk_context = App::get_vkcontext();
  auto& runtime = *vk_context.runtime;
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
  vk_context.create_pipelines(
    {.root_directory = shaders_dir},
    {
      {.path = "passes/rmlui.slang", .module_name = "rmlui", .entry_points = {"vs_main", "fs_main"}},
    }
  );

  return {};
}

auto RmlUI::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::Shutdown();

  return {};
}

auto RmlUI::update(const Timestep& timestep) -> void {
  ZoneScoped;

  for (const auto& ctx : this->contexts) {
    ctx->Update();
  }
}

auto RmlUI::render_contexts(this RmlUI& self) -> void {
  ZoneScoped;

  for (const auto& ctx : self.contexts) {
    ctx->Render();
  }
}

auto RmlUI::get_renderer(this RmlUI& self) -> RmlRenderer& {
  ZoneScoped;

  return self.rml_renderer;
}

auto RmlUI::add_context(this RmlUI& self, u32 width, u32 height) -> option<Rml::Context*> {
  ZoneScoped;

  auto ctx = Rml::CreateContext("main", Rml::Vector2i(width, height));
  if (!ctx) {
    return nullopt;
  }

  return ctx;
}

auto RmlUI::get_contexts(this RmlUI& self) -> std::span<Rml::Context*> {
  ZoneScoped;
  return self.contexts;
}

auto RmlUI::get_main_context(this const RmlUI& self) -> Rml::Context* {
  ZoneScoped;

  return self.contexts.empty() ? nullptr : self.contexts.front();
}
} // namespace ox
