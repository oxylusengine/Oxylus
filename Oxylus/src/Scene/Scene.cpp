#include "Scene/Scene.hpp"

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
// clang-format on
#include <glm/gtx/matrix_decompose.hpp>
#include <simdjson.h>
#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Physics/Physics.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Physics/PhysicsMaterial.hpp"
#include "Render/Camera.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/EntitySerializer.hpp"
#include "Scripting/LuaManager.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Random.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
struct JsonEntityDeserializer : IEntitySerializer {
  simdjson::ondemand::value json_value;
  memory::ScopedStack stack;
  std::vector<UUID> requested_assets = {};

  JsonEntityDeserializer(flecs::world& world_, simdjson::ondemand::value value_)
      : IEntitySerializer(world_),
        json_value(std::move(value_)) {}

  auto on_primitive(std::string_view name, Primitive primitive) -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error()) {
      return;
    }

    std::visit(
      ox::match{
        [](const auto&) {},
        [&](bool* v) {
          auto result = field_result.get_bool();
          if (!result.error()) {
            *v = result.value_unsafe();
          }
        },
        [&](c8* v) {
          auto result = field_result.get_string();
          if (!result.error() && !result.value_unsafe().empty()) {
            *v = result.value_unsafe()[0];
          }
        },
        [&](i8* v) {
          auto result = field_result.get_int64();
          if (!result.error()) {
            *v = static_cast<i8>(result.value_unsafe());
          }
        },
        [&](u8* v) {
          auto result = field_result.get_uint64();
          if (!result.error()) {
            *v = static_cast<u8>(result.value_unsafe());
          }
        },
        [&](i16* v) {
          auto result = field_result.get_int64();
          if (!result.error()) {
            *v = static_cast<i16>(result.value_unsafe());
          }
        },
        [&](u16* v) {
          auto result = field_result.get_uint64();
          if (!result.error()) {
            *v = static_cast<u16>(result.value_unsafe());
          }
        },
        [&](i32* v) {
          auto result = field_result.get_int64();
          if (!result.error()) {
            *v = static_cast<i32>(result.value_unsafe());
          }
        },
        [&](u32* v) {
          auto result = field_result.get_uint64();
          if (!result.error()) {
            *v = static_cast<u32>(result.value_unsafe());
          }
        },
        [&](i64* v) {
          auto result = field_result.get_int64();
          if (!result.error()) {
            *v = result.value_unsafe();
          }
        },
        [&](u64* v) {
          auto result = field_result.get_uint64();
          if (!result.error()) {
            *v = result.value_unsafe();
          }
        },
        [&](f32* v) {
          auto result = field_result.get_double();
          if (!result.error()) {
            *v = static_cast<f32>(result.value_unsafe());
          }
        },
        [&](f64* v) {
          auto result = field_result.get_double();
          if (!result.error()) {
            *v = result.value_unsafe();
          }
        },
      },
      primitive
    );
  }

  auto on_string(std::string_view name, const c8** str) -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error()) {
      return;
    }

    auto result = field_result.get_string();
    if (!result.error()) {
      auto str_view = result.value_unsafe();
      auto* str_copy = stack.null_terminate_cstr(str_view);
      *str = str_copy;
    }
  }

  auto on_entity(std::string_view name, flecs::entity* entity) -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error()) {
      return;
    }

    auto result = field_result.get_string();
    if (!result.error()) {
      auto entity_name = result.value_unsafe();
      auto* entity_name_cstr = stack.null_terminate_cstr(entity_name);
      auto found_entity = world.lookup(entity_name_cstr);
      if (found_entity.is_valid()) {
        *entity = found_entity;
      }
    }
  }

  auto on_component(std::string_view name, flecs::id_t* component) -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error()) {
      return;
    }

    auto result = field_result.get_string();
    if (!result.error()) {
      auto comp_name = result.value_unsafe();
      auto* comp_name_cstr = stack.null_terminate_cstr(comp_name);
      auto comp_entity = world.lookup(comp_name_cstr);
      if (comp_entity.is_valid()) {
        *component = comp_entity.id();
      }
    }
  }

  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    ZoneScoped;

    if (!name.empty()) {
      auto field_result = json_value[name];
      if (field_result.error()) {
        return;
      }

      auto nested_value = field_result.get_object();
      if (nested_value.error()) {
        return;
      }

      auto nested_deserializer = JsonEntityDeserializer(world, field_result.value_unsafe());
      nested_deserializer.serialize_ops(ops + 1, op_count - 1, base);
    } else {
      serialize_ops(ops + 1, op_count - 1, base);
    }
  }

  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error()) {
      return;
    }

    auto* opaque_info = ecs_get(world, field_type, EcsOpaque);
    if (!opaque_info) {
      return;
    }

    if (opaque_type == flecs::Bool) {
      auto result = field_result.get_bool();
      if (!result.error() && opaque_info->assign_bool) {
        opaque_info->assign_bool(field_ptr, result.value_unsafe());
      }
    } else if (opaque_type == flecs::Char) {
      auto result = field_result.get_string();
      if (!result.error() && !result.value_unsafe().empty() && opaque_info->assign_char) {
        opaque_info->assign_char(field_ptr, result.value_unsafe()[0]);
      }
    } else if (opaque_type == flecs::Byte || opaque_type == flecs::U8) {
      auto result = field_result.get_uint64();
      if (!result.error() && opaque_info->assign_uint) {
        opaque_info->assign_uint(field_ptr, static_cast<u64>(result.value_unsafe()));
      }
    } else if (opaque_type == flecs::U16 || opaque_type == flecs::U32 || opaque_type == flecs::U64 ||
               opaque_type == flecs::Uptr) {
      auto result = field_result.get_uint64();
      if (!result.error() && opaque_info->assign_uint) {
        opaque_info->assign_uint(field_ptr, result.value_unsafe());
      }
    } else if (opaque_type == flecs::I8 || opaque_type == flecs::I16 || opaque_type == flecs::I32 ||
               opaque_type == flecs::I64 || opaque_type == flecs::Iptr) {
      auto result = field_result.get_int64();
      if (!result.error() && opaque_info->assign_int) {
        opaque_info->assign_int(field_ptr, result.value_unsafe());
      }
    } else if (opaque_type == flecs::F32 || opaque_type == flecs::F64) {
      auto result = field_result.get_double();
      if (!result.error() && opaque_info->assign_float) {
        opaque_info->assign_float(field_ptr, result.value_unsafe());
      }
    } else if (opaque_type == flecs::String) {
      auto result = field_result.get_string();
      if (!result.error() && opaque_info->assign_string) {
        auto* str_cstr = stack.null_terminate_cstr(result.value_unsafe());
        opaque_info->assign_string(field_ptr, str_cstr);

        if (field_type == world.entity<UUID>()) {
          requested_assets.push_back(*static_cast<UUID*>(field_ptr));
        }
      }
    }
  }
};

