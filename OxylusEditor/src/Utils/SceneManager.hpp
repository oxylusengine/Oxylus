#pragma once

#include "Scene/Scene.hpp"

namespace ox {
struct EditorScene {
  enum class SceneState { Edit = 0, Play = 1 };
  SceneState scene_state = SceneState::Edit;

  std::shared_ptr<Scene> active_scene = nullptr;
  std::shared_ptr<Scene> scene = nullptr;

  EditorScene() : scene(std::make_shared<Scene>()) {}

  auto get_scene(this const EditorScene& self) -> std::shared_ptr<Scene>;
  
  auto play(this EditorScene& self) -> void;
  auto stop(this EditorScene& self) -> void;
};

class SceneManager {
public:
  SceneManager() = default;

  auto reset() -> void;
 
  auto new_scene() -> SceneID;
  auto load_scene(const std::filesystem::path& path) -> std::optional<SceneID>;
  auto load_default_scene(SceneID scene_id) -> void;

  auto get_scene(SceneID scene_id) -> std::shared_ptr<EditorScene>;

  auto get_all_scenes(this SceneManager& self) -> std::span<std::shared_ptr<EditorScene>>;

private:
  SlotMap<std::shared_ptr<EditorScene>, SceneID> scenes = {};
};
} // namespace ox
