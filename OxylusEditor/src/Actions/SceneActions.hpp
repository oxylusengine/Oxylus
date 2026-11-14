#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <flecs.h>
#include <glm/vec3.hpp>
#include <memory>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class Editor;

struct SaveStatus {
  std::atomic<bool> is_done = false;
  std::atomic<bool> is_ok = false;
  // written before is_done is released
  std::string error = {};
  std::filesystem::path path = {};
};

auto finish_save(SaveStatus& status, bool is_ok) -> void;

struct PlayRequest {
  SceneID source = SceneID::Invalid;
  bool is_debugger_skipped = false;
};

struct RaycastHit {
  flecs::entity entity = {};
  glm::vec3 point = {};
  glm::vec3 normal = {};
  f32 distance = 0.0f;
};

// the status finishes once the file is written
auto save_scene(Editor& editor, EditorScene& scene, option<std::filesystem::path> path)
  -> std::expected<std::shared_ptr<SaveStatus>, std::string>;
auto open_scene(Editor& editor, const std::filesystem::path& path) -> std::expected<EditorScene*, std::string>;
auto new_scene(Editor& editor) -> EditorScene*;
// the viewport shows the scene until the next frame, a playing scene is stopped instead
auto close_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string>;
// the play scene appears a frame or two later, find it with find_play_scene
auto play_scene(Editor& editor, EditorScene& scene) -> std::expected<PlayRequest, std::string>;
// takes the play scene or the edit scene it came from, returns the id of the play scene that was stopped
auto stop_scene(Editor& editor, EditorScene& scene) -> std::expected<SceneID, std::string>;
// the viewport takes focus next frame and the hierarchy follows it
auto focus_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string>;

auto find_play_scene(Editor& editor, SceneID source) -> EditorScene*;
auto find_scene_by_path(Editor& editor, const std::filesystem::path& path) -> EditorScene*;

// physics bodies only
auto raycast_scene(Scene& scene, glm::vec3 origin, glm::vec3 direction, f32 max_distance) -> option<RaycastHit>;
} // namespace ox
