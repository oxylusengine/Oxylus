#include "Core/App.hpp"
#include "Core/DefaultModules.hpp"
#include "Core/EmbeddedLogo.hpp"
#include "Core/Enum.hpp"
#include "Editor.hpp"

int main(int argc, char** argv) {
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

  auto app = ox::App(argc, argv);
  app.with_name(name)
    .with_window(
      ox::WindowInfo{
        .title = name,
        .icon =
          ox::WindowInfo::Icon{
            .loaded =
              ox::WindowInfo::Icon::Loaded{
                .data = engine_logo,
                .width = engine_logoWidth,
                .height = engine_logoHeight,
              }
          },
        .width = 1720,
        .height = 900,
        .flags = ox::WindowFlag::Centered | ox::WindowFlag::Resizable | ox::WindowFlag::HighPixelDensity,
      }
    )
    .with(ox::DefaultModules{})
    .with<ox::EditorConfig>()
    .with<ox::Editor>()
    .run();

  return 0;
}
