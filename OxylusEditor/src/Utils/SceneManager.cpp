#include "SceneManager.hpp"

#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
auto EditorScene::is_valid(this const EditorScene& self) -> bool {
  ZoneScoped;

  return self.scene != nullptr;
}

auto EditorScene::is_playing(this const EditorScene& self) -> bool {
  ZoneScoped;

  return self.scene_state == EditorScene::SceneState::Play;
}

auto EditorScene::get_scene(this const EditorScene& self) -> std::shared_ptr<Scene> {
  ZoneScoped;

  OX_ASSERT(self.is_valid());
  return self.scene;
}

auto EditorScene::get_id(this const EditorScene& self) -> SceneID {
  ZoneScoped;

  return self.id;
}

auto EditorScene::play(this const EditorScene& self) -> std::shared_ptr<EditorScene> {
  ZoneScoped;

  auto new_scene = std::make_shared<EditorScene>(Scene::copy(self.scene));
  new_scene->get_scene()->meshes_dirty = true;
  new_scene->get_scene()->runtime_start();
  new_scene->scene_state = SceneState::Play;

  self.scene->reset_renderer_instance();

  return new_scene;
}

auto EditorScene::stop(this EditorScene& self) -> void {
  ZoneScoped;

  self.scene_state = SceneState::Edit;

  auto& event_system = App::get_event_system();
  event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(self.id));

  self.get_scene()
    ->world //
    .query_builder()
    .with<TransformComponent>()
    .build()
    .each([](flecs::entity e) { e.modified<TransformComponent>(); });
}

auto SceneManager::reset() -> void {
  ZoneScoped;

  scenes.reset();
}

auto SceneManager::new_scene() -> SceneID {
  ZoneScoped;

  auto scene_id = scenes.create_slot(std::make_shared<EditorScene>());
  scenes.slot(scene_id)->get()->id = scene_id;
  return scene_id;
}

auto SceneManager::load_scene(const std::filesystem::path& path) -> std::optional<SceneID> {
  ZoneScoped;

  // if (loaded_scenes.contains(path)) {
  // return loaded_scenes.at(path);
  // }

  if (!std::filesystem::exists(path)) {
    OX_LOG_WARN("Could not find scene: {0}", path.filename());
    return nullopt;
  }
  if (path.extension() != ".oxscene") {
    if (!std::filesystem::is_directory(path))
      OX_LOG_WARN("Could not load {0} - not a scene file", path.filename());
    return nullopt;
  }

  auto& job_man = App::get_job_manager();
  job_man.wait();

  auto scene_id = new_scene();
  auto editor_scene = *scenes.slot(scene_id);

  if (editor_scene->scene->load_from_file(path)) {
    loaded_scenes.emplace(path, scene_id);
    return scene_id;
  }

  scenes.destroy_slot(scene_id);

  return nullopt;
}

auto SceneManager::load_default_scene(SceneID scene_id) -> void {
  ZoneScoped;

  auto editor_scene_p = scenes.slot(scene_id);
  OX_CHECK_NULL(editor_scene_p);
  auto editor_scene = *editor_scene_p;

  const auto sun = editor_scene->scene->create_entity("sun", true);
  sun.add<TransformComponent>();
  sun.get_mut<TransformComponent>().rotation = glm::vec3{glm::radians(90.f), glm::radians(45.f), 0.f};
  sun.set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 10.f})
    .add<AtmosphereComponent>();
  const auto camera = editor_scene->scene->create_entity("camera", true);
  camera.add<CameraComponent>();
}

auto SceneManager::get_scene(SceneID scene_id) -> std::shared_ptr<EditorScene> {
  ZoneScoped;

  OX_ASSERT(scene_id != SceneID::Invalid);
  auto scene_ptr = scenes.slot(scene_id);
  OX_CHECK_NULL(scene_ptr);
  return *scene_ptr;
}

auto SceneManager::get_all_scenes(this SceneManager& self) -> std::span<std::shared_ptr<EditorScene>> {
  ZoneScoped;

  return self.scenes.slots_unsafe();
}
} // namespace ox
