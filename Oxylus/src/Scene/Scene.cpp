#include "Scene/Scene.hpp"

#include <Core/FileSystem.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <simdjson.h>
#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Physics/Physics.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Physics/PhysicsMaterial.hpp"
#include "Render/Camera.hpp"
#include "Render/EasyRenderPipeline.hpp"
#include "Render/RenderPipeline.hpp"
#include "Render/RendererConfig.hpp"
#include "Scene/ECSModule/ComponentWrapper.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneEvents.hpp"
#include "Scripting/LuaManager.hpp"
#include "Utils/JsonHelpers.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
auto Scene::entity_to_json(JsonWriter& writer, flecs::entity e) -> void {
  ZoneScoped;

  writer.begin_obj();
  writer["name"] = e.name();

  std::vector<ECS::ComponentWrapper> components = {};
  writer["tags"].begin_array();
  e.each([&](flecs::id component_id) {
    if (!component_id.is_entity()) {
      return;
    }

    ECS::ComponentWrapper component(e, component_id);
    if (!component.is_component()) {
      writer << component.path;
    } else {
      components.emplace_back(e, component_id);
    }
  });
  writer.end_array();

  writer["components"].begin_array();
  for (auto& component : components) {
    writer.begin_obj();
    writer["name"] = component.path;
    component.for_each([&](usize&, std::string_view member_name, ECS::ComponentWrapper::Member& member) {
      auto& member_json = writer[member_name];
      std::visit(ox::match{
                     [](const auto&) {},
                     [&](bool* v) { member_json = *v; },
                     [&](u16* v) { member_json = *v; },
                     [&](f32* v) { member_json = *v; },
                     [&](i32* v) { member_json = *v; },
                     [&](u32* v) { member_json = *v; },
                     [&](i64* v) { member_json = *v; },
                     [&](u64* v) { member_json = *v; },
                     [&](glm::vec2* v) { member_json = *v; },
                     [&](glm::vec3* v) { member_json = *v; },
                     [&](glm::vec4* v) { member_json = *v; },
                     [&](glm::quat* v) { member_json = *v; },
                     [&](glm::mat4* v) { member_json = std::span(glm::value_ptr(*v), 16); },
                     [&](std::string* v) { member_json = *v; },
                     [&](UUID* v) { member_json = v->str().c_str(); },
                 },
                 member);
    });
    writer.end_obj();
  }
  writer.end_array();

  writer["children"].begin_array();
  e.children([&writer](flecs::entity c) { entity_to_json(writer, c); });
  writer.end_array();

  writer.end_obj();
}

auto Scene::json_to_entity(Scene& self,
                           flecs::entity root,
                           simdjson::ondemand::value& json,
                           std::vector<UUID>& requested_assets) -> std::pair<flecs::entity, bool> {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto& world = self.world;

  auto entity_name_json = json["name"];
  if (entity_name_json.error()) {
    OX_LOG_ERROR("Entities must have names!");
    return {{}, false};
  }

  auto e = self.create_entity(std::string(entity_name_json.get_string().value_unsafe()));
  if (root != flecs::entity::null())
    e.child_of(root);

  auto entity_tags_json = json["tags"];
  for (auto entity_tag : entity_tags_json.get_array()) {
    auto tag = world.component(stack.null_terminate(entity_tag.get_string().value_unsafe()).data());
    e.add(tag);
  }

  auto components_json = json["components"];
  for (auto component_json : components_json.get_array()) {
    auto component_name_json = component_json["name"];
    if (component_name_json.error()) {
      OX_LOG_ERROR("Entity '{}' has corrupt components JSON array.", e.name().c_str());
      return {{}, false};
    }

    const auto* component_name = stack.null_terminate_cstr(component_name_json.get_string().value_unsafe());
    auto component_id = world.lookup(component_name);
    if (!component_id) {
      OX_LOG_ERROR("Entity '{}' has invalid component named '{}'!", e.name().c_str(), component_name);
      return {{}, false};
    }

    if (!self.component_db.is_component_known(component_id)) {
      OX_LOG_WARN("Skipping unkown component {}:{}", component_name, (u64)component_id);
      continue;
    }

    e.add(component_id);
    ECS::ComponentWrapper component(e, component_id);
    component.for_each([&](usize&, std::string_view member_name, ECS::ComponentWrapper::Member& member) {
      auto member_json = component_json[member_name];
      if (member_json.error()) {
        // Default construct
        return;
      }

      std::visit(ox::match{
                     [](const auto&) {},
                     [&](bool* v) { *v = static_cast<bool>(member_json.get_bool().value_unsafe()); },
                     [&](u16* v) { *v = static_cast<u16>(member_json.get_uint64().value_unsafe()); },
                     [&](f32* v) { *v = static_cast<f32>(member_json.get_double().value_unsafe()); },
                     [&](i32* v) { *v = static_cast<i32>(member_json.get_int64().value_unsafe()); },
                     [&](u32* v) { *v = static_cast<u32>(member_json.get_uint64().value_unsafe()); },
                     [&](i64* v) { *v = member_json.get_int64().value_unsafe(); },
                     [&](u64* v) { *v = member_json.get_uint64().value_unsafe(); },
                     [&](glm::vec2* v) { json_to_vec(member_json.value_unsafe(), *v); },
                     [&](glm::vec3* v) { json_to_vec(member_json.value_unsafe(), *v); },
                     [&](glm::vec4* v) { json_to_vec(member_json.value_unsafe(), *v); },
                     [&](glm::quat* v) { json_to_quat(member_json.value_unsafe(), *v); },
                     // [&](glm::mat4 *v) {json_to_mat(member_json.value(), *v); },
                     [&](std::string* v) { *v = member_json.get_string().value_unsafe(); },
                     [&](UUID* v) {
                       *v = UUID::from_string(member_json.get_string().value_unsafe()).value();
                       requested_assets.push_back(*v);
                     },
                 },
                 member);
    });

    e.modified(component_id);
  }

  auto children_json = json["children"];
  for (auto children : children_json.get_array()) {
    if (children.error()) {
      continue;
    }

    if (!json_to_entity(self, e, children.value_unsafe(), requested_assets).second) {
      return {{}, false};
    }
  }

  return {e, true};
}

