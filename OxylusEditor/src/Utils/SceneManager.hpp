#pragma once

#include "Scene/Scene.hpp"

namespace ox {
class SceneManager;

class EditorScene {
public:
  enum class SceneState { Edit, Play } scene_state = SceneState::Edit;

  EditorScene() : scene(std::make_shared<Scene>()) {}
  EditorScene(const std::shared_ptr<Scene>& s) : scene(s) {}

  auto is_valid(this const EditorScene& self) -> bool;
  auto is_playing(this const EditorScene& self) -> bool;

  auto get_scene(this const EditorScene& self) -> std::shared_ptr<Scene>;
  auto get_id(this const EditorScene& self) -> SceneID;

  auto play(this const EditorScene& self) -> std::shared_ptr<EditorScene>;
  auto stop(this EditorScene& self) -> void;

private:
  friend SceneManager;

  SceneID id = SceneID::Invalid;
  std::shared_ptr<Scene> scene = nullptr;
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

  std::unordered_map<std::filesystem::path, SceneID> loaded_scenes = {};
};
} // namespace ox