auto Scene::safe_entity_name(this const Scene& self, std::string prefix) -> std::string {
  ZoneScoped;

  u32 index = 0;
  std::string new_entity_name = prefix;
  while (self.world.lookup(new_entity_name.data()) > 0) {
    index += 1;
    new_entity_name = fmt::format("{}_{}", prefix, index);
  }
  return new_entity_name;
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

Scene::Scene(const std::string& name) { init(name); }

Scene::~Scene() {
  if (running)
    runtime_stop();

  for (auto& [uuid, system] : lua_systems) {
    system->on_remove(this);
  }

  world.release();

  lua_systems.clear();
  auto& lua_manager = App::mod<LuaManager>();
  lua_manager.get_state()->collect_gc();
}

auto Scene::init(this Scene& self, const std::string& name) -> void {
  ZoneScoped;
  self.scene_name = name;

  self.component_db.import_module(self.world.import <CoreComponentsModule>());

  if (App::has_mod<Renderer>()) {
    auto& renderer = App::mod<Renderer>();
    self.renderer_instance = renderer.new_instance(self);
  }

  auto& physics = App::mod<Physics>();
  self.physics_system = physics.new_system();
  self.physics_debug_renderer = physics.new_debug_renderer();

  self.world.observer<TransformComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnAdd)
    .event(flecs::OnRemove)
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

        if (mesh_event && mc.model_uuid)
          self.attach_mesh(entity, mc.model_uuid, mc.mesh_index, mc.material_uuid);
      } else if (it.event() == flecs::OnAdd) {
        self.add_transform(entity);
        self.set_dirty(entity);
      } else if (it.event() == flecs::OnRemove) {
        if (mc.model_uuid)
          self.detach_mesh(entity);

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

  self.world.observer<SpriteComponent>().event(flecs::OnAdd).each([](flecs::iter& it, usize i, SpriteComponent& c) {
    auto& asset_man = App::mod<AssetManager>();
    if (it.event() == flecs::OnAdd) {
      if (!c.material) {
        c.material = asset_man.create_asset(AssetType::Material, {});
        asset_man.load_material(c.material, Material{});
      }
    }
  });

  self.world.observer<SpriteComponent>()
    .event(flecs::OnRemove)
    .with<AssetOwner>()
    .each([](flecs::iter& it, usize i, SpriteComponent& c) {
      auto& asset_man = App::mod<AssetManager>();
      if (it.event() == flecs::OnRemove) {
        if (auto* material_asset = asset_man.get_asset(c.material)) {
          asset_man.unload_asset(material_asset->uuid);
        }
      }
    });

  self.world.observer<AudioListenerComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnAdd)
    .each([](flecs::iter& it, usize i, AudioListenerComponent& c) {
      auto& audio_engine = App::mod<AudioEngine>();
      audio_engine.set_listener_cone(c.listener_index, c.cone_inner_angle, c.cone_outer_angle, c.cone_outer_gain);
    });

  self.world.observer<AudioSourceComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnAdd)
    .each([](flecs::iter& it, usize i, AudioSourceComponent& c) {
      auto& asset_man = App::mod<AssetManager>();
      auto* audio_asset = asset_man.get_audio(c.audio_source);
      if (!audio_asset)
        return;

      auto& audio_engine = App::mod<AudioEngine>();
      audio_engine.set_source_volume(audio_asset->get_source(), c.volume);
      audio_engine.set_source_pitch(audio_asset->get_source(), c.pitch);
      audio_engine.set_source_looping(audio_asset->get_source(), c.looping);
      audio_engine.set_source_attenuation_model(
        audio_asset->get_source(),
        static_cast<AudioEngine::AttenuationModelType>(c.attenuation_model)
      );
      audio_engine.set_source_roll_off(audio_asset->get_source(), c.roll_off);
      audio_engine.set_source_min_gain(audio_asset->get_source(), c.min_gain);
      audio_engine.set_source_max_gain(audio_asset->get_source(), c.max_gain);
      audio_engine.set_source_min_distance(audio_asset->get_source(), c.min_distance);
      audio_engine.set_source_max_distance(audio_asset->get_source(), c.max_distance);
      audio_engine
        .set_source_cone(audio_asset->get_source(), c.cone_inner_angle, c.cone_outer_angle, c.cone_outer_gain);
    });

  self.world.observer<SpriteAnimationComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnAdd)
    .each([](flecs::iter& it, usize i, SpriteAnimationComponent& c) { c.reset(); });

  self.world.observer<MeshComponent>()
    .with<AssetOwner>()
    .event(flecs::OnRemove)
    .each([](flecs::iter& it, usize i, MeshComponent& c) {
      ZoneScopedN("MeshComponent AssetOwner handling");
      auto& asset_man = App::mod<AssetManager>();
      asset_man.unload_asset(c.model_uuid);
    });

  self.world.observer<AudioSourceComponent>()
    .with<AssetOwner>()
    .event(flecs::OnRemove)
    .each([](flecs::iter& it, usize i, AudioSourceComponent& c) {
      ZoneScopedN("AudioSourceComponent AssetOwner handling");
      auto& asset_man = App::mod<AssetManager>();
      asset_man.unload_asset(c.audio_source);
    });

  self.world.observer<RigidBodyComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, RigidBodyComponent& rb) {
      ZoneScopedN("Rigidbody observer");

      if (!self.is_running())
        return;

      if (it.event() == flecs::OnSet) {
        auto entity = it.entity(i);
        auto& tc = entity.get<TransformComponent>();
        self.create_rigidbody(it.entity(i), tc, rb);
      } else if (it.event() == flecs::OnRemove) {
        auto& body_interface = self.physics_system->GetBodyInterface();
        if (rb.runtime_body) {
          auto body_id = static_cast<JPH::Body*>(rb.runtime_body)->GetID();
          body_interface.RemoveBody(body_id);
          body_interface.DestroyBody(body_id);
          rb.runtime_body = nullptr;
        }
      }
    });

  self.world.observer<CharacterControllerComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, CharacterControllerComponent& ch) {
      ZoneScopedN("CharacterController observer");

      if (!self.is_running())
        return;

      if (it.event() == flecs::OnSet) {
        auto entity = it.entity(i);
        auto& tc = entity.get<TransformComponent>();
        self.create_character_controller(entity, tc, ch);
      } else if (it.event() == flecs::OnRemove) {
        JPH::BodyInterface& body_interface = self.physics_system->GetBodyInterface();
        if (ch.character) {
          auto* character = reinterpret_cast<JPH::Character*>(ch.character);
          body_interface.RemoveBody(character->GetBodyID());
          ch.character = nullptr;
        }
      }
    });

  self.world.observer<ParticleSystemComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnAdd)
    .event(flecs::OnRemove)
    .each([](flecs::iter& it, usize i, ParticleSystemComponent& c) {
      auto& asset_man = App::mod<AssetManager>();
      if (it.event() == flecs::OnAdd) {
        if (c.play_on_awake) {
          c.system_time = 0.0f;
          c.playing = true;
        }
        if (!c.material)
          c.material = asset_man.create_asset(AssetType::Material, {});
        asset_man.load_material(c.material, Material{});

        auto parent = it.entity(i);
        for (u32 k = 0; k < c.max_particles; k++) {
          auto particle = it.world().entity().set<TransformComponent>({{}, {}, {0.5f, 0.5f, 0.5f}});
          particle.set<ParticleComponent>({.life_remaining = c.start_lifetime});
          c.particles.emplace_back(particle);
          particle.child_of(parent);
        }
      } else if (it.event() == flecs::OnRemove) {
        for (auto p : c.particles) {
          flecs::entity e{it.world(), p};
          e.destruct();
        }
      } else if (it.event() == flecs::OnSet) {
        if (auto* asset = asset_man.get_asset(c.material)) {
          if (!asset->is_loaded()) {
            asset_man.load_material(c.material, Material{});
          }
        }

        asset_man.set_material_dirty(c.material);
      }
    });

  self.world.observer<ParticleSystemComponent>()
    .event(flecs::OnRemove)
    .with<AssetOwner>()
    .each([](flecs::iter& it, usize i, ParticleSystemComponent& c) {
      auto& asset_man = App::mod<AssetManager>();
      if (it.event() == flecs::OnRemove) {
        if (auto* material_asset = asset_man.get_asset(c.material)) {
          asset_man.unload_asset(material_asset->uuid);
        }
      }
    });

  // Systems run order:
  // -- PreUpdate  -> Main Systems
  // -- OnUpdate   -> Physics Systems
  // -- PostUpdate -> Renderer Systems

  // --- Main Systems ---

  self.world.system<const TransformComponent, AudioListenerComponent>("audio_listener_update")
    .kind(flecs::PreUpdate)
    .each([&self](const flecs::entity& e, const TransformComponent& tc, AudioListenerComponent& ac) {
      if (ac.active) {
        auto& audio_engine = App::mod<AudioEngine>();
        const glm::mat4 inverted = glm::inverse(self.get_world_transform(e));
        const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
        audio_engine.set_listener_position(ac.listener_index, tc.position);
        audio_engine.set_listener_direction(ac.listener_index, -forward);
        audio_engine.set_listener_cone(ac.listener_index, ac.cone_inner_angle, ac.cone_outer_angle, ac.cone_outer_gain);
      }
    });

  self.world.system<const TransformComponent, AudioSourceComponent>("audio_source_update")
    .kind(flecs::PreUpdate)
    .each([](const flecs::entity& e, const TransformComponent& tc, const AudioSourceComponent& ac) {
      auto& asset_man = App::mod<AssetManager>();
      if (auto* audio = asset_man.get_audio(ac.audio_source)) {
        auto& audio_engine = App::mod<AudioEngine>();
        audio_engine.set_source_attenuation_model(
          audio->get_source(),
          static_cast<AudioEngine::AttenuationModelType>(ac.attenuation_model)
        );
        audio_engine.set_source_volume(audio->get_source(), ac.volume);
        audio_engine.set_source_pitch(audio->get_source(), ac.pitch);
        audio_engine.set_source_looping(audio->get_source(), ac.looping);
        audio_engine.set_source_spatialization(audio->get_source(), ac.looping);
        audio_engine.set_source_roll_off(audio->get_source(), ac.roll_off);
        audio_engine.set_source_min_gain(audio->get_source(), ac.min_gain);
        audio_engine.set_source_max_gain(audio->get_source(), ac.max_gain);
        audio_engine.set_source_min_distance(audio->get_source(), ac.min_distance);
        audio_engine.set_source_max_distance(audio->get_source(), ac.max_distance);
        audio_engine.set_source_cone(audio->get_source(), ac.cone_inner_angle, ac.cone_outer_angle, ac.cone_outer_gain);
        audio_engine.set_source_doppler_factor(audio->get_source(), ac.doppler_factor);
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
    .run([&self](flecs::iter& it) {
      OX_CHECK_NULL(self.physics_system);
      auto& p = App::mod<Physics>();
      self.physics_system->Update(it.delta_time(), 1, p.get_temp_allocator(), p.get_job_system());
    });

  self.world.system<TransformComponent, RigidBodyComponent>("rigidbody_update")
    .kind(flecs::OnUpdate)
    .tick_source(physics_tick_source)
    .each([&self](const flecs::entity& e, TransformComponent& tc, RigidBodyComponent& rb) {
      if (!rb.runtime_body)
        return;

      const auto* body = static_cast<const JPH::Body*>(rb.runtime_body);
      const auto& body_interface = self.physics_system->GetBodyInterface();

      if (!body_interface.IsActive(body->GetID()))
        return;

      const JPH::Vec3 position = body->GetPosition();
      const JPH::Quat rotation = body->GetRotation();

      rb.previous_translation = rb.translation;
      rb.previous_rotation = rb.rotation;
      rb.translation = {position.GetX(), position.GetY(), position.GetZ()};
      rb.rotation = glm::quat::wxyz(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
      tc.position = rb.translation;
      tc.rotation = rb.rotation;

      e.modified<TransformComponent>();
    });

  self.world.system<TransformComponent, CharacterControllerComponent>("character_controller_update")
    .kind(flecs::OnUpdate)
    .tick_source(physics_tick_source)
    .each([](const flecs::entity& e, TransformComponent& tc, CharacterControllerComponent& ch) {
      auto* character = reinterpret_cast<JPH::Character*>(ch.character);
      OX_CHECK_NULL(character);

      character->PostSimulation(ch.collision_tolerance);
      const JPH::Vec3 position = character->GetPosition();
      const JPH::Quat rotation = character->GetRotation();

      ch.previous_translation = ch.translation;
      ch.previous_rotation = ch.rotation;
      ch.translation = {position.GetX(), position.GetY(), position.GetZ()};
      ch.rotation = glm::quat::wxyz(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
      tc.position = ch.translation;
      tc.rotation = ch.rotation;

      e.modified<TransformComponent>();
    });

  // -- Renderer Systems ---

  self.world.system<const TransformComponent, ParticleSystemComponent>("particle_system_update")
    .kind(flecs::PostUpdate)
    .each([](flecs::iter& it, usize i, const TransformComponent& tc, ParticleSystemComponent& component) {
      const auto emit = [&it, &component](flecs::entity parent, glm::vec3 position, u32 count) {
        if (component.active_particle_count >= component.max_particles)
          return;

        for (uint32_t pool_idx = 0; pool_idx < count; ++pool_idx) {
          if (++component.pool_index >= component.max_particles)
            component.pool_index = 0;

          auto particle = flecs::entity{it.world(), component.particles[component.pool_index]};

          const auto random_float = [](f32 min, f32 max) {
            static Random random = {};
            f32 r = random.get_float();
            return min + r * (max - min);
          };

          auto new_position = position;
          new_position.x += random_float(component.position_start.x, component.position_end.x);
          new_position.y += random_float(component.position_start.y, component.position_end.y);
          new_position.z += random_float(component.position_start.z, component.position_end.z);

          particle.set<TransformComponent>({new_position});
          particle.set<ParticleComponent>({.life_remaining = component.start_lifetime});
        }
      };

      OX_CHECK_EQ(component.particles.size(), component.max_particles);

      auto entity = it.entity(i);

      const float sim_ts = it.delta_time() * component.simulation_speed;

      if (component.playing && !component.looping)
        component.system_time += sim_ts;
      const float delay = component.start_delay;
      if (component.playing && (component.looping || (component.system_time <= delay + component.duration &&
                                                      component.system_time > delay))) {
        // Emit particles in unit time
        component.spawn_time += sim_ts;
        if (component.spawn_time >= 1.0f / static_cast<float>(component.rate_over_time)) {
          component.spawn_time = 0.0f;
          emit(entity, tc.position, 1);
        }

        // Emit particles over unit distance
        if (glm::distance2(component.last_spawned_position, tc.position) > 1.0f) {
          component.last_spawned_position = tc.position;
          emit(entity, tc.position, component.rate_over_distance);
        }

        // Emit bursts of particles over time
        component.burst_time += sim_ts;
        if (component.burst_time >= component.burst_time) {
          component.burst_time = 0.0f;
          emit(entity, tc.position, component.burst_count);
        }
      }

      component.active_particle_count = 0;
    });

  self.world.system<TransformComponent, ParticleComponent>("particle_update")
    .kind(flecs::PostUpdate)
    .each([](flecs::iter& it, usize i, TransformComponent& particle_tc, ParticleComponent& particle) {
      if (particle.life_remaining <= 0.0f)
        return;

      auto evaluate_over_time = []<typename T>(T start, T end, f32 factor) -> T {
        if constexpr (std::is_same_v<T, glm::quat>) {
          if (glm::dot(start, end) < 0.0f)
            end = -end;
          return glm::slerp(end, start, factor);
        } else {
          return glm::lerp(end, start, factor);
        }
      };

      auto evaluate_by_speed = []<typename T>(T start, T end, f32 min_speed, f32 max_speed, f32 speed) -> T {
        f32 factor = math::inverse_lerp_clamped(min_speed, max_speed, speed);

        if constexpr (std::is_same_v<T, glm::quat>) {
          if (glm::dot(start, end) < 0.0f)
            end = -end;
          return glm::slerp(end, start, factor);
        } else {
          return glm::lerp(end, start, factor);
        }
      };

      auto particle_entity = it.entity(i);
      auto parent = particle_entity.parent();
      auto component = parent.get<ParticleSystemComponent>();

      float sim_ts = it.delta_time() * component.simulation_speed;

      particle.life_remaining -= sim_ts;

      const float t = glm::clamp(particle.life_remaining / component.start_lifetime, 0.0f, 1.0f);

      glm::vec3 velocity = component.start_velocity;
      if (component.velocity_over_lifetime_enabled)
        velocity *= evaluate_over_time(component.velocity_over_lifetime_start, component.velocity_over_lifetime_end, t);

      glm::vec3 force(0.0f);
      if (component.force_over_lifetime_enabled)
        force = evaluate_over_time(component.force_over_lifetime_start, component.force_over_lifetime_end, t);

      force.y += component.gravity_modifier * -9.8f;
      velocity += force * sim_ts;

      const float velocity_magnitude = glm::length(velocity);

      // Color
      particle.color = component.start_color;
      if (component.color_over_lifetime_enabled)
        particle.color *= evaluate_over_time(component.color_over_lifetime_start, component.color_over_lifetime_end, t);
      if (component.color_by_speed_enabled)
        particle.color *= evaluate_by_speed(
          component.color_by_speed_start,
          component.color_by_speed_end,
          component.color_by_speed_min_speed,
          component.color_by_speed_max_speed,
          velocity_magnitude
        );

      // Size
      particle_tc.scale = component.start_size;
      if (component.size_over_lifetime_enabled)
        particle_tc
          .scale *= evaluate_over_time(component.size_over_lifetime_start, component.size_over_lifetime_end, t);
      if (component.size_by_speed_enabled)
        particle_tc.scale *= evaluate_by_speed(
          component.size_by_speed_start,
          component.size_by_speed_end,
          component.size_by_speed_min_speed,
          component.size_by_speed_max_speed,
          velocity_magnitude
        );

      // Rotation
      particle_tc.rotation = component.start_rotation;
      if (component.rotation_over_lifetime_enabled)
        particle_tc.rotation += evaluate_over_time(
          component.rotation_over_lifetime_start,
          component.rotation_over_lifetime_end,
          t
        );
      if (component.rotation_by_speed_enabled)
        particle_tc.rotation += evaluate_by_speed(
          component.rotation_by_speed_start,
          component.rotation_by_speed_end,
          component.rotation_by_speed_min_speed,
          component.rotation_by_speed_max_speed,
          velocity_magnitude
        );

      particle_tc.position += velocity * sim_ts;

      particle_entity.modified<TransformComponent>();

      ++component.active_particle_count;
    });

  self.world.system<const TransformComponent, CameraComponent>("camera_update")
    .kind(flecs::PostUpdate)
    .each([](const TransformComponent& tc, CameraComponent& cc) {
      const auto screen_extent = App::get_vkcontext().swapchain_extent;
      cc.position = tc.position;
      cc.pitch = tc.rotation.x;
      cc.yaw = tc.rotation.y;
      Camera::update(cc, screen_extent);
    });

  self.world.system<SpriteComponent>("sprite_update")
    .kind(flecs::PostUpdate)
    .each([](const flecs::entity entity, SpriteComponent& sprite) {
      if (RendererCVar::cvar_draw_bounding_boxes.get()) {
        auto& debug_renderer = App::mod<DebugRenderer>();
        debug_renderer.draw_aabb(sprite.rect, glm::vec4(1, 1, 1, 1.0f));
      }
    });

  self.world.system<SpriteComponent, SpriteAnimationComponent>("sprite_animation_update")
    .kind(flecs::PostUpdate)
    .each([](flecs::iter& it, size_t, SpriteComponent& sprite, SpriteAnimationComponent& sprite_animation) {
      auto& asset_manager = App::mod<AssetManager>();
      auto* material = asset_manager.get_material(sprite.material);

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

      const auto* albedo_texture = asset_manager.get_texture(material->albedo_texture);
      auto& uv_size = material->uv_size;

      auto texture_size = glm::vec2(albedo_texture->get_extent().width, albedo_texture->get_extent().height);
      uv_size = {
        sprite_animation.frame_size[0] * 1.f / texture_size[0],
        sprite_animation.frame_size[1] * 1.f / texture_size[1]
      };
      material->uv_offset = material->uv_offset + glm::vec2{uv_size.x * frame_x, uv_size.y * frame_y};
    });
}

auto Scene::physics_init(this Scene& self) -> void {
  ZoneScoped;

  // Remove old bodies and reset callbacks
  self.physics_deinit();

  self.body_activation_listener_3d = std::make_unique<Physics3DBodyActivationListener>();
  self.contact_listener_3d = std::make_unique<Physics3DContactListener>(&self);
  self.physics_system->SetBodyActivationListener(self.body_activation_listener_3d.get());
  self.physics_system->SetContactListener(self.contact_listener_3d.get());

  // Rigidbodies
  self.world.query_builder<const TransformComponent, RigidBodyComponent>().build().each(
    [&self](flecs::entity e, const TransformComponent& tc, RigidBodyComponent& rb) {
      if (rb.runtime_body == nullptr) {
        rb.previous_translation = rb.translation = tc.position;
        rb.previous_rotation = rb.rotation = tc.rotation;
        self.create_rigidbody(e, tc, rb);
      }
    }
  );

  // Characters
  self.world.query_builder<const TransformComponent, CharacterControllerComponent>().build().each(
    [&self](flecs::entity e, const TransformComponent& tc, CharacterControllerComponent& ch) {
      if (ch.character == nullptr) {
        self.create_character_controller(e, tc, ch);
      }
    }
  );

  self.physics_system->OptimizeBroadPhase();
}

auto Scene::physics_deinit(this Scene& self) -> void {
  ZoneScoped;

  self.world.query_builder<RigidBodyComponent>().build().each([&self](const flecs::entity& e, RigidBodyComponent& rb) {
    if (rb.runtime_body) {
      JPH::BodyInterface& body_interface = self.physics_system->GetBodyInterface();
      const auto* body = static_cast<const JPH::Body*>(rb.runtime_body);
      body_interface.RemoveBody(body->GetID());
      body_interface.DestroyBody(body->GetID());
      rb.runtime_body = nullptr;
    }
  });
  self.world.query_builder<CharacterControllerComponent>().build().each(
    [&self](const flecs::entity& e, CharacterControllerComponent& ch) {
      if (ch.character) {
        JPH::BodyInterface& body_interface = self.physics_system->GetBodyInterface();
        auto* character = reinterpret_cast<JPH::Character*>(ch.character);
        body_interface.RemoveBody(character->GetBodyID());
        ch.character = nullptr;
      }
    }
  );

  self.body_activation_listener_3d.reset();
  self.contact_listener_3d.reset();
}

auto Scene::runtime_start(this Scene& self) -> void {
  ZoneScoped;

  self.running = true;

  self.run_deferred_functions();

  self.physics_init();

  // Scripting
  for (auto& [uuid, system] : self.lua_systems) {
    system->on_scene_start(&self);
  }
}

auto Scene::runtime_stop(this Scene& self) -> void {
  ZoneScoped;

  self.running = false;

  self.physics_deinit();

  // Scripting
  for (auto& [uuid, system] : self.lua_systems) {
    system->on_scene_stop(&self);
  }
}

auto Scene::runtime_update(this Scene& self, const Timestep& delta_time) -> void {
  ZoneScoped;

  self.run_deferred_functions();

  auto pre_update_phase_enabled = !self.world.entity(flecs::PreUpdate).has(flecs::Disabled);
  auto on_update_phase_enabled = !self.world.entity(flecs::OnUpdate).has(flecs::Disabled);
  if (pre_update_phase_enabled && on_update_phase_enabled) {
    for (auto& [uuid, system] : self.lua_systems) {
      system->on_scene_update(&self, static_cast<f32>(delta_time.get_seconds()));
    }
  }

  // TODO: Pass our delta_time?
  self.world.progress();

  if (RendererCVar::cvar_enable_physics_debug_renderer.get()) {
    JPH::BodyManager::DrawSettings settings{};
    settings.mDrawShape = true;
    settings.mDrawShapeWireframe = true;

    self.physics_system->DrawBodies(settings, self.physics_debug_renderer.get());
  }

  if (self.renderer_instance) {
    auto& asset_man = App::mod<AssetManager>();
    auto meshlet_instance_visibility_offset = 0_u32;
    auto max_meshlet_instance_count = 0_u32;
    auto gpu_meshes = std::vector<GPU::Mesh>();
    auto gpu_mesh_instances = std::vector<GPU::MeshInstance>();

    if (self.meshes_dirty) {
      auto mesh_instances = self.mesh_instances.slots_unsafe();
      auto unique_mesh_to_gpu_mesh = ankerl::unordered_dense::map<std::pair<UUID, usize>, usize>();
      for (const auto& mesh_instance : mesh_instances) {
        const auto* model = asset_man.get_model(mesh_instance.model_uuid);
        const auto& mesh = model->gpu_meshes[mesh_instance.mesh_node_index];
        const auto* material_asset = asset_man.get_asset(mesh_instance.material_uuid);

        auto unique_mesh = std::pair(mesh_instance.model_uuid, mesh_instance.mesh_node_index);
        auto mesh_index = 0_u32;
        if (auto it = unique_mesh_to_gpu_mesh.find(unique_mesh); it != unique_mesh_to_gpu_mesh.end()) {
          mesh_index = it->second;
        } else {
          mesh_index = static_cast<u32>(gpu_meshes.size());
          gpu_meshes.emplace_back(mesh);
          unique_mesh_to_gpu_mesh.emplace(unique_mesh, mesh_index);
        }

        auto lod0_index = 0;
        const auto& lod0 = mesh.lods[lod0_index];

        auto& gpu_mesh_instance = gpu_mesh_instances.emplace_back();
        gpu_mesh_instance.mesh_index = mesh_index;
        gpu_mesh_instance.lod_index = lod0_index;
        gpu_mesh_instance.material_index = SlotMap_decode_id(material_asset->material_id).index;
        gpu_mesh_instance.transform_index = SlotMap_decode_id(mesh_instance.transform_id).index;
        gpu_mesh_instance.meshlet_instance_visibility_offset = meshlet_instance_visibility_offset;

        meshlet_instance_visibility_offset += lod0.meshlet_count;
        max_meshlet_instance_count += lod0.meshlet_count;
      }

      self.gpu_mesh_instance_count = gpu_mesh_instances.size();
      self.max_meshlet_instance_count = max_meshlet_instance_count;
    }

    auto uuid_to_image_index = [&](const UUID& uuid) -> option<u32> {
      if (!uuid || !asset_man.is_texture_loaded(uuid)) {
        return nullopt;
      }

      auto* texture = asset_man.get_texture(uuid);
      return texture->get_view_index();
    };

    if (self.force_material_update) {
      asset_man.set_all_materials_dirty();
      self.force_material_update = false;
    }

    auto dirty_material_ids = asset_man.get_dirty_material_ids();
    auto dirty_material_indices = std::vector<u32>();
    for (const auto dirty_id : dirty_material_ids) {
      const auto* material = asset_man.get_material(dirty_id);
      if (!material)
        continue;

      auto dirty_index = SlotMap_decode_id(dirty_id).index;
      dirty_material_indices.push_back(dirty_index);
      if (dirty_index >= self.gpu_materials.size()) {
        self.gpu_materials.resize(dirty_index + 1, {});
      }

      auto albedo_image_index = uuid_to_image_index(material->albedo_texture);
      auto normal_image_index = uuid_to_image_index(material->normal_texture);
      auto emissive_image_index = uuid_to_image_index(material->emissive_texture);
      auto metallic_roughness_image_index = uuid_to_image_index(material->metallic_roughness_texture);
      auto occlusion_image_index = uuid_to_image_index(material->occlusion_texture);
      auto sampler_index = 0_u32;

      auto flags = GPU::MaterialFlag::None;
      if (albedo_image_index.has_value()) {
        flags |= GPU::MaterialFlag::HasAlbedoImage;

        // Incase we wanted to change a material's sampler after it's creation
        // we should prefer material's sampler over texture's default sampler.
        auto& vk_context = App::get_vkcontext();

        auto* texture = asset_man.get_texture(material->albedo_texture);
        sampler_index = texture->get_sampler_index();

        auto texture_sampler = vk_context.resources.samplers.slot(texture->get_sampler_id());

        vuk::SamplerCreateInfo sampler_ci = {};
        switch (material->sampling_mode) {
          case SamplingMode::LinearRepeated          : sampler_ci = vuk::LinearSamplerRepeated; break;
          case SamplingMode::LinearClamped           : sampler_ci = vuk::LinearSamplerClamped; break;
          case SamplingMode::NearestRepeated         : sampler_ci = vuk::NearestSamplerRepeated; break;
          case SamplingMode::NearestClamped          : sampler_ci = vuk::NearestSamplerClamped; break;
          case SamplingMode::LinearRepeatedAnisotropy: sampler_ci = vuk::LinearSamplerRepeatedAnisotropy; break;
        }
        auto material_sampler = vk_context.runtime->acquire_sampler(sampler_ci, vk_context.num_frames);
        if (texture_sampler->id != material_sampler.id) {
          auto sampler_id = vk_context.allocate_sampler(sampler_ci);
          auto sampler_index_from_material = SlotMap_decode_id(sampler_id).index;
          sampler_index = sampler_index_from_material;
        }
      }

      flags |= normal_image_index.has_value() ? GPU::MaterialFlag::HasNormalImage : GPU::MaterialFlag::None;
      flags |= emissive_image_index.has_value() ? GPU::MaterialFlag::HasEmissiveImage : GPU::MaterialFlag::None;
      flags |= metallic_roughness_image_index.has_value() ? GPU::MaterialFlag::HasMetallicRoughnessImage
                                                          : GPU::MaterialFlag::None;
      flags |= occlusion_image_index.has_value() ? GPU::MaterialFlag::HasOcclusionImage : GPU::MaterialFlag::None;

      auto gpu_material = GPU::Material{
        .albedo_color = material->albedo_color,
        .emissive_color = material->emissive_color,
        .roughness_factor = material->roughness_factor,
        .metallic_factor = material->metallic_factor,
        .alpha_cutoff = material->alpha_cutoff,
        .flags = flags,
        .sampler_index = sampler_index,
        .albedo_image_index = albedo_image_index.value_or(0_u32),
        .normal_image_index = normal_image_index.value_or(0_u32),
        .emissive_image_index = emissive_image_index.value_or(0_u32),
        .metallic_roughness_image_index = metallic_roughness_image_index.value_or(0_u32),
        .occlusion_image_index = occlusion_image_index.value_or(0_u32),
        .uv_size = material->uv_size,
        .uv_offset = material->uv_offset,
      };

      self.gpu_materials[dirty_index] = gpu_material;
    }

    auto update_info = RendererInstanceUpdateInfo{
      .mesh_instance_count = self.gpu_mesh_instance_count,
      .max_meshlet_instance_count = self.max_meshlet_instance_count,
      .dirty_transform_ids = self.dirty_transforms,
      .gpu_transforms = self.transforms.slots_unsafe(),
      .dirty_material_indices = dirty_material_indices,
      .gpu_materials = self.gpu_materials,
      .gpu_meshes = gpu_meshes,
      .gpu_mesh_instances = gpu_mesh_instances,
    };
    self.renderer_instance->update(update_info);
  }
  self.dirty_transforms.clear();
  self.meshes_dirty = false;
}

auto Scene::get_lua_system(this const Scene& self, const UUID& lua_script) -> LuaSystem* {
  ZoneScoped;

  if (self.lua_systems.contains(lua_script)) {
    return self.lua_systems.at(lua_script);
  }

  return nullptr;
}

auto Scene::get_lua_systems(this const Scene& self) -> const ankerl::unordered_dense::map<UUID, LuaSystem*>& {
  ZoneScoped;

  return self.lua_systems;
}

auto Scene::add_lua_system(this Scene& self, const UUID& lua_script) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.get_asset(lua_script)->is_loaded()) {
    asset_man.load_asset(lua_script);
  }
  auto* script_system = asset_man.get_script(lua_script);

  script_system->reload();

  self.lua_systems.emplace(lua_script, script_system);

  script_system->on_add(&self);

  OX_LOG_TRACE("Added lua system to the scene {}", script_system->get_path());
}

auto Scene::remove_lua_system(this Scene& self, const UUID& lua_script) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto* script_system = asset_man.get_script(lua_script);

  script_system->on_remove(&self);

  OX_LOG_TRACE("Removed lua system from the scene {}", script_system->get_path());

  self.lua_systems.erase(lua_script);
}

auto Scene::get_physics_system(this const Scene& self) -> JPH::PhysicsSystem* {
  ZoneScoped;

  return self.physics_system.get();
}

auto Scene::cast_ray(this const Scene& self, const RayCast& ray_cast)
  -> JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector> {
  ZoneScoped;

  JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector> collector;
  const JPH::RayCast ray{math::to_jolt(ray_cast.get_origin()), math::to_jolt(ray_cast.get_direction())};
  self.physics_system->GetBroadPhaseQuery().CastRay(ray, collector);

  return collector;
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

  for (auto& [uuid, system] : lua_systems) {
    system->on_scene_render(this, extent, format);
  }
}

auto Scene::on_viewport_render(vuk::Extent3D extent, vuk::Format format) -> void {
  ZoneScoped;

  if (!is_running())
    return;

  for (auto& [uuid, system] : lua_systems) {
    system->on_viewport_render(this, extent, format);
  }
}

auto Scene::create_entity(const std::string& name, bool safe_naming) const -> flecs::entity {
  ZoneScoped;

  flecs::entity e = {};
  if (name.empty()) {
    e = safe_naming ? world.entity(safe_entity_name("entity").c_str()) : world.entity();
  } else {
    e = safe_naming ? world.entity(safe_entity_name(name).c_str()) : world.entity(name.c_str());
  }

  return e.add<TransformComponent>().add<LayerComponent>();
}

auto Scene::create_model_entity(this Scene& self, const UUID& asset_uuid) -> flecs::entity {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  // sanity check
  if (!asset_man.get_asset(asset_uuid)) {
    OX_LOG_ERROR("Cannot import an invalid model '{}' into the scene!", asset_uuid.str());
    return {};
  }

  // acquire model
  if (!asset_man.load_model(asset_uuid)) {
    return {};
  }

  auto* model = asset_man.get_model(asset_uuid);
  auto& default_scene = model->scenes[model->default_scene_index];
  auto& root_node = model->mesh_groups.front();
  auto root_entity = self.create_entity(default_scene.name, default_scene.name.empty() ? false : true);

  struct ProcessingNode {
    flecs::entity parent = {};
    usize mesh_group_index = 0;
  };

  auto processing_nodes = std::stack<ProcessingNode>();
  for (const auto child_index : root_node.child_indices) {
    processing_nodes.push({root_entity, child_index});
  }

  while (!processing_nodes.empty()) {
    const auto [parent_entity, mesh_group_index] = processing_nodes.top();
    const auto& mesh_group = model->mesh_groups[mesh_group_index];
    processing_nodes.pop();

    auto node_entity = self.create_entity(mesh_group.name);
    node_entity.set<TransformComponent>({
      .position = mesh_group.translation,
      .rotation = mesh_group.rotation,
      .scale = mesh_group.scale,
    });
    node_entity.child_of(parent_entity);
    node_entity.modified<TransformComponent>();

    for (const auto mesh_index : mesh_group.mesh_indices) {
      memory::ScopedStack stack;
      auto mesh_entity_name = !mesh_group.name.empty() ? stack.format("{} Mesh {}", mesh_group.name, mesh_index) : "";
      auto mesh_entity = self.create_entity(std::string(mesh_entity_name));
      mesh_entity.set<TransformComponent>({});
      mesh_entity.set<MeshComponent>({
        .model_uuid = asset_uuid,
        .mesh_index = static_cast<u32>(mesh_index),
        .material_uuid = model->initial_materials[mesh_index],
      });
      mesh_entity.child_of(node_entity);
      mesh_entity.modified<TransformComponent>();

      // if (mesh_group.light_index.has_value()) {
      //   auto& node_light = model->lights[cur_node.light_index.value()];
      //   auto lc = LightComponent{
      //     .type = static_cast<LightComponent::LightType>(node_light.type),
      //     .color = node_light.color,
      //     .intensity = node_light.intensity,
      //   };

      //   if (node_light.range.has_value()) {
      //     lc.radius = *node_light.range;
      //   }
      //   if (node_light.inner_cone_angle.has_value()) {
      //     lc.inner_cone_angle = *node_light.inner_cone_angle;
      //   }
      //   if (node_light.outer_cone_angle.has_value()) {
      //     lc.inner_cone_angle = *node_light.inner_cone_angle;
      //   }

      //   node_entity.set<LightComponent>(lc);
      // }
    }

    for (const auto child_node_indices : mesh_group.child_indices) {
      processing_nodes.push({node_entity, child_node_indices});
    }
  }

  return root_entity;
}

auto Scene::get_world_position(const flecs::entity entity) -> glm::vec3 {
  const auto& tc = entity.get<TransformComponent>();
  const auto parent = entity.parent();
  if (parent != flecs::entity::null()) {
    const glm::vec3 parent_position = get_world_position(parent);
    const auto& parent_tc = parent.get<TransformComponent>();
    const glm::quat parent_rotation = parent_tc.rotation;
    const glm::vec3 rotated_scaled_pos = parent_rotation * (parent_tc.scale * tc.position);
    return parent_position + rotated_scaled_pos;
  }
  return tc.position;
}

auto Scene::get_world_transform(const flecs::entity entity) -> glm::mat4 {
  const auto& tc = entity.get<TransformComponent>();
  const auto parent = entity.parent();
  const glm::mat4 parent_transform = parent != flecs::entity::null() ? get_world_transform(parent) : glm::mat4(1.0f);
  return parent_transform * glm::translate(glm::mat4(1.0f), tc.position) * glm::mat4_cast(tc.rotation) *
         glm::scale(glm::mat4(1.0f), tc.scale);
}

auto Scene::get_local_transform(flecs::entity entity) -> glm::mat4 {
  const auto& tc = entity.get<TransformComponent>();
  return glm::translate(glm::mat4(1.0f), tc.position) * glm::mat4_cast(tc.rotation) *
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
  self.transform_index_entities_map.emplace(SlotMap_decode_id(id).index, entity);

  return id;
}

auto Scene::remove_transform(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto it = self.entity_transforms_map.find(entity);
  if (it == self.entity_transforms_map.end()) {
    return;
  }

  self.transform_index_entities_map.erase(SlotMap_decode_id(it->second).index);
  self.transforms.destroy_slot(it->second);
  self.entity_transforms_map.erase(it);
}

auto Scene::attach_mesh(
  this Scene& self, flecs::entity entity, const UUID& model_uuid, usize mesh_index, const UUID& material_uuid
) -> bool {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto transforms_it = self.entity_transforms_map.find(entity);
  if (!self.entity_transforms_map.contains(entity)) {
    OX_LOG_FATAL("Target entity must have a transform component!");
    return false;
  }

  const auto transform_id = transforms_it->second;

  // Find the old model UUID and detach it from entity.
  auto mesh_instances_it = self.entity_to_mesh_instance_map.find(entity);
  if (mesh_instances_it != self.entity_to_mesh_instance_map.end()) {
    const auto old_mesh_instance_id = mesh_instances_it->second;
    self.mesh_instances.destroy_slot(old_mesh_instance_id);
    self.meshes_dirty = true;
  }

  auto overriden_material = material_uuid;
  if (!material_uuid) {
    // No material override, use original one
    auto* model = asset_man.get_model(model_uuid);
    if (mesh_index >= model->initial_materials.size()) {
      // This should not happen because meshes and initial materials
      // are inserted the same way
      OX_DEBUGBREAK();
      return false;
    }
    overriden_material = model->initial_materials[mesh_index];
  }

  auto instance_id = self.mesh_instances.create_slot(
    MeshInstance{
      .model_uuid = model_uuid,
      .mesh_node_index = mesh_index,
      .material_uuid = overriden_material,
      .transform_id = transform_id,
    }
  );
  self.entity_to_mesh_instance_map.emplace(entity, instance_id);
  self.meshes_dirty = true;
  self.set_dirty(entity);

  return true;
}

auto Scene::detach_mesh(this Scene& self, flecs::entity entity) -> bool {
  ZoneScoped;

  auto instances_it = self.entity_to_mesh_instance_map.find(entity);
  auto transforms_it = self.entity_transforms_map.find(entity);
  if (instances_it == self.entity_to_mesh_instance_map.end() || transforms_it == self.entity_transforms_map.end()) {
    return false;
  }

  const auto transform_id = transforms_it->second;
  const auto instance_id = instances_it->second;
  if (!self.mesh_instances.slot(instance_id)) {
    return false;
  }

  self.mesh_instances.destroy_slot(instance_id);
  self.meshes_dirty = true;

  return true;
}

auto Scene::on_contact_added(
  const JPH::Body& body1,
  const JPH::Body& body2,
  const JPH::ContactManifold& manifold,
  const JPH::ContactSettings& settings
) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [uuid, system] : lua_systems) {
    system->on_contact_added(this, body1, body2, manifold, settings);
  }
}

