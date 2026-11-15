#pragma once

#include <expected>
#include <string>

#include "Core/Types.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class Editor;

enum class PlayStart : u8 { Started, StartedWithoutDebugger, WaitingForDebugger };

// the viewport shows the scene until the next frame, a playing scene is stopped instead
auto close_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string>;
// the play scene appears a frame or two later, find it with SceneManager::find_play_scene. With
// `can_wait_for_debugger` the scene does not start while the editor waits for a debugger to attach, the caller
// starts it once one has
auto play_scene(Editor& editor, EditorScene& scene, bool can_wait_for_debugger)
  -> std::expected<PlayStart, std::string>;
// takes the play scene or the edit scene it came from, returns the id of the play scene that was stopped
auto stop_scene(Editor& editor, EditorScene& scene) -> std::expected<SceneID, std::string>;
// the viewport takes focus next frame and the hierarchy follows it
auto focus_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string>;
} // namespace ox
