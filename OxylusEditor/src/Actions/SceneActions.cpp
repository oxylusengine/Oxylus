#include "Actions/SceneActions.hpp"

#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
auto close_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string> {
  ZoneScoped;

  if (scene.is_playing()) {
    const auto stopped = stop_scene(editor, scene);
    if (!stopped) {
      return std::unexpected(stopped.error());
    }

    return {};
  }

  if (editor.scene_manager.find_play_scene(scene.get_id()) != nullptr) {
    return std::unexpected("the scene is playing, stop it first");
  }

  auto* viewport = editor.main_viewport_panel.find_viewport(&scene);
  if (viewport == nullptr) {
    return std::unexpected("no viewport shows this scene");
  }

  viewport->visible = false;
  return {};
}

auto play_scene(Editor& editor, EditorScene& scene, const bool can_wait_for_debugger)
  -> std::expected<PlayStart, std::string> {
  ZoneScoped;

  if (scene.is_playing()) {
    return std::unexpected("this is already a play scene");
  }

  if (editor.scene_manager.find_play_scene(scene.get_id()) != nullptr) {
    return std::unexpected("the scene is already playing");
  }

  auto start = PlayStart::Started;
#ifdef OX_DOTNET
  // waits for the build when the C# scripts changed
  if (!editor.csharp_scripts.prepare_for_play()) {
    return std::unexpected("the C# scripts do not compile, see the log");
  }

  const auto is_debugger_wanted = editor.editor_cvar.cvar_wait_for_debugger.as_bool() &&
                                  editor.csharp_scripts.can_attach_debugger();
  if (is_debugger_wanted && can_wait_for_debugger) {
    return PlayStart::WaitingForDebugger;
  }

  if (is_debugger_wanted) {
    start = PlayStart::StartedWithoutDebugger;
  }
#endif

  std::ignore = App::get_event_system().emit<Editor::ScenePlayEvent>(Editor::ScenePlayEvent(scene.get_id()));
  return start;
}

auto stop_scene(Editor& editor, EditorScene& scene) -> std::expected<SceneID, std::string> {
  ZoneScoped;

  auto* play = scene.is_playing() ? &scene : editor.scene_manager.find_play_scene(scene.get_id());
  if (play == nullptr) {
    return std::unexpected("the scene is not playing");
  }

  const auto play_id = play->get_id();
  std::ignore = App::get_event_system().emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(play_id));
  return play_id;
}

auto focus_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string> {
  ZoneScoped;

  auto* viewport = editor.main_viewport_panel.find_viewport(&scene);
  if (viewport == nullptr) {
    return std::unexpected("no viewport shows this scene");
  }

  viewport->is_focus_requested = true;
  return {};
}
} // namespace ox
