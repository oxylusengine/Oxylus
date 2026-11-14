#include "Actions/SceneActions.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
auto finish_save(SaveStatus& status, bool is_ok) -> void {
  if (!is_ok) {
    status.error = fmt::format("the scene file '{}' could not be written", status.path.string());
  }

  status.is_ok.store(is_ok, std::memory_order_relaxed);
  status.is_done.store(true, std::memory_order_release);
}

auto find_play_scene(Editor& editor, SceneID source) -> EditorScene* {
  ZoneScoped;

  EditorScene* found = nullptr;
  editor.scene_manager.for_each_scene([&](SceneID, EditorScene& scene) {
    if (scene.is_playing() && scene.source == source) {
      found = &scene;
    }
  });

  return found;
}

auto find_scene_by_path(Editor& editor, const std::filesystem::path& path) -> EditorScene* {
  ZoneScoped;

  EditorScene* found = nullptr;
  editor.scene_manager.for_each_scene([&](SceneID, EditorScene& scene) {
    std::error_code error;
    if (
      !scene.is_playing() && !scene.get_path().empty() && std::filesystem::equivalent(scene.get_path(), path, error)
    ) {
      found = &scene;
    }
  });

  return found;
}

auto save_scene(Editor&, EditorScene& scene, option<std::filesystem::path> path)
  -> std::expected<std::shared_ptr<SaveStatus>, std::string> {
  ZoneScoped;

  if (scene.is_playing()) {
    return std::unexpected("stop the scene before saving");
  }

  auto target = path.has_value() ? path.value() : scene.get_path();
  if (target.empty()) {
    return std::unexpected("the scene has no file yet, give a path");
  }

  if (target.extension() != ".oxscene") {
    return std::unexpected(fmt::format("'{}' must end in .oxscene", target.string()));
  }

  std::error_code error;
  const auto parent = target.parent_path();
  if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
    return std::unexpected(fmt::format("the folder '{}' does not exist", parent.string()));
  }

  auto status = std::make_shared<SaveStatus>();
  status->path = target;
  Editor::submit_scene_save(&scene, std::move(target), status);
  return status;
}

auto open_scene(Editor& editor, const std::filesystem::path& path) -> std::expected<EditorScene*, std::string> {
  ZoneScoped;

  const auto scene_id = editor.scene_manager.load_scene(path);
  if (!scene_id.has_value()) {
    return std::unexpected(fmt::format("could not load '{}', log_read has the reason", path.string()));
  }

  auto scene = editor.scene_manager.get_scene(scene_id.value());
  editor.main_viewport_panel.add_new_scene(scene);
  return scene.get();
}

auto new_scene(Editor& editor) -> EditorScene* {
  ZoneScoped;

  const auto new_scene_id = editor.scene_manager.new_scene();
  editor.scene_manager.load_default_scene(new_scene_id);
  auto scene = editor.scene_manager.get_scene(new_scene_id);

  editor.main_viewport_panel.add_new_scene(scene);
  return scene.get();
}

auto close_scene(Editor& editor, EditorScene& scene) -> std::expected<void, std::string> {
  ZoneScoped;

  if (scene.is_playing()) {
    const auto stopped = stop_scene(editor, scene);
    if (!stopped) {
      return std::unexpected(stopped.error());
    }

    return {};
  }

  if (find_play_scene(editor, scene.get_id()) != nullptr) {
    return std::unexpected("the scene is playing, stop it first");
  }

  auto* viewport = editor.main_viewport_panel.find_viewport(&scene);
  if (viewport == nullptr) {
    return std::unexpected("no viewport shows this scene");
  }

  viewport->visible = false;
  return {};
}

auto play_scene(Editor& editor, EditorScene& scene) -> std::expected<PlayRequest, std::string> {
  ZoneScoped;

  if (scene.is_playing()) {
    return std::unexpected("this is already a play scene");
  }

  if (find_play_scene(editor, scene.get_id()) != nullptr) {
    return std::unexpected("the scene is already playing");
  }

  auto is_debugger_skipped = false;
#ifdef OX_DOTNET
  if (!editor.csharp_scripts.prepare_for_play()) {
    return std::unexpected("the C# scripts do not compile, log_read has the errors");
  }

  is_debugger_skipped = editor.editor_cvar.cvar_wait_for_debugger.as_bool() &&
                        editor.csharp_scripts.can_attach_debugger();
#endif

  std::ignore = App::get_event_system().emit<Editor::ScenePlayEvent>(Editor::ScenePlayEvent(scene.get_id()));
  return PlayRequest{.source = scene.get_id(), .is_debugger_skipped = is_debugger_skipped};
}

auto stop_scene(Editor& editor, EditorScene& scene) -> std::expected<SceneID, std::string> {
  ZoneScoped;

  auto* play = scene.is_playing() ? &scene : find_play_scene(editor, scene.get_id());
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

auto raycast_scene(Scene& scene, glm::vec3 origin, glm::vec3 direction, f32 max_distance) -> option<RaycastHit> {
  ZoneScoped;

  auto* system = scene.get_physics_system();
  const auto length = glm::length(direction);
  if (system == nullptr || length < 1e-6f || max_distance <= 0.0f) {
    return nullopt;
  }

  const auto unit = direction / length;
  const JPH::RRayCast ray(math::to_jolt(origin), math::to_jolt(unit * max_distance));
  JPH::RayCastResult hit;
  if (!system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
    return nullopt;
  }

  const auto point = ray.GetPointOnRay(hit.mFraction);
  const JPH::BodyLockRead lock(system->GetBodyLockInterface(), hit.mBodyID);
  if (!lock.Succeeded()) {
    return nullopt;
  }

  const auto& body = lock.GetBody();
  const auto entity_id = body.GetUserData();
  const auto entity = entity_id != 0 && ecs_is_alive(scene.world, entity_id) ? flecs::entity(scene.world, entity_id)
                                                                             : flecs::entity::null();

  return RaycastHit{
    .entity = entity,
    .point = math::from_jolt(JPH::Vec3(point)),
    .normal = math::from_jolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point)),
    .distance = hit.mFraction * max_distance,
  };
}
} // namespace ox