auto ComponentDB::import_module(this ComponentDB& self, flecs::entity module) -> void {
  ZoneScoped;

  self.imported_modules.emplace_back(module);
  module.children([&](flecs::id id) { self.components.push_back(id); });
}

auto ComponentDB::is_component_known(this ComponentDB& self, flecs::id component_id) -> bool {
  ZoneScoped;

  return std::ranges::any_of(self.components, [&](const auto& id) { return id == component_id; });
}

auto ComponentDB::get_components(this ComponentDB& self) -> std::span<flecs::id> { return self.components; }

Scene::Scene(const std::string& name, const std::shared_ptr<RenderPipeline>& render_pipeline) {
  this->init(name, render_pipeline);
}

Scene::Scene(const std::string& name) { init(name); }

Scene::~Scene() {
  const auto* lua_manager = App::get_system<LuaManager>(EngineSystems::LuaManager);
  lua_manager->get_state()->collect_gc();

  if (running)
    runtime_stop();

  _render_pipeline->deinit();
}

auto Scene::init(this Scene& self, const std::string& name, const std::shared_ptr<RenderPipeline>& render_pipeline)
    -> void {
  ZoneScoped;
  self.scene_name = name;

  self.component_db.import_module(self.world.import <Core>());

  // Renderer
  self._render_pipeline = render_pipeline;

  if (!self._render_pipeline) {
    self._render_pipeline = std::make_shared<EasyRenderPipeline>();
    self._render_pipeline->init(App::get_vkcontext());
  }

  self.physics_events = self.world.entity("ox_physics_events");

  self.world.observer<TransformComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .event(flecs::OnRemove)
      .without<MeshComponent>()
      .each([&self](flecs::iter& it, usize i, TransformComponent&) {
        auto entity = it.entity(i);
        if (it.event() == flecs::OnSet) {
          self.set_dirty(entity);
        } else if (it.event() == flecs::OnAdd) {
          self.add_transform(entity);
          self.set_dirty(entity);
        } else if (it.event() == flecs::OnRemove) {
          self.remove_transform(entity);
        }
      });

  self.world.observer<TransformComponent, MeshComponent>()
      .event(flecs::OnAdd)
      .event(flecs::OnSet)
      .event(flecs::OnRemove)
      .each([&self](flecs::iter& it, usize i, TransformComponent& tc, MeshComponent& mc) {
        auto entity = it.entity(i);
        const auto mesh_event = it.event_id() == self.world.component<MeshComponent>();
        if (it.event() == flecs::OnSet) {
          if (!self.entity_transforms_map.contains(entity))
            self.add_transform(entity);
          self.set_dirty(entity);

          if (mesh_event && mc.mesh_uuid)
            self.attach_mesh(entity, mc.mesh_uuid, mc.mesh_index);
        } else if (it.event() == flecs::OnAdd) {
          self.add_transform(entity);
          self.set_dirty(entity);
        } else if (it.event() == flecs::OnRemove) {
          if (mc.mesh_uuid)
            self.detach_mesh(entity, mc.mesh_uuid, mc.mesh_index);

          self.remove_transform(entity);
        }
      });

  self.world.observer<TransformComponent, SpriteComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .each([&self](flecs::iter& it, usize i, TransformComponent&, SpriteComponent& sprite) {
        auto entity = it.entity(i);
        // Set sprite rect
        if (auto id = self.get_entity_transform_id(entity)) {
          if (auto* transform = self.get_entity_transform(*id)) {
            sprite.rect = AABB(glm::vec3(-0.5, -0.5, -0.5), glm::vec3(0.5, 0.5, 0.5));
            sprite.rect = sprite.rect.get_transformed(transform->world);
          }
        }
      });

  self.world.observer<SpriteComponent>()
      .event(flecs::OnRemove) //
      .each([](flecs::iter& it, usize i, SpriteComponent& c) {
        auto* asset_man = App::get_asset_manager();
        if (auto* material_asset = asset_man->get_asset(c.material)) {
          asset_man->unload_asset(material_asset->uuid);
        }
      });

  self.world.observer<AudioListenerComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .each([](flecs::iter& it, usize i, AudioListenerComponent& c) {
        auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);
        audio_engine->set_listener_cone(c.listener_index, c.cone_inner_angle, c.cone_outer_angle, c.cone_outer_gain);
      });

  self.world.observer<AudioSourceComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .event(flecs::OnRemove)
      .each([](flecs::iter& it, usize i, AudioSourceComponent& c) {
        auto* asset_man = App::get_asset_manager();
        auto* audio_asset = asset_man->get_audio(c.audio_source);
        if (!audio_asset)
          return;

        if (it.event() == flecs::OnRemove) {
          asset_man->unload_asset(c.audio_source);
          return;
        }

        auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);
        audio_engine->set_source_volume(audio_asset->get_source(), c.volume);
        audio_engine->set_source_pitch(audio_asset->get_source(), c.pitch);
        audio_engine->set_source_looping(audio_asset->get_source(), c.looping);
        audio_engine->set_source_attenuation_model(audio_asset->get_source(),
                                                   static_cast<AudioEngine::AttenuationModelType>(c.attenuation_model));
        audio_engine->set_source_roll_off(audio_asset->get_source(), c.roll_off);
        audio_engine->set_source_min_gain(audio_asset->get_source(), c.min_gain);
        audio_engine->set_source_max_gain(audio_asset->get_source(), c.max_gain);
        audio_engine->set_source_min_distance(audio_asset->get_source(), c.min_distance);
        audio_engine->set_source_max_distance(audio_asset->get_source(), c.max_distance);
        audio_engine->set_source_cone(
            audio_asset->get_source(), c.cone_inner_angle, c.cone_outer_angle, c.cone_outer_gain);
      });

  self.world.observer<SpriteAnimationComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .each([](flecs::iter& it, usize i, SpriteAnimationComponent& c) { c.reset(); });

  self.world.observer<LuaScriptComponent>()
      .event(flecs::OnSet)
      .event(flecs::OnAdd)
      .event(flecs::OnRemove)
      .each([scene = &self](flecs::iter& it, usize i, LuaScriptComponent& c) {
        if (it.event() == flecs::OnAdd || it.event() == flecs::OnSet) {
          auto* asset_man = App::get_asset_manager();
          if (auto* script_asset = asset_man->get_script(c.script_uuid)) {
            script_asset->on_add(scene, it.entity(i));
          }
        } else if (it.event() == flecs::OnRemove) {
          auto* asset_man = App::get_asset_manager();
          if (auto* script_asset = asset_man->get_script(c.script_uuid)) {
            script_asset->on_remove(scene, it.entity(i));
          }
        }
      });

  // Systems run order:
  // -- PreUpdate  -> Main Systems
  // -- OnUpdate   -> Physics Systems
  // -- PostUpdate -> Renderer Systems

  // --- Main Systems ---

  self.world.system<const LuaScriptComponent>("lua_update")
      .kind(flecs::PreUpdate)
      .each([](flecs::iter& it, size_t i, const LuaScriptComponent& c) {
        auto* asset_man = App::get_asset_manager();
        if (auto* script = asset_man->get_script(c.script_uuid)) {
          script->on_scene_update(it.delta_time());
        }
      });

  self.world.system<const TransformComponent, AudioListenerComponent>("audio_listener_update")
      .kind(flecs::PreUpdate)
      .each([&self](const flecs::entity& e, const TransformComponent& tc, AudioListenerComponent& ac) {
        if (ac.active) {
          auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);
          const glm::mat4 inverted = glm::inverse(self.get_world_transform(e));
          const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
          audio_engine->set_listener_position(ac.listener_index, tc.position);
          audio_engine->set_listener_direction(ac.listener_index, -forward);
          audio_engine->set_listener_cone(
              ac.listener_index, ac.cone_inner_angle, ac.cone_outer_angle, ac.cone_outer_gain);
        }
      });

  self.world.system<const TransformComponent, AudioSourceComponent>("audio_source_update")
      .kind(flecs::PreUpdate)
      .each([](const flecs::entity& e, const TransformComponent& tc, const AudioSourceComponent& ac) {
        auto* asset_man = App::get_asset_manager();
        if (auto* audio = asset_man->get_audio(ac.audio_source)) {
          auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);
          audio_engine->set_source_attenuation_model(
              audio->get_source(), static_cast<AudioEngine::AttenuationModelType>(ac.attenuation_model));
          audio_engine->set_source_volume(audio->get_source(), ac.volume);
          audio_engine->set_source_pitch(audio->get_source(), ac.pitch);
          audio_engine->set_source_looping(audio->get_source(), ac.looping);
          audio_engine->set_source_spatialization(audio->get_source(), ac.looping);
          audio_engine->set_source_roll_off(audio->get_source(), ac.roll_off);
          audio_engine->set_source_min_gain(audio->get_source(), ac.min_gain);
          audio_engine->set_source_max_gain(audio->get_source(), ac.max_gain);
          audio_engine->set_source_min_distance(audio->get_source(), ac.min_distance);
          audio_engine->set_source_max_distance(audio->get_source(), ac.max_distance);
          audio_engine->set_source_cone(
              audio->get_source(), ac.cone_inner_angle, ac.cone_outer_angle, ac.cone_outer_gain);
          audio_engine->set_source_doppler_factor(audio->get_source(), ac.doppler_factor);
        }
      });

  // --- Physics Systems ---

  // TODOs(hatrickek):
  // Interpolation for rigibodies.

  const auto physics_tick_source = self.world.timer().interval(self.physics_interval);

  self.world
      .system("physics_step") //
      .kind(flecs::OnUpdate)
      .tick_source(physics_tick_source)
      .run([](flecs::iter& it) {
        auto* physics = App::get_system<Physics>(EngineSystems::Physics);
        physics->step(it.delta_time());
      });

  self.world.system<const LuaScriptComponent>("lua_fixed_update")
      .kind(flecs::OnUpdate)
      .tick_source(physics_tick_source)
      .each([](flecs::iter& it, size_t i, const LuaScriptComponent& c) {
        auto* asset_man = App::get_asset_manager();
        if (auto* script = asset_man->get_script(c.script_uuid)) {
          script->on_scene_fixed_update(it.delta_time());
        }
      });

  self.world.system<TransformComponent, RigidbodyComponent>("rigidbody_update")
      .kind(flecs::OnUpdate)
      .tick_source(physics_tick_source)
      .each([](flecs::entity e, TransformComponent& tc, RigidbodyComponent& rb) {
        if (!rb.runtime_body)
          return;

        auto* physics = App::get_system<Physics>(EngineSystems::Physics);
        const auto* body = static_cast<const JPH::Body*>(rb.runtime_body);
        const auto& body_interface = physics->get_physics_system()->GetBodyInterface();

        if (!body_interface.IsActive(body->GetID()))
          return;

        const JPH::Vec3 position = body->GetPosition();
        const JPH::Vec3 rotation = body->GetRotation().GetEulerAngles();

        rb.previous_translation = rb.translation;
        rb.previous_rotation = rb.rotation;
        rb.translation = {position.GetX(), position.GetY(), position.GetZ()};
        rb.rotation = glm::vec3(rotation.GetX(), rotation.GetY(), rotation.GetZ());
        tc.position = rb.translation;
        tc.rotation = glm::eulerAngles(rb.rotation);
      });

  self.world.system<TransformComponent, CharacterControllerComponent>("character_controller_update")
      .kind(flecs::OnUpdate)
      .tick_source(physics_tick_source)
      .each([](TransformComponent& tc, CharacterControllerComponent& ch) {
        auto* character = reinterpret_cast<JPH::Character*>(ch.character);
        character->PostSimulation(ch.collision_tolerance);
        const JPH::Vec3 position = character->GetPosition();
        const JPH::Vec3 rotation = character->GetRotation().GetEulerAngles();

        ch.previous_translation = ch.translation;
        ch.previous_rotation = ch.rotation;
        ch.translation = {position.GetX(), position.GetY(), position.GetZ()};
        ch.rotation = glm::vec3(rotation.GetX(), rotation.GetY(), rotation.GetZ());
        tc.position = ch.translation;
        tc.rotation = glm::eulerAngles(ch.rotation);
      });

  // -- Renderer Systems ---

  self.world.system<const TransformComponent, CameraComponent>("camera_update")
      .kind(flecs::PostUpdate)
      .each([](const TransformComponent& tc, CameraComponent& cc) {
        const auto screen_extent = App::get()->get_swapchain_extent();
        cc.position = tc.position;
        cc.pitch = tc.rotation.x;
        cc.yaw = tc.rotation.y;
        Camera::update(cc, screen_extent);
      });

  self.world.system<const TransformComponent, MeshComponent>("meshes_update")
      .kind(flecs::PostUpdate)
      .each([](const TransformComponent& tc, MeshComponent& mc) {});

  self.world.system<SpriteComponent>("sprite_update")
      .kind(flecs::PostUpdate)
      .each([](const flecs::entity entity, SpriteComponent& sprite) {
        if (RendererCVar::cvar_draw_bounding_boxes.get()) {
          DebugRenderer::draw_aabb(sprite.rect, glm::vec4(1, 1, 1, 1.0f));
        }
      });

  self.world.system<SpriteComponent, SpriteAnimationComponent>("sprite_animation_update")
      .kind(flecs::PostUpdate)
      .each([](flecs::iter& it, size_t, SpriteComponent& sprite, SpriteAnimationComponent& sprite_animation) {
        const auto asset_manager = App::get_system<AssetManager>(EngineSystems::AssetManager);
        auto* material = asset_manager->get_material(sprite.material);

        if (sprite_animation.num_frames < 1 || sprite_animation.fps < 1 || sprite_animation.columns < 1 || !material ||
            !material->albedo_texture)
          return;

        const auto dt = glm::clamp(static_cast<float>(it.delta_time()), 0.0f, 0.25f);
        const auto time = sprite_animation.current_time + dt;

        sprite_animation.current_time = time;

        const float duration = static_cast<float>(sprite_animation.num_frames) / sprite_animation.fps;
        u32 frame = math::flooru32(sprite_animation.num_frames * (time / duration));

        if (time > duration) {
          if (sprite_animation.inverted) {
            // Remove/add a frame depending on the direction
            const float frame_length = 1.0f / sprite_animation.fps;
            sprite_animation.current_time -= duration - frame_length;
          } else {
            sprite_animation.current_time -= duration;
          }
        }

        if (sprite_animation.loop)
          frame %= sprite_animation.num_frames;
        else
          frame = glm::min(frame, sprite_animation.num_frames - 1);

        frame = sprite_animation.inverted ? sprite_animation.num_frames - 1 - frame : frame;

        const u32 frame_x = frame % sprite_animation.columns;
        const u32 frame_y = frame / sprite_animation.columns;

        const auto* albedo_texture = asset_manager->get_texture(material->albedo_texture);
        auto& uv_size = material->uv_size;

        auto texture_size = glm::vec2(albedo_texture->get_extent().width, albedo_texture->get_extent().height);
        uv_size = {sprite_animation.frame_size[0] * 1.f / texture_size[0],
                   sprite_animation.frame_size[1] * 1.f / texture_size[1]};
        material->uv_offset = material->uv_offset + glm::vec2{uv_size.x * frame_x, uv_size.y * frame_y};
      });
}