auto Scene::on_contact_persisted(
  const JPH::Body& body1,
  const JPH::Body& body2,
  const JPH::ContactManifold& manifold,
  const JPH::ContactSettings& settings
) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [uuid, system] : lua_systems) {
    system->on_contact_persisted(this, body1, body2, manifold, settings);
  }
}

auto Scene::on_contact_removed(const JPH::SubShapeIDPair& sub_shape_pair) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [uuid, system] : lua_systems) {
    system->on_contact_removed(this, sub_shape_pair);
  }
}

auto Scene::on_body_activated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [uuid, system] : lua_systems) {
    system->on_body_activated(this, body_id, (u64)body_user_data);
  }
}

auto Scene::on_body_deactivated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [uuid, system] : lua_systems) {
    system->on_body_deactivated(this, body_id, (u64)body_user_data);
  }
}

auto Scene::create_rigidbody(
  this Scene& self, flecs::entity entity, const TransformComponent& transform, RigidBodyComponent& component
) -> void {
  ZoneScoped;

  auto& body_interface = self.physics_system->GetBodyInterface();
  if (component.runtime_body) {
    auto body_id = static_cast<JPH::Body*>(component.runtime_body)->GetID();
    body_interface.RemoveBody(body_id);
    body_interface.DestroyBody(body_id);
    component.runtime_body = nullptr;
  }

  JPH::MutableCompoundShapeSettings compound_shape_settings = {};
  float max_scale_component = glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);

  const auto entity_name = std::string(entity.name());

  JPH::ShapeSettings::ShapeResult shape_result = {};
  glm::vec3 offset = {};

  if (const auto* bc = entity.try_get<BoxColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), bc->friction, bc->restitution);

    glm::vec3 scale = bc->size;
    JPH::BoxShapeSettings shape_settings({glm::abs(scale.x), glm::abs(scale.y), glm::abs(scale.z)}, 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, bc->density));
    shape_result = shape_settings.Create();
    offset = bc->offset;
  } else if (const auto* scc = entity.try_get<SphereColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), scc->friction, scc->restitution);

    float radius = 2.0f * scc->radius * max_scale_component;
    JPH::SphereShapeSettings shape_settings(glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, scc->density));
    shape_result = shape_settings.Create();
    offset = scc->offset;
  } else if (const auto* ccc = entity.try_get<CapsuleColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), ccc->friction, ccc->restitution);

    float radius = 2.0f * ccc->radius * max_scale_component;
    JPH::CapsuleShapeSettings shape_settings(glm::max(0.01f, ccc->height) * 0.5f, glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, ccc->density));
    shape_result = shape_settings.Create();
    offset = ccc->offset;
  } else if (const auto* tcc = entity.try_get<TaperedCapsuleColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), tcc->friction, tcc->restitution);

    float top_radius = 2.0f * tcc->top_radius * max_scale_component;
    float bottom_radius = 2.0f * tcc->bottom_radius * max_scale_component;
    JPH::TaperedCapsuleShapeSettings shape_settings(
      glm::max(0.01f, tcc->height) * 0.5f,
      glm::max(0.01f, top_radius),
      glm::max(0.01f, bottom_radius),
      mat
    );
    shape_settings.SetDensity(glm::max(0.001f, tcc->density));
    shape_result = shape_settings.Create();
    offset = tcc->offset;
  } else if (const auto* cycc = entity.try_get<CylinderColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), cycc->friction, cycc->restitution);

    float radius = 2.0f * cycc->radius * max_scale_component;
    JPH::CylinderShapeSettings
      shape_settings(glm::max(0.01f, cycc->height) * 0.5f, glm::max(0.01f, radius), 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, cycc->density));
    shape_result = shape_settings.Create();
    offset = cycc->offset;
  }

  if (shape_result.HasError()) {
    OX_LOG_ERROR("Jolt shape error: {}", shape_result.GetError().c_str());
  }

  if (!shape_result.IsEmpty()) {
    compound_shape_settings.AddShape({offset.x, offset.y, offset.z}, JPH::Quat::sIdentity(), shape_result.Get());
  } else {
    return; // No Shape
  }

  // Body
  auto rotation = glm::quat(transform.rotation);

  u16 layer_index = 1; // Default Layer
  if (const auto* layer_component = entity.try_get<LayerComponent>()) {
    layer_index = layer_component->layer;
  }

  auto compound_shape = compound_shape_settings.Create();
  if (compound_shape.HasError()) {
    OX_LOG_ERROR("Jolt shape error: {}", compound_shape.GetError().c_str());
  }

  JPH::BodyCreationSettings body_settings(
    compound_shape.Get(),
    {transform.position.x, transform.position.y, transform.position.z},
    {rotation.x, rotation.y, rotation.z, rotation.w},
    static_cast<JPH::EMotionType>(component.type),
    layer_index
  );

  JPH::MassProperties mass_properties;
  mass_properties.mMass = glm::max(0.01f, component.mass);
  body_settings.mMassPropertiesOverride = mass_properties;
  body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  body_settings.mAllowSleeping = component.allow_sleep;
  body_settings.mLinearDamping = glm::max(0.0f, component.linear_drag);
  body_settings.mAngularDamping = glm::max(0.0f, component.angular_drag);
  body_settings.mMotionQuality = component.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
  body_settings.mGravityFactor = component.gravity_factor;
  body_settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(component.allowed_dofs);
  body_settings.mFriction = component.friction;
  body_settings.mRestitution = component.restitution;

  body_settings.mIsSensor = component.is_sensor;

  JPH::Body* body = body_interface.CreateBody(body_settings);

  OX_CHECK_NULL(body, "Jolt is out of bodies!");

  JPH::EActivation activation = component.awake && component.type != RigidBodyComponent::BodyType::Static
                                  ? JPH::EActivation::Activate
                                  : JPH::EActivation::DontActivate;
  body_interface.AddBody(body->GetID(), activation);

  body->SetUserData(static_cast<u64>(entity.id()));

  component.runtime_body = body;
}

