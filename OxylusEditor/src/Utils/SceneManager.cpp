#include "SceneManager.hpp"

#include "Core/App.hpp"

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

auto EditorScene::set_path(this EditorScene& self, const std::filesystem::path& path) -> void {
  ZoneScoped;

  self.path = path;
}

auto EditorScene::get_path(this const EditorScene& self) -> const std::filesystem::path& {
  ZoneScoped;

  return self.path;
}

auto EditorScene::play(this EditorScene& self) -> void {
  ZoneScoped;

  self.get_scene()->meshes_dirty = true;
  self.get_scene()->runtime_start();
  self.scene_state = SceneState::Play;

  self.get_scene()->reset_renderer_instance();
}

auto EditorScene::stop(this EditorScene& self) -> void {
  ZoneScoped;

  self.scene_state = SceneState::Edit;

  self.get_scene()->reset_renderer_instance();
}

auto SceneManager::reset(this SceneManager& self) -> void {
  ZoneScoped;

  self.scenes.reset();
}

auto SceneManager::new_scene(this SceneManager& self) -> SceneID {
  ZoneScoped;

  auto scene_id = self.scenes.create_slot(std::make_shared<EditorScene>());
  self.scenes.slot(scene_id)->get()->id = scene_id;
  return scene_id;
}

auto SceneManager::new_play_scene(this SceneManager& self, SceneID from) -> SceneID {
  ZoneScoped;

  auto src_scene = self.get_scene(from);
  auto copy_scene = Scene::copy(src_scene->get_scene());

  auto copy_scene_id = self.scenes.create_slot(std::make_shared<EditorScene>(copy_scene));
  self.scenes.slot(copy_scene_id)->get()->id = copy_scene_id;

  self.get_scene(copy_scene_id)->play();

  return copy_scene_id;
}

auto SceneManager::remove_scene(this SceneManager& self, SceneID id) -> void {
  ZoneScoped;

  self.scenes.destroy_slot(id);
}

auto SceneManager::load_scene(this SceneManager& self, const std::filesystem::path& path) -> std::optional<SceneID> {
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

  auto scene_id = self.new_scene();
  auto editor_scene = *self.scenes.slot(scene_id);
  editor_scene->path = path;

  if (editor_scene->scene->load_from_file(path)) {
    self.loaded_scenes.emplace(path, scene_id);
    return scene_id;
  }

  self.scenes.destroy_slot(scene_id);

  return nullopt;
}

auto SceneManager::load_default_scene(this SceneManager& self, SceneID scene_id) -> void {
  ZoneScoped;

  auto editor_scene_p = self.scenes.slot(scene_id);
  OX_CHECK_NULL(editor_scene_p);
  auto editor_scene = *editor_scene_p;

  const auto sun = editor_scene->scene->create_entity("sun", true);
  sun.set<TransformComponent>({
    .rotation = glm::quat(glm::vec3(glm::radians(45.f), glm::radians(90.f), 0.f)),
  });
  sun.set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 10.f})
    .add<AtmosphereComponent>();
  const auto camera = editor_scene->scene->create_entity("camera", true);
  camera.set<CameraComponent>({});
}

auto SceneManager::get_scene(this const SceneManager& self, SceneID scene_id) -> std::shared_ptr<EditorScene> {
  ZoneScoped;

  OX_ASSERT(scene_id != SceneID::Invalid);
  auto scene_ptr = self.scenes.slotc(scene_id);
  OX_CHECK_NULL(scene_ptr);
  return *scene_ptr;
}
} // namespace ox