auto Scene::runtime_start(this Scene& self) -> void {
  ZoneScoped;

  self.running = true;

  self.run_deferred_functions();

  // Physics
  {
    ZoneNamedN(z, "Physics Start", true);
    self.body_activation_listener_3d = new Physics3DBodyActivationListener();
    self.contact_listener_3d = new Physics3DContactListener(&self);
    const auto physics_system = App::get_system<Physics>(EngineSystems::Physics)->get_physics_system();
    physics_system->SetBodyActivationListener(self.body_activation_listener_3d);
    physics_system->SetContactListener(self.contact_listener_3d);

    // Rigidbodies
    self.world.query_builder<const TransformComponent, RigidbodyComponent>().build().each(
        [&self](flecs::entity e, const TransformComponent& tc, RigidbodyComponent& rb) {
          rb.previous_translation = rb.translation = tc.position;
          rb.previous_rotation = rb.rotation = tc.rotation;
          self.create_rigidbody(e, tc, rb);
        });

    // Characters
    self.world.query_builder<const TransformComponent, CharacterControllerComponent>().build().each(
        [&self](const TransformComponent& tc, CharacterControllerComponent& ch) {
          self.create_character_controller(tc, ch);
        });

    physics_system->OptimizeBroadPhase();
  }

  // Scripting
  {
    ZoneNamedN(z, "LuaScripting/on_scene_start", true);
    self.world.query_builder<const LuaScriptComponent>().build().each(
        [&self](const flecs::entity& e, const LuaScriptComponent& c) {
          auto* asset_man = App::get_asset_manager();
          if (auto* script = asset_man->get_script(c.script_uuid)) {
            script->on_scene_start(&self, e);
          }
        });
  }
}

