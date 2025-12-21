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

  auto set_path(this EditorScene& self, const std::filesystem::path& path) -> void;
  auto get_path(this const EditorScene& self) -> const std::filesystem::path&;

  auto play(this EditorScene& self) -> void;
  auto stop(this EditorScene& self) -> void;

private:
  friend SceneManager;

  SceneID id = SceneID::Invalid;
  std::shared_ptr<Scene> scene = nullptr;
  std::filesystem::path path = {};
};

class SceneManager {
public:
  SceneManager() = default;

  auto reset(this SceneManager& self) -> void;

  auto new_scene(this SceneManager& self) -> SceneID;
  auto new_play_scene(this SceneManager& self, SceneID from) -> SceneID;
  auto remove_scene(this SceneManager& self, SceneID id) -> void;
  auto load_scene(this SceneManager& self, const std::filesystem::path& path) -> std::optional<SceneID>;
  auto load_default_scene(this SceneManager& self, SceneID scene_id) -> void;

  auto get_scene(this const SceneManager& self, SceneID scene_id) -> std::shared_ptr<EditorScene>;

private:
  SlotMap<std::shared_ptr<EditorScene>, SceneID> scenes = {};

  std::unordered_map<std::filesystem::path, SceneID> loaded_scenes = {};
};
} // namespace ox