void Scene::create_character_controller(
  flecs::entity entity, const TransformComponent& transform, CharacterControllerComponent& component
) const {
  ZoneScoped;

  const auto position = JPH::Vec3(transform.position.x, transform.position.y, transform.position.z);
  const auto capsule_shape =
    JPH::RotatedTranslatedShapeSettings(
      JPH::Vec3(0, 0.5f * component.character_height_standing + component.character_radius_standing, 0),
      JPH::Quat::sIdentity(),
      new JPH::CapsuleShape(0.5f * component.character_height_standing, component.character_radius_standing)
    )
      .Create()
      .Get();

  // Create character
  const std::shared_ptr<JPH::CharacterSettings> settings = std::make_shared<JPH::CharacterSettings>();
  settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
  settings->mLayer = PhysicsLayers::MOVING;
  settings->mShape = capsule_shape;
  settings->mFriction = 0.0f; // For now this is not set.
  settings->mSupportingVolume = JPH::Plane(
    JPH::Vec3::sAxisY(),
    -component.character_radius_standing
  ); // Accept contacts that touch the
     // lower sphere of the capsule

  component.character = new JPH::Character(settings.get(), position, JPH::Quat::sIdentity(), 0, physics_system.get());

  auto* ch = reinterpret_cast<JPH::Character*>(component.character);
  ch->AddToPhysicsSystem(JPH::EActivation::Activate);

  auto ch_body = physics_system->GetBodyLockInterface().TryGetBody(ch->GetBodyID());
  ch_body->SetUserData(static_cast<u64>(entity.id()));
}