auto Scene::runtime_stop(this Scene& self) -> void {
  ZoneScoped;

  self.running = false;

  // Physics
  {
    ZoneNamedN(z, "physics_stop", true);
    const auto physics = App::get_system<Physics>(EngineSystems::Physics);
    self.world.query_builder<RigidbodyComponent>().build().each(
        [physics](const flecs::entity& e, const RigidbodyComponent& rb) {
          if (rb.runtime_body) {
            JPH::BodyInterface& body_interface = physics->get_physics_system()->GetBodyInterface();
            const auto* body = static_cast<const JPH::Body*>(rb.runtime_body);
            body_interface.RemoveBody(body->GetID());
            body_interface.DestroyBody(body->GetID());
          }
        });
    self.world.query_builder<CharacterControllerComponent>().build().each(
        [physics](const flecs::entity& e, CharacterControllerComponent& ch) {
          if (ch.character) {
            JPH::BodyInterface& body_interface = physics->get_physics_system()->GetBodyInterface();
            auto* character = reinterpret_cast<JPH::Character*>(ch.character);
            body_interface.RemoveBody(character->GetBodyID());
            ch.character = nullptr;
          }
        });

    delete self.body_activation_listener_3d;
    delete self.contact_listener_3d;
    self.body_activation_listener_3d = nullptr;
    self.contact_listener_3d = nullptr;
  }

  // Scripting
  {
    ZoneNamedN(z, "LuaScripting/on_scene_deinit", true);
    self.world.query_builder<const LuaScriptComponent>().build().each(
        [&self](const flecs::entity& e, const LuaScriptComponent& c) {
          auto* asset_man = App::get_asset_manager();
          if (auto* script = asset_man->get_script(c.script_uuid)) {
            script->on_scene_stop(&self, e);
          }
        });
  }
}

