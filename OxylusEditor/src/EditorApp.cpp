#include "Core/DefaultModules.hpp"
#include "Core/EmbeddedLogo.hpp"
#include "Core/EntryPoint.hpp"
#include "Core/Enum.hpp"
#include "Editor.hpp"

namespace ox {
App* create_application(const AppCommandLineArgs& args) {
  std::string name = "Oxylus Engine - Editor";
#ifdef OX_RELEASE
  name = "Oxylus Engine - Editor - Release";
#endif
#ifdef OX_DEBUG
  name = "Oxylus Engine - Editor - Debug";
#endif
#ifdef OX_DISTRIBUTION
  name = "Oxylus Engine - Editor - Dist";
#endif

  const auto app = new App();
  app->with_name(name)
    .with_args(args)
    .with_working_directory(std::filesystem::current_path().string())
    .with_window(
      WindowInfo{
        .title = name,
        .icon =
          WindowInfo::Icon{
            .loaded =
              WindowInfo::Icon::Loaded{
                .data = engine_logo,
                .width = engine_logoWidth,
                .height = engine_logoHeight,
              }
          },
        .width = 1720,
        .height = 900,
        .flags = WindowFlag::Centered | WindowFlag::Resizable | WindowFlag::HighPixelDensity,
      }
    )
    .with(DefaultModules{})
    .with<Editor>();

  return app;
}
} // namespace ox
