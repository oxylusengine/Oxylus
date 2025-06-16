#include "Core/EmbeddedLogo.hpp"
#include "Core/EntryPoint.hpp"
#include "EditorLayer.hpp"

namespace ox {
class OxylusEditor : public App {
public:
  OxylusEditor(const AppSpec& spec) : App(spec) {}
};

App* create_application(const AppCommandLineArgs& args) {
  AppSpec spec;
#ifdef OX_RELEASE
  spec.name = "Oxylus Engine - Editor (Vulkan) - Release";
#endif
#ifdef OX_DEBUG
  spec.name = "Oxylus Engine - Editor (Vulkan) - Debug";
#endif
#ifdef OX_DISTRIBUTION
  spec.name = "Oxylus Engine - Editor (Vulkan) - Dist";
#endif
  spec.working_directory = std::filesystem::current_path().string();
  spec.command_line_args = args;
  const WindowInfo::Icon icon = {.data = EngineLogo, .data_length = EngineLogoLen};
  spec.window_info = {
      .title = spec.name,
      .icon = icon,
      .width = 1720,
      .height = 900,
#ifdef OX_PLATFORM_LINUX
      .flags = WindowFlag::Centered,
#else
      .flags = WindowFlag::Centered | WindowFlag::Resizable,
#endif
  };

  const auto app = new OxylusEditor(spec);
  app->push_layer(new EditorLayer());

  return app;
}
} // namespace ox