auto Scene::runtime_update(this Scene& self, const Timestep& delta_time) -> void {
  ZoneScoped;

  self.run_deferred_functions();

  // TODO: Pass our delta_time?
  self.world.progress();

  self._render_pipeline->on_update(&self);
  self.dirty_transforms.clear();

  if (RendererCVar::cvar_enable_physics_debug_renderer.get()) {
    auto physics = App::get_system<Physics>(EngineSystems::Physics);
    physics->debug_draw();
  }
}

auto Scene::defer_function(this Scene& self, const std::function<void(Scene* scene)>& func) -> void {
  ZoneScoped;

  self.deferred_functions_.emplace_back(func);
}

auto Scene::run_deferred_functions(this Scene& self) -> void {
  ZoneScoped;

  if (!self.deferred_functions_.empty()) {
    for (auto& func : self.deferred_functions_) {
      func(&self);
    }
    self.deferred_functions_.clear();
  }
}

auto Scene::disable_phases(const std::vector<flecs::entity_t>& phases) -> void {
  ZoneScoped;
  for (auto& phase : phases) {
    if (!world.entity(phase).has(flecs::Disabled))
      world.entity(phase).disable();
  }
}

auto Scene::enable_all_phases() -> void {
  ZoneScoped;
  world.entity(flecs::PreUpdate).enable();
  world.entity(flecs::OnUpdate).enable();
  world.entity(flecs::PostUpdate).enable();
}

void Scene::on_render(const vuk::Extent3D extent, const vuk::Format format) {
  ZoneScoped;

  {
    ZoneNamedN(z, "LuaScripting/on_render", true);
    world.query_builder<const LuaScriptComponent>().build().each([extent, format](const LuaScriptComponent& c) {
      auto* asset_man = App::get_asset_manager();
      if (auto* script = asset_man->get_script(c.script_uuid)) {
        script->on_scene_render(extent, format);
      }
    });
  }
}

auto Scene::create_entity(const std::string& name) const -> flecs::entity {
  ZoneScoped;

  if (auto found_entity = world.lookup(name.c_str()))
    return found_entity;

  flecs::entity e = world.entity();
  if (name.empty()) {
    memory::ScopedStack stack;
    e.set_name(stack.format_char("Entity {}", e.id()));
  } else {
    e.set_name(name.c_str());
  }

  return e.add<TransformComponent>().add<LayerComponent>();
}

auto Scene::create_mesh_entity(this Scene& self, const UUID& asset_uuid) -> flecs::entity {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  // sanity check
  if (!asset_man->get_asset(asset_uuid)) {
    OX_LOG_ERROR("Cannot import an invalid model '{}' into the scene!", asset_uuid.str());
    return {};
  }

  // acquire model
  if (!asset_man->load_mesh(asset_uuid)) {
    return {};
  }

  auto* imported_model = asset_man->get_mesh(asset_uuid);
  auto& default_scene = imported_model->scenes[imported_model->default_scene_index];
  auto root_entity = self.create_entity(self.world.lookup(default_scene.name.c_str()) ? std::string{}
                                                                                      : default_scene.name);

  auto visit_nodes = [&self, //
                      &imported_model,
                      &asset_uuid](this auto& visitor, flecs::entity& root, std::vector<usize>& node_indices) -> void {
    for (const auto node_index : node_indices) {
      auto& cur_node = imported_model->nodes[node_index];
      auto node_entity = self.create_entity(self.world.lookup(cur_node.name.c_str()) ? std::string{} : cur_node.name);

      const auto T = glm::translate(glm::mat4(1.0f), cur_node.translation);
      const auto R = glm::mat4_cast(cur_node.rotation);
      const auto S = glm::scale(glm::mat4(1.0f), cur_node.scale);
      auto TRS = T * R * S;
      auto transform_comp = TransformComponent{};
      {
        glm::quat rotation = {};
        glm::vec3 skew = {};
        glm::vec4 perspective = {};
        glm::decompose(TRS, transform_comp.scale, rotation, transform_comp.position, skew, perspective);
        transform_comp.rotation = glm::eulerAngles(glm::quat(rotation[3], rotation[0], rotation[1], rotation[2]));
      }
      node_entity.set(transform_comp);

      if (cur_node.mesh_index.has_value()) {
        node_entity.set<MeshComponent>(
            {.mesh_index = static_cast<u32>(cur_node.mesh_index.value()), .mesh_uuid = asset_uuid});
      }

      node_entity.child_of(root);
      node_entity.modified<TransformComponent>();

      visitor(node_entity, cur_node.child_indices);
    }
  };

  visit_nodes(root_entity, default_scene.node_indices);

  return root_entity;
}

