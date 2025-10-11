#include "Audio/AudioEngine.hpp"
#include "Core/EmbeddedLogo.hpp"
#include "Core/EntryPoint.hpp"
#include "Core/Enum.hpp"
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
  OxylusEditor() : App() {}
};

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

  const auto app = new OxylusEditor();
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
        .flags = WindowFlag::Centered | WindowFlag::Resizable,
      }
    )
    .with<AssetManager>()
    .with<AudioEngine>()
    .with<Physics>()
    .with<Input>()
    .with<NetworkManager>()
    .with<LuaManager>()
    .push_imgui_layer()
    .push_layer(std::make_unique<EditorLayer>());

  return app;
}
} // namespace ox