auto Scene::entity_to_json(JsonWriter& writer, flecs::entity e) -> void {
  ZoneScoped;

  auto world = e.world();
  writer.begin_obj();
  writer["name"] = e.name();
  writer["tags"].begin_array();
  auto components = std::vector<flecs::entity>{};
  e.each([&](flecs::id component_id) {
    if (!component_id.is_entity()) {
      return;
    }

    auto ty = component_id.entity();
    if (ty.has<flecs::Component>()) {
      components.push_back(ty);
    } else {
      writer << ty.path();
    }
  });
  writer.end_array();

  writer["components"].begin_array();
  for (auto& component : components) {
    auto* component_data = e.get_mut(component.id());

    writer.begin_obj();
    writer.key(component.path().c_str());
    writer.begin_obj();
    auto serializer = JsonEntitySerializer(world, writer);
    serializer.serialize(component, component_data);
    writer.end_obj();
    writer.end_obj();
  }
  writer.end_array();

  writer["children"].begin_array();
  e.children([&writer](flecs::entity c) { entity_to_json(writer, c); });
  writer.end_array();

  writer.end_obj();
}

auto Scene::json_to_entity(
  Scene& self, flecs::entity root, simdjson::ondemand::value& json, std::vector<UUID>& requested_assets
) -> flecs::entity {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto& world = self.world;

  auto entity_name_json = json["name"];
  if (entity_name_json.error()) {
    OX_LOG_ERROR("Entities must have names!");
    return flecs::entity::null();
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
    auto component_obj_json = component_json.get_object();
    for (auto field_json : component_obj_json) {
      auto component_name_json = field_json.unescaped_key();
      if (component_name_json.error()) {
        OX_LOG_ERROR("Entity '{}' has corrupt components JSON array.", e.name().c_str());
        return flecs::entity::null();
      }

      const auto* component_name = stack.null_terminate_cstr(component_name_json.value_unsafe());
      auto component_id = world.lookup(component_name);
      if (!component_id) {
        OX_LOG_ERROR("Entity '{}' has invalid component named '{}'!", e.name().c_str(), component_name);
        return flecs::entity::null();
      }

      if (!self.component_db.is_component_known(component_id)) {
        OX_LOG_WARN("Skipping unkown component {}:{}", component_name, (u64)component_id);
        continue;
      }

      e.add(component_id);
      auto* component = e.get_mut(component_id);
      auto deserializer = JsonEntityDeserializer(self.world, field_json.value());
      deserializer.serialize(component_id, component);
      requested_assets.insert_range(requested_assets.end(), std::move(deserializer.requested_assets));
      e.modified(component_id);
    }
  }

  auto children_json = json["children"];
  for (auto children : children_json.get_array()) {
    if (children.error()) {
      continue;
    }

    if (json_to_entity(self, e, children.value_unsafe(), requested_assets) == flecs::entity::null()) {
      return flecs::entity::null();
    }
  }

  return e;
}