auto Scene::copy(const std::shared_ptr<Scene>& src_scene) -> std::shared_ptr<Scene> {
  ZoneScoped;

  // Copies the world but not the renderer.

  std::shared_ptr<Scene> new_scene = std::make_shared<Scene>(src_scene->scene_name, src_scene->_render_pipeline);

  JsonWriter writer{};
  writer.begin_obj();
  writer["entities"].begin_array();
  src_scene->world.query_builder().with<TransformComponent>().build().each([&writer](flecs::entity e) {
    if (e.parent() == flecs::entity::null() && !e.has<Hidden>()) {
      entity_to_json(writer, e);
    }
  });
  writer.end_array();
  writer.end_obj();

  auto content = simdjson::padded_string(writer.stream.str());
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(content);
  if (doc.error()) {
    OX_LOG_ERROR("Failed to parse scene file! {}", simdjson::error_message(doc.error()));
    return nullptr;
  }
  auto entities_array = doc["entities"];

  std::vector<UUID> requested_assets = {};
  for (auto entity_json : entities_array.get_array()) {
    if (!json_to_entity(*new_scene, flecs::entity::null(), entity_json.value_unsafe(), requested_assets).second) {
      return nullptr;
    }
  }

  new_scene->meshes_dirty = true;

  return new_scene;
}

auto Scene::get_world_transform(const flecs::entity entity) const -> glm::mat4 {
  const auto& tc = entity.get<TransformComponent>();
  const auto parent = entity.parent();
  const glm::mat4 parent_transform = parent != flecs::entity::null() ? get_world_transform(parent) : glm::mat4(1.0f);
  return parent_transform * glm::translate(glm::mat4(1.0f), tc.position) * glm::toMat4(glm::quat(tc.rotation)) *
         glm::scale(glm::mat4(1.0f), tc.scale);
}

auto Scene::get_local_transform(flecs::entity entity) const -> glm::mat4 {
  const auto& tc = entity.get<TransformComponent>();
  return glm::translate(glm::mat4(1.0f), tc.position) * glm::toMat4(glm::quat(tc.rotation)) *
         glm::scale(glm::mat4(1.0f), tc.scale);
}

auto Scene::set_dirty(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto visit_parent = [](this auto& visitor, Scene& s, flecs::entity e) -> glm::mat4 {
    auto local_mat = glm::mat4(1.0f);
    if (e.has<TransformComponent>()) {
      local_mat = s.get_local_transform(e);
    }

    auto parent = e.parent();
    if (parent) {
      return visitor(s, parent) * local_mat;
    } else {
      return local_mat;
    }
  };

  OX_ASSERT(entity.has<TransformComponent>());
  auto it = self.entity_transforms_map.find(entity);
  if (it == self.entity_transforms_map.end()) {
    return;
  }

  auto transform_id = it->second;
  auto* gpu_transform = self.transforms.slot(transform_id);
  gpu_transform->local = glm::mat4(1.0f);
  gpu_transform->world = visit_parent(self, entity);
  gpu_transform->normal = glm::mat3(gpu_transform->world);
  self.dirty_transforms.push_back(transform_id);

  // notify children
  entity.children([](flecs::entity e) {
    if (e.has<TransformComponent>()) {
      e.modified<TransformComponent>();
    }
  });
}

auto Scene::get_entity_transform_id(flecs::entity entity) const -> option<GPU::TransformID> {
  auto it = entity_transforms_map.find(entity);
  if (it == entity_transforms_map.end())
    return nullopt;
  return it->second;
}

auto Scene::get_entity_transform(GPU::TransformID transform_id) const -> const GPU::Transforms* {
  return transforms.slotc(transform_id);
}

auto Scene::add_transform(this Scene& self, flecs::entity entity) -> GPU::TransformID {
  ZoneScoped;

  auto id = self.transforms.create_slot();
  self.entity_transforms_map.emplace(entity, id);

  return id;
}

auto Scene::remove_transform(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto it = self.entity_transforms_map.find(entity);
  if (it == self.entity_transforms_map.end()) {
    return;
  }

  self.transforms.destroy_slot(it->second);
  self.entity_transforms_map.erase(it);
}

auto Scene::attach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool {
  ZoneScoped;

  auto transforms_it = self.entity_transforms_map.find(entity);
  if (!self.entity_transforms_map.contains(entity)) {
    OX_LOG_FATAL("Target entity must have a transform component!");
    return false;
  }

  const auto transform_id = transforms_it->second;

  auto old_mesh_uuid = std::ranges::find_if(self.rendering_meshes_map, [transform_id](const auto& entry) {
    const auto& [_, transform_ids] = entry;
    return std::ranges::contains(transform_ids, transform_id);
  });

  if (old_mesh_uuid != self.rendering_meshes_map.end()) {
    self.detach_mesh(entity, old_mesh_uuid->first.first, mesh_index);
  }

  auto [instances_it, inserted] = self.rendering_meshes_map.try_emplace(std::pair{mesh_uuid, mesh_index});
  if (!inserted && instances_it == self.rendering_meshes_map.end()) {
    return false;
  }

  instances_it->second.emplace_back(transform_id);
  self.meshes_dirty = true;
  self.set_dirty(entity);

  return true;
}

auto Scene::detach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool {
  ZoneScoped;

  auto instances_it = self.rendering_meshes_map.find(std::pair(mesh_uuid, mesh_index));
  auto transforms_it = self.entity_transforms_map.find(entity);
  if (instances_it == self.rendering_meshes_map.end() || transforms_it == self.entity_transforms_map.end()) {
    return false;
  }

  const auto transform_id = transforms_it->second;
  auto& instances = instances_it->second;
  std::erase_if(instances, [transform_id](const GPU::TransformID& id) { return id == transform_id; });
  self.meshes_dirty = true;

  if (instances.empty()) {
    self.rendering_meshes_map.erase(instances_it);
  }

  return true;
}

