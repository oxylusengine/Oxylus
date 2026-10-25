#include "Scripting/ManagedSceneScript.hpp"

#include <fmt/format.h>
#include <vuk/Types.hpp>

#include "DotNetHosts.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/DotNetScripting.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto to_string_view(std::string_view text) -> OxStringView {
  return OxStringView{.data = text.data(), .length = text.size()};
}

// attaches its scene's world and forwards every scene hook to the managed side, which runs the scene scripts and the
// behaviours of that world from there
class ManagedSceneRuntime final : public SceneScript {
public:
  ManagedSceneRuntime(DotNetScripting& module, Scene& scene) : module_(module), world_(scene.world.c_ptr()) {}

  auto display_name() const -> std::string override { return "C# runtime"; }

  auto on_add(Scene* scene) -> void override {
    ZoneScoped;

    set_scene(world_, scene);
    if (auto result = module_.attach_world(world_, &scene->component_db); !result) {
      OX_LOG_ERROR("{}", result.error());
    }

    // a behaviour exists while its entity has a ScriptComponent and the scene runs, start_world creates the ones that
    // are already there
    observer_ = scene->world.observer<const ScriptComponent>()
                  .event(flecs::OnSet)
                  .event(flecs::OnRemove)
                  .each([this](flecs::iter& it, usize i, const ScriptComponent& component) {
                    const auto* attached = module_.find_world(world_);
                    if (!attached || !attached->is_running || !module_.is_loaded()) {
                      return;
                    }

                    const auto entity = it.entity(i).id();
                    if (it.event() == flecs::OnRemove) {
                      module_.managed_api.behaviour_remove(world_, entity);
                      return;
                    }

                    const auto status = module_.managed_api
                                          .behaviour_add(world_, entity, to_string_view(component.type_name));
                    if (status != OxStatus::Ok) {
                      OX_LOG_ERROR("Couldn't create C# behaviour {}: {}", component.type_name, status_message(status));
                    }
                  });
  }

  auto on_remove(Scene*) -> void override {
    ZoneScoped;

    observer_.destruct();
    module_.detach_world(world_);
    set_scene(world_, nullptr);
  }

  auto on_scene_start(Scene*) -> void override { module_.start_world(world_); }
  auto on_scene_stop(Scene*) -> void override { module_.stop_world(world_); }

  auto on_scene_update(Scene*, f32 delta_time) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_update(world_, delta_time);
    }
  }

  auto on_scene_fixed_update(Scene*, f32 delta_time) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_fixed_update(world_, delta_time);
    }
  }

  auto on_scene_render(Scene*, vuk::Extent3D extent) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_render(world_, extent.width, extent.height);
    }
  }

  auto on_contacts(Scene*, std::span<const PhysicsContact> contacts) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_contacts(world_, contacts.data(), static_cast<i32>(contacts.size()));
    }
  }

  auto on_body_activated(Scene*, const JPH::BodyID&, u64 body_user_data) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_body_activation(world_, body_user_data, true);
    }
  }

  auto on_body_deactivated(Scene*, const JPH::BodyID&, u64 body_user_data) -> void override {
    if (module_.is_loaded()) {
      module_.managed_api.world_body_activation(world_, body_user_data, false);
    }
  }

private:
  DotNetScripting& module_;
  ecs_world_t* world_ = nullptr;
  flecs::entity observer_ = {};
};

class DotNetSceneHost final : public SceneScriptHost {
public:
  explicit DotNetSceneHost(DotNetScripting& module) : module_(module) {}

  auto create_runtime(Scene& scene) -> std::unique_ptr<SceneScript> override {
    return std::make_unique<ManagedSceneRuntime>(module_, scene);
  }

  auto create_script(Scene& scene, std::string_view type_name)
    -> std::expected<std::unique_ptr<SceneScript>, std::string> override {
    // unchecked while nothing is loaded, the editor opens scenes before it has built the project's scripts
    if (module_.is_loaded()) {
      switch (module_.managed_api.find_script_type(to_string_view(type_name))) {
        case OxScriptKind::SceneScript: break;
        case OxScriptKind::Behaviour:
          return std::unexpected(fmt::format("{} is a behaviour, not a scene script", type_name));
        case OxScriptKind::None: return std::unexpected(fmt::format("There is no C# scene script {}", type_name));
      }
    }

    return std::make_unique<ManagedSceneScript>(module_, scene.world.c_ptr(), std::string(type_name));
  }

private:
  DotNetScripting& module_;
};

auto make_scene_host(DotNetScripting& module) -> std::unique_ptr<SceneScriptHost> {
  return std::make_unique<DotNetSceneHost>(module);
}

ManagedSceneScript::ManagedSceneScript(DotNetScripting& module, ecs_world_t* world, std::string type_name_)
    : type_name(std::move(type_name_)),
      module_(module),
      world_(world) {}

auto ManagedSceneScript::display_name() const -> std::string { return type_name; }

auto ManagedSceneScript::on_add(Scene*) -> void { module_.add_scene_script(world_, *this); }

auto ManagedSceneScript::on_remove(Scene*) -> void { module_.remove_scene_script(world_, *this); }
} // namespace ox