auto Scene::to_json(this const Scene& self) -> JsonWriter {
  JsonWriter writer{};

  writer.begin_obj();

  writer["name"] = self.scene_name;

  writer["scripts"].begin_array();
  for (auto& [uuid, system] : self.lua_systems) {
    writer.begin_obj();
    writer["uuid"] = uuid.str();
    writer.end_obj();
  }
  writer.end_array();

  writer["entities"].begin_array();
  const auto q = self.world.query_builder().with<TransformComponent>().build();
  q.each([&writer](flecs::entity e) {
    if (e.parent() == flecs::entity::null() && !e.has<Hidden>()) {
      entity_to_json(writer, e);
    }
  });
  writer.end_array();

  writer.end_obj();

  return writer;
}

auto Scene::copy(const std::shared_ptr<Scene>& src_scene) -> std::shared_ptr<Scene> {
  ZoneScoped;

  // Copies the world but not the renderer instance.

  auto new_name = fmt::format("{}_copy", src_scene->scene_name);
  std::shared_ptr<Scene> new_scene = std::make_shared<Scene>(new_name);

  auto writer = src_scene->to_json();
  new_scene->from_json(writer.stream.str());
  new_scene->scene_name = new_name;
  new_scene->meshes_dirty = true;

  OX_LOG_TRACE("Copied scene {} to {}", src_scene->scene_name, new_scene->scene_name);

  return new_scene;
}