auto Scene::on_contact_added(const JPH::Body& body1,
                             const JPH::Body& body2,
                             const JPH::ContactManifold& manifold,
                             const JPH::ContactSettings& settings) -> void {
  ZoneScoped;

  physics_events.emit<SceneEvents::OnContactAddedEvent>({body1, body2, manifold, settings});
}

auto Scene::on_contact_persisted(const JPH::Body& body1,
                                 const JPH::Body& body2,
                                 const JPH::ContactManifold& manifold,
                                 const JPH::ContactSettings& settings) -> void {
  ZoneScoped;

  physics_events.emit<SceneEvents::OnContactPersistedEvent>({body1, body2, manifold, settings});
}

auto Scene::on_contact_removed(const JPH::SubShapeIDPair& sub_shape_pair) -> void {
  ZoneScoped;

  physics_events.emit<SceneEvents::OnContactRemovedEvent>({sub_shape_pair});
}

auto Scene::on_body_activated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  physics_events.emit<SceneEvents::OnBodyActivatedEvent>({body_id, body_user_data});
}

auto Scene::on_body_deactivated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  physics_events.emit<SceneEvents::OnBodyDeactivatedEvent>({body_id, body_user_data});
}

auto Scene::create_rigidbody(flecs::entity entity, const TransformComponent& transform, RigidbodyComponent& component)
    -> void {
  ZoneScoped;
  if (!running)
    return;

  // TODO: We should get rid of 'new' usages and use JPH::Ref<> instead.

  auto physics = App::get_system<Physics>(EngineSystems::Physics);

  auto& body_interface = physics->get_body_interface();
  if (component.runtime_body) {
    body_interface.DestroyBody(static_cast<JPH::Body*>(component.runtime_body)->GetID());
    component.runtime_body = nullptr;
  }

  JPH::MutableCompoundShapeSettings compound_shape_settings;
  float max_scale_component = glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);

  const auto entity_name = std::string(entity.name());

  if (const auto* bc = entity.try_get<BoxColliderComponent>()) {
    const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), bc->friction, bc->restitution);

    glm::vec3 scale = bc->size;
    JPH::BoxShapeSettings shape_settings({glm::abs(scale.x), glm::abs(scale.y), glm::abs(scale.z)}, 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, bc->density));

    compound_shape_settings.AddShape(
        {bc->offset.x, bc->offset.y, bc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
  }

  if (const auto* sc = entity.try_get<SphereColliderComponent>()) {
    const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), sc->friction, sc->restitution);

    float radius = 2.0f * sc->radius * max_scale_component;
    JPH::SphereShapeSettings shape_settings(glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, sc->density));

    compound_shape_settings.AddShape(
        {sc->offset.x, sc->offset.y, sc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
  }

  if (const auto* cc = entity.try_get<CapsuleColliderComponent>()) {
    const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), cc->friction, cc->restitution);

    float radius = 2.0f * cc->radius * max_scale_component;
    JPH::CapsuleShapeSettings shape_settings(glm::max(0.01f, cc->height) * 0.5f, glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, cc->density));

    compound_shape_settings.AddShape(
        {cc->offset.x, cc->offset.y, cc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
  }

  if (const auto* tcc = entity.try_get<TaperedCapsuleColliderComponent>()) {
    const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), tcc->friction, tcc->restitution);

    float top_radius = 2.0f * tcc->top_radius * max_scale_component;
    float bottom_radius = 2.0f * tcc->bottom_radius * max_scale_component;
    JPH::TaperedCapsuleShapeSettings shape_settings(
        glm::max(0.01f, tcc->height) * 0.5f, glm::max(0.01f, top_radius), glm::max(0.01f, bottom_radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, tcc->density));

    compound_shape_settings.AddShape(
        {tcc->offset.x, tcc->offset.y, tcc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
  }

  if (const auto* cc = entity.try_get<CylinderColliderComponent>()) {
    const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), cc->friction, cc->restitution);

    float radius = 2.0f * cc->radius * max_scale_component;
    JPH::CylinderShapeSettings shape_settings(glm::max(0.01f, cc->height) * 0.5f, glm::max(0.01f, radius), 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, cc->density));

    compound_shape_settings.AddShape(
        {cc->offset.x, cc->offset.y, cc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
  }

#if TODO
  (const auto* mc = entity.try_get<MeshColliderComponent>()) {
    if (const auto* mesh_component = entity.get<MeshComponent>()) {
      const auto* mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), mc->friction, mc->restitution);

      // TODO: We should only get the vertices and indices for this particular MeshComponent using
      // MeshComponent::node_index
      auto vertices = mesh_component->mesh_base->_vertices;
      const auto& indices = mesh_component->mesh_base->_indices;

      // scale vertices
      const auto world_transform = get_world_transform(entity);
      for (auto& vert : vertices) {
        glm::vec4 scaled_pos = world_transform * glm::vec4(vert.position, 1.0);
        vert.position = glm::vec3(scaled_pos);
      }

      const uint32_t vertex_count = static_cast<uint32_t>(vertices.size());
      const uint32_t index_count = static_cast<uint32_t>(indices.size());
      const uint32_t triangle_count = vertex_count / 3;

      JPH::VertexList vertex_list;
      vertex_list.resize(vertex_count);
      for (uint32_t i = 0; i < vertex_count; ++i)
        vertex_list[i] = JPH::Float3(vertices[i].position.x, vertices[i].position.y, vertices[i].position.z);

      JPH::IndexedTriangleList indexedTriangleList;
      indexedTriangleList.resize(index_count * 2);

      for (uint32_t i = 0; i < triangle_count; ++i) {
        indexedTriangleList[i * 2 + 0].mIdx[0] = indices[i * 3 + 0];
        indexedTriangleList[i * 2 + 0].mIdx[1] = indices[i * 3 + 1];
        indexedTriangleList[i * 2 + 0].mIdx[2] = indices[i * 3 + 2];

        indexedTriangleList[i * 2 + 1].mIdx[2] = indices[i * 3 + 0];
        indexedTriangleList[i * 2 + 1].mIdx[1] = indices[i * 3 + 1];
        indexedTriangleList[i * 2 + 1].mIdx[0] = indices[i * 3 + 2];
      }

      JPH::PhysicsMaterialList material_list = {};
      material_list.emplace_back(mat);
      JPH::MeshShapeSettings shape_settings(vertex_list, indexedTriangleList, material_list);
      compound_shape_settings.AddShape(
          {mc->offset.x, mc->offset.y, mc->offset.z}, JPH::Quat::sIdentity(), shape_settings.Create().Get());
    }
  }
