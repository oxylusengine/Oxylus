#include "Audio/AudioEngine.hpp"
#include "Core/EmbeddedLogo.hpp"
#include "Core/EntryPoint.hpp"
#include "Core/EventSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "EditorLayer.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
class OxylusEditor : public App {
public:
  OxylusEditor(const AppSpec& spec) : App(spec) {}
};

App* create_application(const AppCommandLineArgs& args) {
  AppSpec spec;
#ifdef OX_RELEASE
  spec.name = "Oxylus Engine - Editor - Release";
#endif
#ifdef OX_DEBUG
  spec.name = "Oxylus Engine - Editor - Debug";
#endif
#ifdef OX_DISTRIBUTION
  spec.name = "Oxylus Engine - Editor - Dist";
#endif
  spec.working_directory = std::filesystem::current_path().string();
  spec.command_line_args = args;
  const WindowInfo::Icon icon = {
    .loaded = WindowInfo::Icon::Loaded{.data = engine_logo, .width = engine_logoWidth, .height = engine_logoHeight}
  };
  spec.window_info = {
    .title = spec.name,
    .icon = icon,
    .width = 1720,
    .height = 900,
#if 0
      .flags = WindowFlag::Centered,
#else
    .flags = WindowFlag::Centered | WindowFlag::Resizable,
#endif
  };

  const auto app = new OxylusEditor(spec);
  app->with<AssetManager>()
    .with<AudioEngine>()
    .with<LuaManager>()
    .with<Physics>()
    .with<Input>()
    .with<NetworkManager>()
    .push_imgui_layer()
    .push_layer(std::make_unique<EditorLayer>());

  return app;
}
} // namespace ox