auto Scene::from_json(this Scene& self, const std::string& json) -> bool {
  auto content = simdjson::padded_string(json);
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(content);
  if (doc.error()) {
    OX_LOG_ERROR("Failed to parse scene! {}", simdjson::error_message(doc.error()));
    return false;
  }

  auto name_json = doc["name"];
  if (name_json.error()) {
    OX_LOG_ERROR("Scenes must have names!");
    return false;
  }

  self.scene_name = name_json.get_string().value_unsafe();

  std::vector<UUID> requested_assets = {};

  auto scripts_array = doc["scripts"];
  if (!scripts_array.error()) {
    for (auto script_json : scripts_array.get_array()) {
      auto uuid_json = script_json.value_unsafe();
      auto uuid_str = uuid_json["uuid"].get_string();
      if (!uuid_str.error()) {
        auto script_uuid = UUID::from_string(uuid_str.value_unsafe()).value();
        requested_assets.emplace_back(script_uuid);
      }
    }
  } else {
    OX_LOG_ERROR("No scripts field found in scene!");
  }

  auto entities_array = doc["entities"];
  if (!entities_array.error()) {
    for (auto entity_json : entities_array.get_array()) {
      if (Scene::json_to_entity(self, flecs::entity::null(), entity_json.value_unsafe(), requested_assets) ==
          flecs::entity::null()) {
        return false;
      }
    }
  } else {
    OX_LOG_ERROR("No entities field found in scene!");
    return false;
  }

  OX_LOG_INFO("Loading scene {} with {} assets...", self.scene_name, requested_assets.size());

  for (const auto& uuid : requested_assets) {
    auto& asset_man = App::mod<AssetManager>();
    if (auto asset = asset_man.get_asset(uuid); asset) {
      if (asset->type == AssetType::Script) {
        self.add_lua_system(uuid);
      } else {
        asset_man.load_asset(uuid);
      }
    } else {
      // Not an imported/physical asset
      // Most likely was created on runtime and never written to a file, these should never exist.
      // Otherwise component will be left with an unloaded asset.
      OX_LOG_WARN("Ghost asset found! {}", uuid.str());
    }
  }

  return true;
}

auto Scene::save_to_file(this const Scene& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto writer = self.to_json();

  std::ofstream filestream(path);
  filestream << writer.stream.rdbuf();

  OX_LOG_INFO("Saved scene: {} to {}.", self.scene_name, path);

  return true;
}

auto Scene::load_from_file(this Scene& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto content = File::to_string(path);
  if (content.empty()) {
    OX_LOG_ERROR("Failed to read/open file {}!", path);
    return false;
  }

  return self.from_json(content);
}

auto Scene::reset_renderer_instance() -> void {
  auto& renderer = App::mod<Renderer>();
  renderer_instance = renderer.new_instance(*this);
}
} // namespace ox