#endif

  // Body
  auto rotation = glm::quat(transform.rotation);

  u8 layer_index = 1; // Default Layer
  if (const auto* layer_component = entity.try_get<LayerComponent>()) {
    const auto collision_mask_it = physics->layer_collision_mask.find(layer_component->layer);
    if (collision_mask_it != physics->layer_collision_mask.end())
      layer_index = collision_mask_it->second.index;
  }

  JPH::BodyCreationSettings body_settings(compound_shape_settings.Create().Get(),
                                          {transform.position.x, transform.position.y, transform.position.z},
                                          {rotation.x, rotation.y, rotation.z, rotation.w},
                                          static_cast<JPH::EMotionType>(component.type),
                                          layer_index);

  JPH::MassProperties mass_properties;
  mass_properties.mMass = glm::max(0.01f, component.mass);
  body_settings.mMassPropertiesOverride = mass_properties;
  body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  body_settings.mAllowSleeping = component.allow_sleep;
  body_settings.mLinearDamping = glm::max(0.0f, component.linear_drag);
  body_settings.mAngularDamping = glm::max(0.0f, component.angular_drag);
  body_settings.mMotionQuality = component.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
  body_settings.mGravityFactor = component.gravity_scale;
  body_settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(component.allowed_dofs);

  body_settings.mIsSensor = component.is_sensor;

  JPH::Body* body = body_interface.CreateBody(body_settings);

  JPH::EActivation activation = component.awake && component.type != RigidbodyComponent::BodyType::Static
                                    ? JPH::EActivation::Activate
                                    : JPH::EActivation::DontActivate;
  body_interface.AddBody(body->GetID(), activation);

  body->SetUserData((u64)entity);

  component.runtime_body = body;
}

void Scene::create_character_controller(const TransformComponent& transform,
                                        CharacterControllerComponent& component) const {
  ZoneScoped;
  if (!running)
    return;

  const auto physics = App::get_system<Physics>(EngineSystems::Physics);

  const auto position = JPH::Vec3(transform.position.x, transform.position.y, transform.position.z);
  const auto capsule_shape = JPH::RotatedTranslatedShapeSettings(
                                 JPH::Vec3(0,
                                           0.5f * component.character_height_standing +
                                               component.character_radius_standing,
                                           0),
                                 JPH::Quat::sIdentity(),
                                 new JPH::CapsuleShape(0.5f * component.character_height_standing,
                                                       component.character_radius_standing))
                                 .Create()
                                 .Get();

  // Create character
  const std::shared_ptr<JPH::CharacterSettings> settings = std::make_shared<JPH::CharacterSettings>();
  settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
  settings->mLayer = PhysicsLayers::MOVING;
  settings->mShape = capsule_shape;
  settings->mFriction = 0.0f;                                                     // For now this is not set.
  settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(),
                                           -component.character_radius_standing); // Accept contacts that touch the
                                                                                  // lower sphere of the capsule
  // TODO: Cleanup
  component.character = new JPH::Character(
      settings.get(), position, JPH::Quat::sIdentity(), 0, physics->get_physics_system());
  reinterpret_cast<JPH::Character*>(component.character)->AddToPhysicsSystem(JPH::EActivation::Activate);
}

auto Scene::save_to_file(this const Scene& self, std::string path) -> bool {
  ZoneScoped;

  JsonWriter writer{};

  writer.begin_obj();

  writer["name"] = self.scene_name;

  writer["entities"].begin_array();
  const auto q = self.world.query_builder().with<TransformComponent>().build();
  q.each([&writer](flecs::entity e) {
    if (e.parent() == flecs::entity::null() && !e.has<Hidden>()) {
      entity_to_json(writer, e);
    }
  });
  writer.end_array();

  writer.end_obj();

  std::ofstream filestream(path);
  filestream << writer.stream.rdbuf();

  OX_LOG_INFO("Saved scene {0}.", self.scene_name);

  return true;
}

auto Scene::load_from_file(this Scene& self, const std::string& path) -> bool {
  ZoneScoped;
  namespace sj = simdjson;

  std::string content = fs::read_file(path);
  if (content.empty()) {
    OX_LOG_ERROR("Failed to read/open file {}!", path);
    return false;
  }

  content = sj::padded_string(content);
  sj::ondemand::parser parser;
  auto doc = parser.iterate(content);
  if (doc.error()) {
    OX_LOG_ERROR("Failed to parse scene file! {}", sj::error_message(doc.error()));
    return false;
  }

  auto name_json = doc["name"];
  if (name_json.error()) {
    OX_LOG_ERROR("Scene files must have names!");
    return false;
  }

  self.scene_name = name_json.get_string().value_unsafe();

  auto entities_array = doc["entities"];

  std::vector<UUID> requested_assets = {};
  for (auto entity_json : entities_array.get_array()) {
    if (!json_to_entity(self, flecs::entity::null(), entity_json.value_unsafe(), requested_assets).second) {
      return false;
    }
  }

  OX_LOG_TRACE("Loading scene {} with {} assets...", self.scene_name, requested_assets.size());
  for (const auto& uuid : requested_assets) {
    auto* asset_man = App::get_system<AssetManager>(EngineSystems::AssetManager);
    if (uuid && asset_man->get_asset(uuid)) {
      asset_man->load_asset(uuid);
    }
  }

  return true;
}
} // namespace ox
