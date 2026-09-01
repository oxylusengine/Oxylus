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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
// clang-format on
#include <RmlUi/Core.h>
#include <algorithm>
#include <glm/gtx/compatibility.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <simdjson.h>
#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Physics/Physics.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Physics/PhysicsMaterial.hpp"
#include "Render/Camera.hpp"
#include "Scene/EntitySerializer.hpp"
#include "Scripting/LuaManager.hpp"
#include "UI/RmlUI.hpp"
#include "UI/RmlView.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"
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

  auto on_enum(std::string_view name, ecs_meta_op_kind_t underlying_kind, flecs::entity_t type, void* ptr)
    -> void override {
    ZoneScoped;

    auto field_result = json_value[name];
    if (field_result.error() || !ptr) {
      return;
    }

    if (
      underlying_kind == EcsOpU8 || underlying_kind == EcsOpU16 || underlying_kind == EcsOpU32 ||
      underlying_kind == EcsOpU64
    ) {
      auto result = field_result.get_uint64();
      auto current = static_cast<u64*>(ptr);
      *current = result.value_unsafe();
    } else if (
      underlying_kind == EcsOpI8 || underlying_kind == EcsOpI16 || underlying_kind == EcsOpI32 ||
      underlying_kind == EcsOpI64
    ) {
      auto result = field_result.get_int64();
      auto current = static_cast<i64*>(ptr);
      *current = result.value_unsafe();
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
    } else if (
      opaque_type == flecs::U16 || opaque_type == flecs::U32 || opaque_type == flecs::U64 || opaque_type == flecs::Uptr
    ) {
      auto result = field_result.get_uint64();
      if (!result.error() && opaque_info->assign_uint) {
        opaque_info->assign_uint(field_ptr, result.value_unsafe());
      }
    } else if (
      opaque_type == flecs::I8 || opaque_type == flecs::I16 || opaque_type == flecs::I32 || opaque_type == flecs::I64 ||
      opaque_type == flecs::Iptr
    ) {
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

// Physics reports world space; TransformComponent is local to the parent. Bodies on child entities
// need the parent folded back out or the parent's transform is applied twice.
auto world_pose_to_local(flecs::entity entity, glm::vec3& position, glm::quat& rotation) -> void {
  const auto parent = entity.parent();
  if (parent == flecs::entity::null() || !parent.has<TransformComponent>()) {
    return;
  }

  const auto parent_world = Scene::get_world_transform(parent);
  const auto local = glm::inverse(parent_world) * glm::translate(glm::mat4(1.f), position) * glm::mat4_cast(rotation);

  auto scale = glm::vec3{};
  auto skew = glm::vec3{};
  auto perspective = glm::vec4{};
  glm::decompose(local, scale, rotation, position, skew, perspective);
}

auto Scene::safe_entity_name(this const Scene& self, std::string prefix, flecs::entity parent) -> std::string {
  ZoneScoped;

  // A name must be free BOTH at the world root (where `world.entity(name)` will
  // initially create it) AND under `parent`'s child scope (where it lands after
  // `child_of(parent)` reparents it). flecs keeps a per-scope name index, so a
  // name that's free at the root can still cause `flecs_reparent_name_index` to
  // abort when the parent already has a child by that name.
  auto name_exists = [&](const char* name) -> bool {
    if (parent.is_valid()) {
      if (parent.lookup(name) != 0) {
        return true;
      }
    }
    return self.world.lookup(name) > 0;
  };

  // Fast path: prefix itself is free.
  if (!name_exists(prefix.data())) {
    return prefix;
  }

  // Detect a Blender-style ".NNN" suffix (e.g. "leaf.001", "Cube.042"). If the
  // original name already exists, we increment the numeric portion so that
  // duplicated exports get sensible successors ("leaf.001" -> "leaf.002" ->
  // "leaf.003") instead of the uglier "leaf.001_1" / "leaf.001_2" the old
  // logic produced. Names without such a suffix fall back to "_1", "_2", ...
  auto try_parse_blender_suffix =
    [](const std::string& s, std::string& out_base, u32& out_num, usize& out_width) -> bool {
    const auto dot = s.rfind('.');
    if (dot == std::string::npos || dot == 0) {
      return false;
    }
    const auto suffix = std::string_view{s}.substr(dot + 1);
    if (suffix.empty() || suffix.size() > 9) {
      return false;
    }
    for (char c : suffix) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    out_base = s.substr(0, dot);
    out_num = static_cast<u32>(std::stoul(std::string{suffix}));
    out_width = suffix.size();
    return true;
  };

  auto new_entity_name = prefix;

  std::string base;
  u32 num = 0;
  usize width = 0;
  const bool is_blender_style = try_parse_blender_suffix(prefix, base, num, width);

  if (is_blender_style) {
    do {
      num += 1;
      new_entity_name = fmt::format("{}.{:0{}}", base, num, width);
    } while (name_exists(new_entity_name.data()));
  } else {
    u32 index = 0;
    do {
      index += 1;
      new_entity_name = fmt::format("{}_{}", prefix, index);
    } while (name_exists(new_entity_name.data()));
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

  tearing_down = true;

  {
    auto& asset_man = App::mod<AssetManager>();
    for (const auto& [entity, emitter_id] : entity_particle_emitters_map) {
      if (const auto* state = particle_emitters.slot(emitter_id); state && state->asset) {
        asset_man.unload_asset(state->asset);
      }
    }
    entity_particle_emitters_map.clear();
    particle_emitters.reset();
  }

  destroy_terrain_collision();
  set_terrain_edits_ref({});
  terrain.reset();

  for (auto& [_, system] : lua_systems) {
    system->on_remove(this);
  }

  lua_systems.clear();
  rml_view.reset();
  auto& lua_manager = App::mod<LuaManager>();
  lua_manager.get_state()->collect_gc();
}

auto Scene::init(this Scene& self, const std::string& name) -> void {
  ZoneScoped;

  self.uuid = UUID::generate_random();

  self.scene_name = name;

  self.component_db.import_module(self.world.import<CoreComponentsModule>());

  if (App::has_mod<Renderer>()) {
    auto& renderer = App::mod<Renderer>();
    self.renderer_instance = renderer.new_instance(self);
  }

  if (App::has_mod<RmlUI>()) {
    self.rml_view = std::make_unique<RmlView>(fmt::format("scene_{}", self.uuid.str()));
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
    .event(flecs::OnSet)
    .each([&self](flecs::iter& it, usize i, TransformComponent&, MeshComponent& mc) {
      auto entity = it.entity(i);
      self.set_dirty(entity);

      if (mc.model_uuid)
        self.attach_mesh(entity, mc.model_uuid, mc.mesh_index, mc.material_uuid);

      if (auto id = self.get_entity_transform_id(entity)) {
        if (auto* transform = self.get_entity_transform(*id)) {
          mc.world_aabb = mc.baked_aabb.get_transformed(transform->world);
        }
      }
    });

  self.world.observer<TransformComponent, MeshComponent>()
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, TransformComponent&, MeshComponent& mc) {
      if (mc.model_uuid) {
        self.detach_mesh(it.entity(i));
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

  self.world.observer<TerrainComponent>()
    .event(flecs::OnAdd)
    .event(flecs::OnSet)
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, TerrainComponent& c) {
      const auto entity = it.entity(i);

      if (it.event() == flecs::OnRemove) {
        // ~Scene already did this, and the members it touches are gone by the time the world's own
        // teardown gets here.
        if (self.tearing_down) {
          return;
        }

        if (self.terrain_entity == entity) {
          self.destroy_terrain_collision();
          self.terrain.reset();
          self.terrain_entity = {};
          self.terrain_dirty = false;
          self.set_terrain_edits_ref({});
        }
        return;
      }

      if (it.event() == flecs::OnAdd && self.deserializing_entity) {
        return;
      }

      if (self.terrain_entity && self.terrain_entity != entity) {
        OX_LOG_WARN("Scene already has a terrain; ignoring the one on entity '{}'.", entity.name().c_str());
        return;
      }

      self.terrain_entity = entity;
      self.terrain_dirty = true;
      self.set_terrain_edits_ref(c.terrain_edits);
    });

  self.world.observer<SpriteComponent>()
    .event(flecs::OnAdd)
    .event(flecs::OnSet)
    .each([&self](flecs::iter& it, usize i, SpriteComponent& c) {
      // Deserialization adds the component first and writes its fields (including the material
      // UUID) right after, so creating a material on OnAdd would orphan it. Wait for the OnSet
      // that follows instead; it only gets here if the entity really came without a material.
      if (it.event() == flecs::OnAdd && self.deserializing_entity) {
        return;
      }

      if (c.material) {
        return;
      }

      auto& asset_man = App::mod<AssetManager>();
      c.material = asset_man.create_asset(AssetType::Material, {});
      asset_man.load_asset(c.material);
    });

  self.world.observer<SpriteComponent>().event(flecs::OnRemove).each([](flecs::iter& it, usize i, SpriteComponent& c) {
    auto& asset_man = App::mod<AssetManager>();
    if (it.event() == flecs::OnRemove) {
      // Must not hold a registry read guard across unload_asset(): it takes the registry
      // write lock (self-deadlock otherwise). unload_asset() no-ops on missing/unloaded.
      asset_man.unload_asset(c.material);
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
      auto audio_asset = asset_man.get_audio(c.audio_source);
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

  self.world.observer<MeshComponent>().event(flecs::OnRemove).each([](flecs::iter& it, usize i, MeshComponent& c) {
    auto& asset_man = App::mod<AssetManager>();
    asset_man.unload_asset(c.model_uuid);
  });

  self.world.observer<AudioSourceComponent>()
    .event(flecs::OnRemove)
    .each([](flecs::iter& it, usize i, AudioSourceComponent& c) {
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
        self.create_rigidbody(it.entity(i), rb);
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
        self.create_character_controller(it.entity(i), ch);
      } else if (it.event() == flecs::OnRemove) {
        if (ch.character) {
          auto* character = reinterpret_cast<JPH::Character*>(ch.character);
          character->RemoveFromPhysicsSystem();
          character->Release();
          ch.character = nullptr;
        }
      }
    });

  self.world.observer<VehicleComponent>()
    .event(flecs::OnSet)
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, VehicleComponent& vehicle) {
      ZoneScopedN("Vehicle observer");

      if (!self.is_running())
        return;

      if (it.event() == flecs::OnSet) {
        self.create_vehicle(it.entity(i), vehicle);
      } else if (it.event() == flecs::OnRemove) {
        self.destroy_vehicle(vehicle);
      }
    });

  self.world.observer<ParticleSystemComponent>()
    .event(flecs::OnAdd)
    .event(flecs::OnSet)
    .event(flecs::OnRemove)
    .each([&self](flecs::iter& it, usize i, ParticleSystemComponent& c) {
      ZoneScopedN("Particle system observer");

      auto& asset_man = App::mod<AssetManager>();
      const auto entity = it.entity(i);

      if (it.event() == flecs::OnRemove) {
        if (self.tearing_down) {
          return;
        }

        const auto emitter_it = self.entity_particle_emitters_map.find(entity);
        if (emitter_it == self.entity_particle_emitters_map.end()) {
          return;
        }

        auto asset = UUID{};
        if (const auto* state = self.particle_emitters.slot(emitter_it->second)) {
          asset = state->asset;
        }

        self.particle_emitters.destroy_slot(emitter_it->second);
        self.entity_particle_emitters_map.erase(emitter_it);

        if (asset) {
          asset_man.unload_asset(asset);
        }

        return;
      }

      auto [emitter_it, inserted] = self.entity_particle_emitters_map.try_emplace(entity, ParticleEmitterID::Invalid);
      if (inserted) {
        emitter_it->second = self.particle_emitters.create_slot(ParticleEmitterState{});
      }

      auto* state = self.particle_emitters.slot(emitter_it->second);
      if (!state) {
        return;
      }

      // whoever writes the uuid owns the ref, the same way MeshComponent and AudioSourceComponent
      // do. acquiring here as well would double count every scene load, since `from_json` already
      // acquires every deserialized asset uuid
      if (state->asset != c.particle_system) {
        state->asset = c.particle_system;
        state->pool_valid = false;
        state->time = 0.0f;
        state->spawn_accumulator = 0.0f;
        state->program_state = {};
      }

      if (it.event() == flecs::OnAdd) {
        state->playing = c.play_on_awake;
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
      if (auto audio = asset_man.get_audio(ac.audio_source)) {
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

  const auto physics_tick_source = self.world.timer().interval(self.physics_interval);

  // Declared before physics_step so the controller sees this tick's input. Systems in a phase run in
  // declaration order.
  self.world.system<VehicleComponent>("vehicle_input")
    .kind(flecs::OnUpdate)
    .tick_source(physics_tick_source)
    .each([](VehicleComponent& vehicle) {
      if (!vehicle.runtime_constraint)
        return;

      auto* constraint = static_cast<JPH::VehicleConstraint*>(vehicle.runtime_constraint);
      auto* controller = static_cast<JPH::WheeledVehicleController*>(constraint->GetController());
      controller
        ->SetDriverInput(vehicle.input_forward, vehicle.input_right, vehicle.input_brake, vehicle.input_hand_brake);

      // Jolt puts the body to sleep on its own, and a sleeping chassis ignores driver input.
      if (
        vehicle.input_forward != 0.f || vehicle.input_right != 0.f || vehicle.input_brake != 0.f ||
        vehicle.input_hand_brake != 0.f
      ) {
        constraint->GetVehicleBody()->GetMotionProperties()->SetLinearVelocity(
          constraint->GetVehicleBody()->GetLinearVelocity()
        );
      }
    });

  self.world.system("physics_step")
    .kind(flecs::OnUpdate)
    .tick_source(physics_tick_source)
    .run([&self](flecs::iter& it) {
      OX_CHECK_NULL(self.physics_system);
      auto& p = App::mod<Physics>();
      self.physics_system->Update(self.physics_interval, 1, p.get_temp_allocator(), p.get_job_system());
    });

  // Drives the wheel child entities from the constraint so wheel meshes spin and steer.
  self.world.system<TransformComponent, const VehicleWheelComponent>("vehicle_wheel_update")
    .kind(flecs::OnUpdate)
    .tick_source(physics_tick_source)
    .each([](const flecs::entity& e, TransformComponent& tc, const VehicleWheelComponent& wheel) {
      auto parent = e.parent();
      if (!parent || !parent.has<VehicleComponent>())
        return;

      const auto& vehicle = parent.get<VehicleComponent>();
      if (!vehicle.runtime_constraint)
        return;

      auto* constraint = static_cast<JPH::VehicleConstraint*>(vehicle.runtime_constraint);
      if (wheel.runtime_wheel_index >= constraint->GetWheels().size())
        return;

      // Jolt reports the wheel in world space, but the child transform is relative to the chassis.
      const auto wheel_world = constraint->GetWheelWorldTransform(
        wheel.runtime_wheel_index,
        JPH::Vec3::sAxisX(),
        JPH::Vec3::sAxisY()
      );
      const auto chassis_world = constraint->GetVehicleBody()->GetWorldTransform();
      const auto local = chassis_world.InversedRotationTranslation() * wheel_world;

      const auto position = local.GetTranslation();
      const auto rotation = local.GetQuaternion().Normalized();
      tc.position = {position.GetX(), position.GetY(), position.GetZ()};
      tc.rotation = glm::quat::wxyz(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());

      e.modified<TransformComponent>();
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
    });

  self.world.system<TransformComponent, const RigidBodyComponent>("physics_interpolate")
    .kind(flecs::OnUpdate)
    .each([physics_tick_source](const flecs::entity& e, TransformComponent& tc, const RigidBodyComponent& rb) {
      if (!rb.runtime_body)
        return;

      const auto* timer = physics_tick_source.try_get<flecs::Timer>();
      const f32 alpha = (timer && timer->timeout > 0.f)
                          ? std::clamp(static_cast<f32>(timer->time / timer->timeout), 0.0f, 1.0f)
                          : 1.0f;

      auto position = glm::mix(rb.previous_translation, rb.translation, alpha);
      auto rotation = glm::slerp(rb.previous_rotation, rb.rotation, alpha);
      world_pose_to_local(e, position, rotation);

      tc.position = position;
      tc.rotation = rotation;

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

      auto local_position = ch.translation;
      auto local_rotation = ch.rotation;
      world_pose_to_local(e, local_position, local_rotation);
      tc.position = local_position;
      tc.rotation = local_rotation;

      e.modified<TransformComponent>();
    });

  // -- Renderer Systems ---

  self.world.system<const TransformComponent, CameraComponent>("camera_update")
    .kind(flecs::PostUpdate)
    .each([&self](const TransformComponent& tc, CameraComponent& cc) {
      auto ri = self.get_renderer_instance();
      if (ri)
        Camera::update(cc, tc, ri->get_viewport_size());
    });

  self.world.system<SpriteComponent>("sprite_aabb")
    .kind(flecs::PostUpdate)
    .each([cvar = &self.renderer_cvar](const flecs::entity entity, SpriteComponent& sprite) {
      if (cvar->cvar_draw_bounding_boxes.get()) {
        auto& debug_renderer = App::mod<DebugRenderer>();
        debug_renderer.draw_aabb(sprite.rect, glm::vec4(1, 1, 1, 1.0f));
      }
    });

  self.world.system<MeshComponent>("mesh_aabb")
    .kind(flecs::PostUpdate)
    .each([cvar = &self.renderer_cvar](const flecs::entity entity, MeshComponent& mc) {
      if (cvar->cvar_draw_bounding_boxes.get()) {
        auto& debug_renderer = App::mod<DebugRenderer>();
        debug_renderer.draw_aabb(mc.world_aabb, glm::vec4(0.f, 1.f, 0.f, 1.0f));
      }
    });

  self.world.system<SpriteComponent, SpriteAnimationComponent>("sprite_animation_update")
    .kind(flecs::PostUpdate)
    .each([](flecs::iter& it, size_t, SpriteComponent& sprite, SpriteAnimationComponent& sprite_animation) {
      auto& asset_manager = App::mod<AssetManager>();
      auto material = asset_manager.get_material(sprite.material);

      if (
        sprite_animation.num_frames < 1 || sprite_animation.fps < 1 || sprite_animation.columns < 1 || !material ||
        !material->albedo_texture
      )
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

      const auto albedo_texture = asset_manager.get_texture(material->albedo_texture);
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
  self.world.query_builder<RigidBodyComponent>().build().each([&self](flecs::entity e, RigidBodyComponent& rb) {
    if (rb.runtime_body == nullptr) {
      self.create_rigidbody(e, rb);
    }
  });

  // Characters
  self.world.query_builder<CharacterControllerComponent>().build().each(
    [&self](flecs::entity e, CharacterControllerComponent& ch) {
      if (ch.character == nullptr) {
        self.create_character_controller(e, ch);
      }
    }
  );

  self.create_terrain_collision();

  // Vehicles last: the constraint attaches to the chassis body the rigidbody pass just created.
  self.world.query_builder<VehicleComponent>().build().each([&self](flecs::entity e, VehicleComponent& vehicle) {
    if (vehicle.runtime_constraint == nullptr) {
      self.create_vehicle(e, vehicle);
    }
  });

  self.physics_system->OptimizeBroadPhase();
}

auto Scene::physics_deinit(this Scene& self) -> void {
  ZoneScoped;

  self.destroy_terrain_collision();

  // Vehicles first: the constraint references the chassis body that the rigidbody pass destroys.
  self.world.query_builder<VehicleComponent>().build().each([&self](const flecs::entity& e, VehicleComponent& vehicle) {
    self.destroy_vehicle(vehicle);
  });

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
    [](const flecs::entity& e, CharacterControllerComponent& ch) {
      if (ch.character) {
        // Character is refcounted and owns its body. RemoveBody alone leaves the body registered
        // with the character and leaks the object, so go through its own teardown and release.
        auto* character = reinterpret_cast<JPH::Character*>(ch.character);
        character->RemoveFromPhysicsSystem();
        character->Release();
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

  // Baked up front rather than on the first update: `physics_init` builds the terrain collider out
  // of the heightmap, and a scene that just came out of deserialization has not baked one yet.
  if (self.terrain_dirty) {
    self.terrain_dirty = false;
    self.bake_terrain();
  }

  self.physics_init();

  // Scripting
  for (auto& [_, system] : self.lua_systems) {
    system->on_scene_start(&self);
  }
}

auto Scene::runtime_stop(this Scene& self) -> void {
  ZoneScoped;

  self.running = false;

  self.physics_deinit();

  // Scripting
  for (auto& [_, system] : self.lua_systems) {
    system->on_scene_stop(&self);
  }

  if (auto* rml_context = self.get_rml_context()) {
    auto doc_count = rml_context->GetNumDocuments();
    for (i32 i = 0; i < doc_count; i++) {
      auto doc = rml_context->GetDocument(i);
      if (doc) {
        doc->Hide();
      }
    }
  }
}

auto Scene::runtime_update(this Scene& self, const Timestep& delta_time) -> void {
  ZoneScoped;

  self.run_deferred_functions();
  self.update_pending_model_spawns();

  if (auto* rml_context = self.get_rml_context()) {
    rml_context->Update();
  }

  auto pre_update_phase_enabled = !self.world.entity(flecs::PreUpdate).has(flecs::Disabled);
  auto on_update_phase_enabled = !self.world.entity(flecs::OnUpdate).has(flecs::Disabled);
  if (pre_update_phase_enabled && on_update_phase_enabled) {
    for (auto& [_, system] : self.lua_systems) {
      system->on_scene_update(&self, static_cast<f32>(delta_time.get_seconds()));
    }
  }

  // TODO: Pass our delta_time?
  self.world.progress();

  if (self.renderer_cvar.cvar_enable_physics_debug_renderer.get()) {
    JPH::BodyManager::DrawSettings settings{};
    settings.mDrawShape = true;
    settings.mDrawShapeWireframe = true;

    self.physics_system->DrawBodies(settings, self.physics_debug_renderer.get());
  }

  if (self.terrain_dirty) {
    self.terrain_dirty = false;
    self.bake_terrain();
  }

  // Only while running: outside play mode there is no body to keep in sync, and the readback the
  // rebuild needs is expensive enough that sculpting should not pay for it.
  if (self.running && self.terrain != nullptr && self.terrain->collision_dirty) {
    self.create_terrain_collision();
  }

  std::ranges::sort(self.dirty_transforms);
  self.dirty_transforms.erase(std::ranges::unique(self.dirty_transforms).begin(), self.dirty_transforms.end());
  std::ranges::sort(self.dirty_mesh_instances);
  self.dirty_mesh_instances.erase(
    std::ranges::unique(self.dirty_mesh_instances).begin(),
    self.dirty_mesh_instances.end()
  );

  if (self.rml_view) {
    self.rml_view->update(self.rml_surface_size);
  }
}

auto Scene::prepare_render(this Scene& self) -> void {
  ZoneScoped;

  if (self.renderer_instance) {
    auto& asset_man = App::mod<AssetManager>();
    auto meshlet_instance_visibility_offset = 0_u32;
    auto max_meshlet_instance_count = 0_u32;
    auto gpu_meshes = std::vector<GPU::Mesh>();
    // Parallel to `gpu_meshes`, so the TLAS build can look a BLAS up by mesh index.
    auto blas_addresses = std::vector<u64>();
    auto gpu_mesh_instances = std::vector<GPU::MeshInstance>();
    auto mesh_slot_to_gpu_index = ankerl::unordered_dense::map<u32, u32>();

    if (self.meshes_dirty) {
      auto mesh_instances = self.mesh_instances.slots_unsafe();
      auto unique_mesh_to_gpu_mesh = ankerl::unordered_dense::map<std::pair<UUID, usize>, u32>();

      self.mesh_instances.for_each_active([&](usize index, const MeshInstance& mesh_instance) {
        const auto model = asset_man.get_model(mesh_instance.model_uuid);
        const auto& mesh = model->gpu_meshes[mesh_instance.mesh_node_index];
        const auto material_asset = asset_man.get_asset(mesh_instance.material_uuid);
        const auto material_id = material_asset ? material_asset->material_id
                                                : asset_man.get_null_material()->material_id;

        auto unique_mesh = std::pair(mesh_instance.model_uuid, mesh_instance.mesh_node_index);
        auto mesh_index = 0_u32;
        if (auto it = unique_mesh_to_gpu_mesh.find(unique_mesh); it != unique_mesh_to_gpu_mesh.end()) {
          mesh_index = it->second;
        } else {
          mesh_index = static_cast<u32>(gpu_meshes.size());
          gpu_meshes.emplace_back(mesh);
          const auto& mesh_blases = model->mesh_blases;
          blas_addresses.emplace_back(
            mesh_instance.mesh_node_index < mesh_blases.size()
              ? mesh_blases[mesh_instance.mesh_node_index].device_address
              : 0
          );
          unique_mesh_to_gpu_mesh.emplace(unique_mesh, mesh_index);
        }

        auto lod0_index = 0;
        const auto lod0_meshlet_count = model->lod0_meshlet_counts[mesh_instance.mesh_node_index];

        auto& gpu_mesh_instance = gpu_mesh_instances.emplace_back();
        gpu_mesh_instance.mesh_index = mesh_index;
        gpu_mesh_instance.lod_index = lod0_index;
        gpu_mesh_instance.material_index = SlotMap_decode_id(material_id).index;
        gpu_mesh_instance.transform_index = SlotMap_decode_id(mesh_instance.transform_id).index;
        gpu_mesh_instance.meshlet_instance_visibility_offset = meshlet_instance_visibility_offset;

        mesh_slot_to_gpu_index[static_cast<u32>(index)] = static_cast<u32>(gpu_mesh_instances.size() - 1);

        meshlet_instance_visibility_offset += lod0_meshlet_count;
        max_meshlet_instance_count += lod0_meshlet_count;
      });

      self.gpu_mesh_instance_count = static_cast<u32>(gpu_mesh_instances.size());
      self.max_meshlet_instance_count = max_meshlet_instance_count;
    } else if (!self.dirty_mesh_instances.empty()) {
      u32 gpu_idx = 0;
      self.mesh_instances.for_each_active([&](usize slot_index, const MeshInstance&) {
        mesh_slot_to_gpu_index[static_cast<u32>(slot_index)] = gpu_idx++;
      });
    }

    auto dirty_mesh_instance_gpu_indices = std::vector<u32>();
    dirty_mesh_instance_gpu_indices.reserve(self.dirty_mesh_instances.size());
    for (const auto mesh_instance_id : self.dirty_mesh_instances) {
      const auto slot_index = SlotMap_decode_id(mesh_instance_id).index;
      if (const auto it = mesh_slot_to_gpu_index.find(slot_index); it != mesh_slot_to_gpu_index.end()) {
        dirty_mesh_instance_gpu_indices.push_back(it->second);
      }
    }

    // Upload this frame's dirty transforms plus last frame's: the `previous_world` fix-up below
    // runs after the upload, so a transform's corrected previous matrix only reaches the GPU on the
    // following frame. Duplicates are collapsed inside the uploader.
    auto transform_upload_ids = self.dirty_transforms;
    transform_upload_ids.insert(
      transform_upload_ids.end(),
      self.previously_dirty_transforms.begin(),
      self.previously_dirty_transforms.end()
    );

    auto update_info = RendererInstanceUpdateInfo{
      .mesh_instance_count = self.gpu_mesh_instance_count,
      .max_meshlet_instance_count = self.max_meshlet_instance_count,
      .dirty_transform_ids = transform_upload_ids,
      .gpu_transforms = self.transforms.slots_unsafe(),
      .gpu_meshes = gpu_meshes,
      .gpu_mesh_blas_addresses = blas_addresses,
      .gpu_mesh_instances = gpu_mesh_instances,
      .dirty_mesh_instance_indices = dirty_mesh_instance_gpu_indices,
    };
    self.renderer_instance->update(update_info, self.renderer_cvar);

    for (const auto transform_id : self.dirty_transforms) {
      if (auto* gpu_transform = self.transforms.slot(transform_id)) {
        gpu_transform->previous_world = gpu_transform->world;
      }
    }

    self.previously_dirty_transforms = self.dirty_transforms;
  }
}

auto Scene::get_lua_system(this const Scene& self, const UUID& lua_script) -> LuaSystem* {
  ZoneScoped;

  const auto it = self.lua_systems.find(lua_script);

  return it == self.lua_systems.end() ? nullptr : it->second.get();
}

auto Scene::get_lua_systems(this const Scene& self)
  -> const ankerl::unordered_dense::map<UUID, std::unique_ptr<LuaSystem>>& {
  ZoneScoped;

  return self.lua_systems;
}

auto Scene::add_lua_system(this Scene& self, const UUID& lua_script) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.get_asset(lua_script)->is_loaded()) {
    asset_man.load_asset(lua_script);
  }

  // Copy the source out and drop the guard before running any Lua, which can reach back into the asset registry.
  auto script = LuaScript{};
  {
    auto guard = asset_man.get_script(lua_script);
    if (!guard) {
      OX_LOG_ERROR("Failed to add lua system {}, script asset is not loaded.", lua_script.str());
      return;
    }
    script = guard.copy();
  }

  auto [it, inserted] = self.lua_systems.try_emplace(lua_script, std::make_unique<LuaSystem>(script));
  if (!inserted) {
    return;
  }

  it->second->on_add(&self);

  OX_LOG_TRACE("Added lua system to the scene {}", script.path);
}

auto Scene::remove_lua_system(this Scene& self, const UUID& lua_script) -> void {
  ZoneScoped;

  const auto it = self.lua_systems.find(lua_script);
  if (it == self.lua_systems.end()) {
    return;
  }

  it->second->on_remove(&self);

  OX_LOG_TRACE("Removed lua system from the scene {}", it->second->get_path());

  self.lua_systems.erase(it);
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

auto Scene::spawn_model_hierarchy(this Scene& self, Model& model, PendingModelSpawn& spawn) -> flecs::entity {
  ZoneScoped;

  const auto& root_node = model.mesh_groups.front();
  auto root_entity = self.create_entity(root_node.name, root_node.name.empty() ? false : true);

  struct ProcessingNode {
    flecs::entity parent = {};
    usize mesh_group_index = 0;
  };

  // Used for the root group too: a model built in code has its mesh on the root and no children,
  // so walking only child_indices would drop it.
  auto emit_group_contents = [&](flecs::entity target, const Model::MeshGroup& mesh_group, usize mesh_group_index) {
    for (const auto mesh_index : mesh_group.mesh_indices) {
      spawn.mesh_entities.push_back({
        .mesh_index = mesh_index,
        .mesh_group_index = mesh_group_index,
        .parent = target,
      });
    }

    for (const auto light_index : mesh_group.light_indices) {
      auto& node_light = model.lights[light_index];

      auto lc = LightComponent{
        .type = static_cast<LightComponent::LightType>(node_light.type),
        .color = node_light.color,
        .intensity = node_light.intensity,
      };

      if (node_light.range.has_value()) {
        lc.radius = *node_light.range;
      }
      if (node_light.inner_cone_angle.has_value()) {
        lc.inner_cone_angle = *node_light.inner_cone_angle;
      }
      if (node_light.outer_cone_angle.has_value()) {
        lc.outer_cone_angle = *node_light.outer_cone_angle;
      }

      target.set<LightComponent>(lc);
    }
  };

  emit_group_contents(root_entity, root_node, 0);

  auto processing_nodes = std::stack<ProcessingNode>();
  for (const auto child_index : root_node.child_indices) {
    processing_nodes.push({root_entity, child_index});
  }

  while (!processing_nodes.empty()) {
    const auto [parent_entity, mesh_group_index] = processing_nodes.top();
    const auto& mesh_group = model.mesh_groups[mesh_group_index];
    processing_nodes.pop();

    // Must be unique under `parent_entity` as well as at the root before the entity exists:
    // `create_entity` only deduplicates against the root, and `child_of` then aborts inside
    // `flecs_reparent_name_index` on a name already registered under the parent.
    const auto safe_node_name = self.safe_entity_name(std::string{mesh_group.name}, parent_entity);
    auto node_entity = self.create_entity(safe_node_name, false);
    node_entity.set<TransformComponent>({
      .position = mesh_group.translation,
      .rotation = mesh_group.rotation,
      .scale = mesh_group.scale,
    });
    node_entity.child_of(parent_entity);
    node_entity.modified<TransformComponent>();

    emit_group_contents(node_entity, mesh_group, mesh_group_index);

    for (const auto child_node_indices : mesh_group.child_indices) {
      processing_nodes.push({node_entity, child_node_indices});
    }
  }

  return root_entity;
}

auto Scene::resolve_mesh_spawn(this Scene& self, Model& model, const PendingModelSpawn::MeshEntity& mesh_entity)
  -> MeshSpawnInfo {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto& mesh_group = model.mesh_groups[mesh_entity.mesh_group_index];
  const auto mesh_index = mesh_entity.mesh_index;

  auto mesh_entity_name = !mesh_group.name.empty() ? stack.format("{} Mesh {}", mesh_group.name, mesh_index) : "";
  auto material_index = model.material_indices[mesh_index];
  const auto& mesh_bounds = model.gpu_meshes[mesh_index].bounds;

  return MeshSpawnInfo{
    .mesh_index = mesh_index,
    .parent = mesh_entity.parent,
    .name = std::string{mesh_entity_name},
    .material_uuid = material_index.has_value() ? model.materials[material_index.value()] : UUID(nullptr),
    .aabb = AABB::from_bounds(mesh_bounds.aabb_center, mesh_bounds.aabb_extent),
  };
}

auto Scene::spawn_model_mesh_entity(this Scene& self, const UUID& model_uuid, const MeshSpawnInfo& info) -> void {
  ZoneScoped;

  auto entity = self.create_entity(self.safe_entity_name(info.name, info.parent), false);
  entity.set<TransformComponent>({});
  entity.set<MeshComponent>({
    .model_uuid = model_uuid,
    .mesh_index = static_cast<u32>(info.mesh_index),
    .material_uuid = info.material_uuid,
    .baked_aabb = info.aabb,
  });
  entity.child_of(info.parent);
  entity.modified<TransformComponent>();
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
  if (!asset_man.load_asset(asset_uuid)) {
    return {};
  }

  auto root_entity = flecs::entity();
  auto mesh_spawns = std::vector<MeshSpawnInfo>();
  {
    auto model = asset_man.get_model(asset_uuid);
    if (!model) {
      return {};
    }

    auto spawn = PendingModelSpawn{.model_uuid = asset_uuid};
    root_entity = self.spawn_model_hierarchy(*model.value, spawn);

    for (const auto& mesh_entity : spawn.mesh_entities) {
      if (!model->is_mesh_ready(mesh_entity.mesh_index)) {
        continue;
      }

      mesh_spawns.emplace_back(self.resolve_mesh_spawn(*model.value, mesh_entity));
    }
  }

  for (const auto& mesh_spawn : mesh_spawns) {
    self.spawn_model_mesh_entity(asset_uuid, mesh_spawn);
  }

  return root_entity;
}

auto Scene::create_model_entity_async(this Scene& self, const UUID& asset_uuid) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.get_asset(asset_uuid)) {
    OX_LOG_ERROR("Cannot import an invalid model '{}' into the scene!", asset_uuid.str());
    return;
  }

  if (!asset_man.load_asset_async(asset_uuid)) {
    return;
  }

  self.pending_model_spawns.push_back(PendingModelSpawn{.model_uuid = asset_uuid});
}

auto Scene::update_pending_model_spawns(this Scene& self) -> void {
  ZoneScoped;

  if (self.pending_model_spawns.empty()) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();

  for (auto it = self.pending_model_spawns.begin(); it != self.pending_model_spawns.end();) {
    auto& spawn = *it;
    auto mesh_spawns = std::vector<MeshSpawnInfo>();
    auto fully_loaded = false;
    auto hierarchy_just_spawned = false;

    {
      auto model = asset_man.get_model(spawn.model_uuid);
      if (!model) {
        if (asset_man.is_loading(spawn.model_uuid)) {
          ++it;
        } else {
          it = self.pending_model_spawns.erase(it);
        }

        continue;
      }

      // Must be read before the ready flags below, never after.
      fully_loaded = model->is_fully_loaded();

      if (!spawn.hierarchy_spawned) {
        std::ignore = self.spawn_model_hierarchy(*model.value, spawn);
        spawn.hierarchy_spawned = true;
        hierarchy_just_spawned = true;
      }

      for (auto mesh_it = spawn.mesh_entities.begin(); mesh_it != spawn.mesh_entities.end();) {
        if (!model->is_mesh_ready(mesh_it->mesh_index)) {
          ++mesh_it;
          continue;
        }

        mesh_spawns.emplace_back(self.resolve_mesh_spawn(*model.value, *mesh_it));
        mesh_it = spawn.mesh_entities.erase(mesh_it);
      }
    }

    if (hierarchy_just_spawned) {
      asset_man.acquire_ref(asset_man.get_asset(spawn.model_uuid));
    }

    for (const auto& mesh_spawn : mesh_spawns) {
      self.spawn_model_mesh_entity(spawn.model_uuid, mesh_spawn);
    }

    // Anything still listed once the model is done failed to build.
    if (fully_loaded) {
      it = self.pending_model_spawns.erase(it);
    } else {
      ++it;
    }
  }
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
  gpu_transform->world = visit_parent(self, entity);
  self.dirty_transforms.push_back(transform_id);

  // Mark the entity's mesh instance (if any) as dirty so the VSM invalidate-pages
  // pass can clear pages the mesh used to cover. Child entities are notified below,
  // which re-enters `set_dirty` for each child that has a transform.
  if (
    const auto mesh_it = self.entity_to_mesh_instance_map.find(entity);
    mesh_it != self.entity_to_mesh_instance_map.end()
  ) {
    self.dirty_mesh_instances.push_back(mesh_it->second);
  }

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

  if (auto it = self.entity_transforms_map.find(entity); it != self.entity_transforms_map.end()) {
    return it->second;
  }

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

auto Scene::bake_terrain(this Scene& self) -> void {
  ZoneScoped;

  if (!self.terrain_entity || !self.terrain_entity.has<TerrainComponent>()) {
    self.terrain.reset();
    self.terrain_entity = {};
    return;
  }

  const auto& c = self.terrain_entity.get<TerrainComponent>();

  auto origin = glm::vec3(0.0f);
  if (auto id = self.get_entity_transform_id(self.terrain_entity)) {
    if (const auto* transform = self.get_entity_transform(*id)) {
      origin = glm::vec3(transform->world[3]);
    }
  }

  // Updated in place rather than rebuilt: `Terrain::create` carries the brush edit maps across a
  // re-bake, so tweaking a noise parameter reshapes the procedural base without erasing sculpting.
  if (self.terrain == nullptr) {
    self.terrain = std::make_unique<Terrain>();
  }

  auto* terrain = self.terrain.get();
  terrain->world_origin = origin;
  terrain->world_size = c.world_size;
  terrain->height_range = c.height_range;
  terrain->resolution = {c.resolution, c.resolution};
  terrain->patch_count = {c.patch_count, c.patch_count};
  terrain->target_edge_pixels = c.target_edge_pixels;
  terrain->max_tessellation = c.max_tessellation;
  terrain->layer_tiling = c.layer_tiling;
  terrain->triplanar_begin = c.triplanar_begin;
  terrain->collision_enabled = c.collision_enabled;
  terrain->collision_resolution = c.collision_resolution;
  terrain->collision_friction = c.collision_friction;
  terrain->collision_restitution = c.collision_restitution;

  auto& asset_man = App::mod<AssetManager>();
  const auto material_index = [&asset_man](const UUID& uuid) -> u32 {
    if (!uuid || !asset_man.is_loaded(uuid)) {
      return GPU::TERRAIN_INVALID_LAYER_MATERIAL;
    }
    const auto asset = asset_man.get_asset(uuid);
    return asset ? SlotMap_decode_id(asset->material_id).index : GPU::TERRAIN_INVALID_LAYER_MATERIAL;
  };
  terrain->layer_material_indices = {
    material_index(c.layer_grass),
    material_index(c.layer_rock),
    material_index(c.layer_drainage),
    material_index(c.layer_snow),
  };

  terrain->generate_settings = GPU::TerrainGenerate{
    .erosion =
      {.scale = c.erosion_scale,
       .strength = c.erosion_strength,
       .gully_weight = c.gully_weight,
       .detail = c.detail,
       // `rounding.w` must track the lacunarity so rounding keeps a constant world-space size.
       .rounding = {c.ridge_rounding, c.crease_rounding, 0.1f, 2.0f},
       .octaves = c.erosion_octaves,
       .seed = c.seed},
    .domain_size = c.domain_size,
    .height_frequency = c.height_frequency,
    .height_amplitude = c.height_amplitude,
    .height_lacunarity = c.height_lacunarity,
    .height_gain = c.height_gain,
    .height_octaves = c.height_octaves,
  };

  terrain->derive_settings = GPU::TerrainDerive{
    .slope_rock_begin = c.slope_rock_begin,
    .slope_rock_end = c.slope_rock_end,
    .altitude_snow_begin = c.altitude_snow_begin,
    .altitude_snow_end = c.altitude_snow_end,
  };

  if (const auto result = terrain->create(); !result.has_value()) {
    OX_LOG_ERROR("Failed to create terrain: {}", result.error());
    self.destroy_terrain_collision();
    self.terrain.reset();
    return;
  }

  // `create` only leaves the maps uninitialized when it had to allocate new ones, which is exactly
  // when the strokes have to come back off the asset.
  self.set_terrain_edits_ref(c.terrain_edits);
  if (terrain->edits_uninitialized && self.terrain_edits_ref) {
    if (auto edits = App::mod<AssetManager>().get_terrain_edits(self.terrain_edits_ref)) {
      terrain->upload_edits(*edits.value);
    }
  }

  terrain->bake(App::get_rendercontext());
  terrain->collision_dirty = true;
}

auto Scene::attach_mesh(
  this Scene& self, flecs::entity entity, const UUID& model_uuid, usize mesh_index, const UUID& material_uuid
) -> bool {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto transforms_it = self.entity_transforms_map.find(entity);
  if (transforms_it == self.entity_transforms_map.end()) {
    OX_LOG_FATAL("Target entity must have a transform component!");
    return false;
  }

  const auto transform_id = transforms_it->second;

  // Resolve everything that needs the model before touching scene state. Deserialization sets
  // components before it requests their assets, so the model can legitimately still be unloaded
  // here; from_json attaches those meshes once the assets are in. Rendering assumes every mesh
  // instance has a loaded model, so don't create one until it does.
  auto overriden_material = material_uuid;
  {
    auto model = asset_man.get_model(model_uuid);
    if (!model) {
      return false;
    }

    if (!material_uuid && mesh_index < model->material_indices.size()) {
      // No material override, use original one
      auto material_index = model->material_indices[mesh_index];
      if (material_index.has_value() && material_index.value() < model->materials.size()) {
        overriden_material = model->materials[material_index.value()];
      }
    }
  }

  // Find the old model UUID and detach it from entity.
  auto mesh_instances_it = self.entity_to_mesh_instance_map.find(entity);
  if (mesh_instances_it != self.entity_to_mesh_instance_map.end()) {
    const auto old_mesh_instance_id = mesh_instances_it->second;
    self.mesh_instances.destroy_slot(old_mesh_instance_id);
    self.meshes_dirty = true;
  }

  auto instance_id = self.mesh_instances.create_slot(
    MeshInstance{
      .model_uuid = model_uuid,
      .mesh_node_index = mesh_index,
      .material_uuid = overriden_material,
      .transform_id = transform_id,
    }
  );
  self.entity_to_mesh_instance_map.insert_or_assign(entity, instance_id);
  self.meshes_dirty = true;
  self.set_dirty(entity);

  return true;
}

auto Scene::detach_mesh(this Scene& self, flecs::entity entity) -> bool {
  ZoneScoped;

  auto instances_it = self.entity_to_mesh_instance_map.find(entity);
  if (instances_it == self.entity_to_mesh_instance_map.end()) {
    return false;
  }

  const auto instance_id = instances_it->second;
  if (!self.mesh_instances.slot(instance_id)) {
    return false;
  }

  self.mesh_instances.destroy_slot(instance_id);
  self.meshes_dirty = true;

  self.entity_to_mesh_instance_map.erase(instances_it);

  return true;
}

auto Scene::particle_emitter_state(this Scene& self, const flecs::entity entity) -> ParticleEmitterState* {
  const auto it = self.entity_particle_emitters_map.find(entity);
  if (it == self.entity_particle_emitters_map.end()) {
    return nullptr;
  }

  return self.particle_emitters.slot(it->second);
}

auto Scene::play_particles(this Scene& self, const flecs::entity entity) -> void {
  if (auto* state = self.particle_emitter_state(entity)) {
    state->playing = true;
  }
}

auto Scene::stop_particles(this Scene& self, const flecs::entity entity) -> void {
  if (auto* state = self.particle_emitter_state(entity)) {
    state->playing = false;
  }
}

auto Scene::restart_particles(this Scene& self, const flecs::entity entity) -> void {
  auto* state = self.particle_emitter_state(entity);
  if (!state) {
    return;
  }

  state->playing = true;
  state->time = 0.0f;
  state->spawn_accumulator = 0.0f;
  // clearing the trigger state is what re-arms Once nodes and restarts Interval accumulators
  state->program_state = {};
}

auto Scene::is_particles_playing(this Scene& self, const flecs::entity entity) -> bool {
  const auto* state = self.particle_emitter_state(entity);
  return state != nullptr && state->playing;
}

auto Scene::emit_particle_burst(this Scene& self, const flecs::entity entity, const u32 count) -> void {
  if (auto* state = self.particle_emitter_state(entity)) {
    state->pending_burst += count;
  }
}

auto Scene::set_particle_parameter(
  this Scene& self, const flecs::entity entity, const u32 index, const glm::vec4& value
) -> void {
  if (index >= GPU::PARTICLE_USER_PARAM_COUNT || !entity.has<ParticleSystemComponent>()) {
    return;
  }

  auto& component = entity.get_mut<ParticleSystemComponent>();

  // the first write takes the instance off the asset's defaults, so seed the other slots from them
  // instead of dropping them to zero
  if (!component.override_parameters) {
    component.override_parameters = true;

    if (auto* state = self.particle_emitter_state(entity); state && state->asset) {
      if (auto system = App::mod<AssetManager>().get_particle_system(state->asset)) {
        for (auto i = 0_u32; i < GPU::PARTICLE_USER_PARAM_COUNT; i++) {
          component.parameter(i) = i < system->parameters.size() ? system->parameters[i].default_value
                                                                 : glm::vec4(0.0f);
        }
      }
    }
  }

  component.parameter(index) = value;
}

auto Scene::set_particle_parameter(
  this Scene& self, const flecs::entity entity, const std::string_view name, const glm::vec4& value
) -> bool {
  const auto* state = self.particle_emitter_state(entity);
  if (!state || !state->asset) {
    return false;
  }

  auto index = [&]() -> option<u32> {
    auto system = App::mod<AssetManager>().get_particle_system(state->asset);
    return system ? system->find_parameter(name) : nullopt;
  }();

  if (!index) {
    return false;
  }

  self.set_particle_parameter(entity, *index, value);

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

  for (auto& [_, system] : lua_systems) {
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

  for (auto& [_, system] : lua_systems) {
    system->on_contact_persisted(this, body1, body2, manifold, settings);
  }
}

auto Scene::on_contact_removed(const JPH::SubShapeIDPair& sub_shape_pair) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [_, system] : lua_systems) {
    system->on_contact_removed(this, sub_shape_pair);
  }
}

auto Scene::on_body_activated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [_, system] : lua_systems) {
    system->on_body_activated(this, body_id, (u64)body_user_data);
  }
}

auto Scene::on_body_deactivated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(physics_mutex);

  for (auto& [_, system] : lua_systems) {
    system->on_body_deactivated(this, body_id, (u64)body_user_data);
  }
}

auto build_mesh_collider_shape(
  flecs::entity entity,
  const glm::vec3& scale,
  const MeshColliderComponent& component,
  const JPH::PhysicsMaterial* material
) -> JPH::ShapeSettings::ShapeResult {
  ZoneScoped;

  const auto* mesh_component = entity.try_get<MeshComponent>();
  if (!mesh_component) {
    OX_LOG_ERROR("MeshColliderComponent on '{}' needs a MeshComponent to take triangles from.", entity.name().c_str());
    return {};
  }

  auto& asset_man = App::mod<AssetManager>();
  auto model = asset_man.get_model(mesh_component->model_uuid);
  if (!model) {
    OX_LOG_ERROR("MeshColliderComponent on '{}' has no loaded model.", entity.name().c_str());
    return {};
  }

  if (mesh_component->mesh_index >= model->collision_meshes.size()) {
    OX_LOG_ERROR(
      "MeshColliderComponent on '{}' points at mesh {}, which does not exist.",
      entity.name().c_str(),
      mesh_component->mesh_index
    );
    return {};
  }

  if (!model->is_mesh_ready(mesh_component->mesh_index)) {
    OX_LOG_ERROR("MeshColliderComponent on '{}' ran before its mesh finished loading.", entity.name().c_str());
    return {};
  }

  const auto& collision = model->collision_meshes[mesh_component->mesh_index];
  if (collision.positions.empty() || collision.indices.size() < 3) {
    OX_LOG_ERROR("MeshColliderComponent on '{}' has no triangles.", entity.name().c_str());
    return {};
  }

  // Jolt cannot scale a mesh shape non-uniformly after the fact, so bake the world scale into the
  // vertices. A mirrored scale flips winding, which would leave every triangle facing inward.
  const auto flipped = scale.x * scale.y * scale.z < 0.f;

  auto vertices = JPH::Array<JPH::Float3>();
  vertices.reserve(collision.positions.size());
  for (const auto& position : collision.positions) {
    const auto scaled = position * scale;
    vertices.push_back(JPH::Float3(scaled.x, scaled.y, scaled.z));
  }

  if (component.convex) {
    auto points = JPH::Array<JPH::Vec3>();
    points.reserve(vertices.size());
    for (const auto& vertex : vertices) {
      points.push_back(JPH::Vec3(vertex.x, vertex.y, vertex.z));
    }

    auto shape_settings = JPH::ConvexHullShapeSettings(points, JPH::cDefaultConvexRadius, material);
    shape_settings.SetDensity(glm::max(0.001f, component.density));
    return shape_settings.Create();
  }

  auto triangles = JPH::IndexedTriangleList();
  triangles.reserve(collision.indices.size() / 3);
  for (auto i = 0_sz; i + 2 < collision.indices.size(); i += 3) {
    const auto i0 = collision.indices[i];
    const auto i1 = collision.indices[flipped ? i + 2 : i + 1];
    const auto i2 = collision.indices[flipped ? i + 1 : i + 2];
    // Jolt rejects the whole shape on a degenerate triangle, and simplification does produce them.
    if (i0 == i1 || i1 == i2 || i0 == i2) {
      continue;
    }
    triangles.push_back(JPH::IndexedTriangle(i0, i1, i2, 0));
  }

  if (triangles.empty()) {
    OX_LOG_ERROR("MeshColliderComponent on '{}' has no non-degenerate triangles.", entity.name().c_str());
    return {};
  }

  auto materials = JPH::PhysicsMaterialList();
  materials.push_back(material);

  return JPH::MeshShapeSettings(std::move(vertices), std::move(triangles), std::move(materials)).Create();
}

auto build_collider_shape(
  flecs::entity entity, RigidBodyComponent::BodyType body_type, glm::vec3& offset, bool& needs_mass_override
) -> JPH::ShapeSettings::ShapeResult {
  ZoneScoped;

  // World scale, not local: a collider deep in a model hierarchy inherits every parent's scale.
  const auto world_matrix = Scene::get_world_transform(entity);
  const auto world_scale = glm::vec3(
    glm::length(glm::vec3(world_matrix[0])),
    glm::length(glm::vec3(world_matrix[1])),
    glm::length(glm::vec3(world_matrix[2]))
  );
  const auto max_scale_component = glm::max(glm::max(world_scale.x, world_scale.y), world_scale.z);
  const auto entity_name = std::string(entity.name());

  if (const auto* bc = entity.try_get<BoxColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), bc->friction, bc->restitution);

    glm::vec3 scale = bc->size;
    JPH::BoxShapeSettings shape_settings({glm::abs(scale.x), glm::abs(scale.y), glm::abs(scale.z)}, 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, bc->density));
    offset = bc->offset;
    return shape_settings.Create();
  }

  if (const auto* scc = entity.try_get<SphereColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), scc->friction, scc->restitution);

    float radius = 2.0f * scc->radius * max_scale_component;
    JPH::SphereShapeSettings shape_settings(glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, scc->density));
    offset = scc->offset;
    return shape_settings.Create();
  }

  if (const auto* ccc = entity.try_get<CapsuleColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), ccc->friction, ccc->restitution);

    float radius = 2.0f * ccc->radius * max_scale_component;
    JPH::CapsuleShapeSettings shape_settings(glm::max(0.01f, ccc->height) * 0.5f, glm::max(0.01f, radius), mat);
    shape_settings.SetDensity(glm::max(0.001f, ccc->density));
    offset = ccc->offset;
    return shape_settings.Create();
  }

  if (const auto* tcc = entity.try_get<TaperedCapsuleColliderComponent>()) {
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
    offset = tcc->offset;
    return shape_settings.Create();
  }

  if (const auto* cycc = entity.try_get<CylinderColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), cycc->friction, cycc->restitution);

    float radius = 2.0f * cycc->radius * max_scale_component;
    JPH::CylinderShapeSettings
      shape_settings(glm::max(0.01f, cycc->height) * 0.5f, glm::max(0.01f, radius), 0.05f, mat);
    shape_settings.SetDensity(glm::max(0.001f, cycc->density));
    offset = cycc->offset;
    return shape_settings.Create();
  }

  if (const auto* mcc = entity.try_get<MeshColliderComponent>()) {
    const JPH::Ref<PhysicsMaterial3D>
      mat = new PhysicsMaterial3D(entity_name, JPH::ColorArg(255, 0, 0), mcc->friction, mcc->restitution);

    // Jolt only registers triangle mesh collision against convex shapes, so a Dynamic mesh body never
    // finds the static world or the terrain and drops through it. Kinematic is fine: it is driven, and
    // whatever it hits is convex.
    if (!mcc->convex && body_type == RigidBodyComponent::BodyType::Dynamic) {
      OX_LOG_ERROR(
        "MeshColliderComponent on '{}' is a triangle mesh on a Dynamic body, which cannot collide with "
        "static meshes or terrain. Set `convex` on the collider, or make the body Static or Kinematic.",
        entity_name
      );
      return {};
    }

    offset = mcc->offset;
    // A triangle mesh has no computable volume, so Jolt hands back empty mass properties that cannot
    // be scaled to a mass. Kinematic bodies still need an invertible inertia tensor.
    needs_mass_override = !mcc->convex;
    return build_mesh_collider_shape(entity, world_scale, *mcc, mat);
  }

  return {};
}

auto Scene::create_rigidbody(this Scene& self, flecs::entity entity, RigidBodyComponent& component) -> void {
  ZoneScoped;

  auto& body_interface = self.physics_system->GetBodyInterface();
  if (component.runtime_body) {
    auto body_id = static_cast<JPH::Body*>(component.runtime_body)->GetID();
    body_interface.RemoveBody(body_id);
    body_interface.DestroyBody(body_id);
    component.runtime_body = nullptr;
  }

  // Jolt bodies live in world space, but TransformComponent is local to the parent. A rigidbody on a
  // child (which is where a model's MeshComponent ends up) would otherwise be created at its local
  // offset, typically the origin.
  auto body_world = Scene::get_world_transform(entity);
  auto body_position = glm::vec3{};
  auto body_rotation = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
  auto body_scale = glm::vec3{1.f};
  {
    auto skew = glm::vec3{};
    auto perspective = glm::vec4{};
    glm::decompose(body_world, body_scale, body_rotation, body_position, skew, perspective);
  }

  // Shapes are placed relative to the body, so the body's own rotation and translation come back out.
  // Scale stays in: each collider bakes its own world scale into its shape.
  const auto world_to_body = glm::inverse(
    glm::translate(glm::mat4(1.f), body_position) * glm::mat4_cast(body_rotation)
  );

  JPH::MutableCompoundShapeSettings compound_shape_settings = {};
  bool needs_mass_override = false;
  auto shape_count = 0_u32;

  // Colliders on descendants fold into this body as long as they do not have a body of their own, so
  // a model root can carry the RigidBodyComponent while its mesh children carry the colliders.
  auto collect = [&](this auto& collect_ref, flecs::entity collider_entity) -> void {
    if (collider_entity != entity && collider_entity.has<RigidBodyComponent>()) {
      return; // Owned by another body, along with everything below it.
    }

    if (collider_entity.has<TransformComponent>()) {
      auto local_needs_mass_override = false;
      auto offset = glm::vec3{};
      auto shape_result = build_collider_shape(collider_entity, component.type, offset, local_needs_mass_override);

      if (shape_result.HasError()) {
        OX_LOG_ERROR("Jolt shape error: {}", shape_result.GetError().c_str());
      } else if (!shape_result.IsEmpty()) {
        const auto relative = world_to_body * Scene::get_world_transform(collider_entity);
        auto relative_position = glm::vec3{};
        auto relative_rotation = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
        auto relative_scale = glm::vec3{};
        auto skew = glm::vec3{};
        auto perspective = glm::vec4{};
        glm::decompose(relative, relative_scale, relative_rotation, relative_position, skew, perspective);

        const auto placement = relative_position + relative_rotation * offset;
        compound_shape_settings.AddShape(
          {placement.x, placement.y, placement.z},
          {relative_rotation.x, relative_rotation.y, relative_rotation.z, relative_rotation.w},
          shape_result.Get()
        );

        needs_mass_override |= local_needs_mass_override;
        shape_count += 1;
      }
    }

    collider_entity.children([&collect_ref](flecs::entity child) { collect_ref(child); });
  };
  collect(entity);

  if (shape_count == 0) {
    return; // No Shape
  }

  const auto object_layer = component.type == RigidBodyComponent::BodyType::Static ? PhysicsLayers::NON_MOVING
                                                                                   : PhysicsLayers::MOVING;

  auto compound_shape = compound_shape_settings.Create();
  if (compound_shape.HasError()) {
    OX_LOG_ERROR("Jolt shape error: {}", compound_shape.GetError().c_str());
    return;
  }

  JPH::BodyCreationSettings body_settings(
    compound_shape.Get(),
    {body_position.x, body_position.y, body_position.z},
    {body_rotation.x, body_rotation.y, body_rotation.z, body_rotation.w},
    static_cast<JPH::EMotionType>(component.type),
    object_layer
  );

  JPH::MassProperties mass_properties;
  mass_properties.mMass = glm::max(0.01f, component.mass);
  if (needs_mass_override) {
    const auto extent = compound_shape.Get()->GetLocalBounds().GetSize();
    mass_properties.SetMassAndInertiaOfSolidBox(JPH::Vec3::sMax(extent, JPH::Vec3::sReplicate(0.01f)), 1.f);
    mass_properties.ScaleToMass(glm::max(0.01f, component.mass));
    body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
  } else {
    body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  }
  body_settings.mMassPropertiesOverride = mass_properties;
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

  // Seeded in world space so the first interpolated frame does not snap from the local transform.
  component.previous_translation = component.translation = body_position;
  component.previous_rotation = component.rotation = body_rotation;

  component.runtime_body = body;
}

auto Scene::create_terrain_collision(this Scene& self) -> void {
  ZoneScoped;

  self.destroy_terrain_collision();

  auto* terrain = self.terrain.get();
  if (terrain == nullptr) {
    return;
  }

  // Cleared even when no body comes out of it, so a terrain that cannot collide (disabled, not baked
  // yet, degenerate) does not retry the download every frame.
  terrain->collision_dirty = false;
  if (!terrain->collision_enabled || !terrain->is_baked()) {
    return;
  }

  terrain->download_collision_heights(App::get_rendercontext());

  const auto sample_count = terrain->collision_sample_count;
  if (
    sample_count < TERRAIN_COLLISION_MIN_SAMPLES ||
    terrain->collision_heights.size() != static_cast<usize>(sample_count) * sample_count
  ) {
    OX_LOG_ERROR("Terrain heightmap readback produced no samples; terrain will not collide.");
    return;
  }

  // Jolt spans `sample_count - 1` cells between the outer samples, which is exactly the world rect
  // the renderer maps the heightmap onto.
  const auto world_min = terrain->world_min();
  const auto cell_size = terrain->world_size / static_cast<f32>(sample_count - 1);

  auto shape_settings = JPH::HeightFieldShapeSettings(
    terrain->collision_heights.data(),
    JPH::Vec3(world_min.x, 0.0f, world_min.y),
    JPH::Vec3(cell_size.x, 1.0f, cell_size.y),
    sample_count
  );
  shape_settings.mBlockSize = TERRAIN_COLLISION_BLOCK_SIZE;
  // Quantization is relative to the terrain's own height range rather than to whatever the current
  // samples happen to span, so re-baking a flatter terrain does not change the encoding.
  shape_settings.mMinHeightValue = terrain->base_height();
  shape_settings.mMaxHeightValue = terrain->base_height() + terrain->height_scale();

  const auto shape_result = shape_settings.Create();
  if (shape_result.HasError()) {
    OX_LOG_ERROR("Jolt terrain shape error: {}", shape_result.GetError().c_str());
    return;
  }

  auto body_settings = JPH::BodyCreationSettings(
    shape_result.Get(),
    JPH::Vec3::sZero(),
    JPH::Quat::sIdentity(),
    JPH::EMotionType::Static,
    PhysicsLayers::NON_MOVING
  );
  body_settings.mFriction = terrain->collision_friction;
  body_settings.mRestitution = terrain->collision_restitution;

  auto& body_interface = self.physics_system->GetBodyInterface();
  auto* body = body_interface.CreateBody(body_settings);
  OX_CHECK_NULL(body, "Jolt is out of bodies!");

  body->SetUserData(static_cast<u64>(self.terrain_entity.id()));
  body_interface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
  self.terrain_body_id = body->GetID();
}

auto Scene::destroy_terrain_collision(this Scene& self) -> void {
  ZoneScoped;

  if (self.terrain_body_id.IsInvalid()) {
    return;
  }

  auto& body_interface = self.physics_system->GetBodyInterface();
  body_interface.RemoveBody(self.terrain_body_id);
  body_interface.DestroyBody(self.terrain_body_id);
  self.terrain_body_id = JPH::BodyID();
}

auto Scene::sync_terrain_edits(this Scene& self) -> void {
  ZoneScoped;

  if (self.terrain == nullptr || !self.terrain->edits_dirty || !self.terrain_edits_ref) {
    return;
  }

  self.terrain->edits_dirty = false;
  App::mod<AssetManager>().set_terrain_edits(
    self.terrain_edits_ref,
    self.terrain->download_edits(App::get_rendercontext())
  );
}

auto Scene::clear_terrain_edits(this Scene& self) -> void {
  ZoneScoped;

  if (self.terrain == nullptr) {
    return;
  }

  self.terrain->clear_edits();
  self.terrain_dirty = true;

  if (self.terrain_edits_ref) {
    App::mod<AssetManager>().set_terrain_edits(self.terrain_edits_ref, {});
  }
}

auto Scene::set_terrain_edits_ref(this Scene& self, const UUID& uuid) -> void {
  ZoneScoped;

  if (self.terrain_edits_ref == uuid) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto previous = std::exchange(self.terrain_edits_ref, uuid);

  // The scene holds exactly one ref, matched by the release below and by the OnRemove observer.
  // `from_json` already took it for a UUID that came out of the scene file, so only an asset
  // nothing has loaded yet is acquired here. is_loaded() takes and drops its own read guard; don't
  // hold one across load_asset().
  if (uuid && !asset_man.is_loaded(uuid)) {
    asset_man.load_asset(uuid);
  }

  // Released after the acquire so re-pointing at the same underlying asset does not drop it to zero
  // in between.
  if (previous) {
    asset_man.unload_asset(previous);
  }
}

void Scene::create_character_controller(flecs::entity entity, CharacterControllerComponent& component) const {
  ZoneScoped;

  // World space, same as rigidbodies: a character parented to something would otherwise spawn at its
  // local offset.
  const auto world_position = get_world_position(entity);
  const auto position = JPH::Vec3(world_position.x, world_position.y, world_position.z);
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

  auto* character = new JPH::Character(settings.get(), position, JPH::Quat::sIdentity(), 0, physics_system.get());
  // Balances the Release in physics_deinit / the OnRemove observer.
  character->AddRef();
  component.character = character;

  character->AddToPhysicsSystem(JPH::EActivation::Activate);

  auto ch_body = physics_system->GetBodyLockInterface().TryGetBody(character->GetBodyID());
  ch_body->SetUserData(static_cast<u64>(entity.id()));
}

auto Scene::create_vehicle(this Scene& self, flecs::entity entity, VehicleComponent& component) -> void {
  ZoneScoped;

  self.destroy_vehicle(component);

  const auto* rb = entity.try_get<RigidBodyComponent>();
  if (!rb || !rb->runtime_body) {
    OX_LOG_ERROR("Vehicle entity '{}' needs a RigidBodyComponent with a body.", entity.name().c_str());
    return;
  }
  if (rb->type != RigidBodyComponent::BodyType::Dynamic) {
    OX_LOG_ERROR("Vehicle entity '{}' needs a Dynamic rigidbody.", entity.name().c_str());
    return;
  }

  // Wheel order is hierarchy order, and the differentials below pair them up as (0,1), (2,3), ...
  // so author them left/right per axle, front to back.
  auto wheel_entities = std::vector<flecs::entity>();
  entity.children([&wheel_entities](flecs::entity child) {
    if (child.has<VehicleWheelComponent>()) {
      wheel_entities.emplace_back(child);
    }
  });

  if (wheel_entities.size() < 2 || wheel_entities.size() % 2 != 0) {
    OX_LOG_ERROR(
      "Vehicle entity '{}' has {} wheel children, needs an even count of at least 2.",
      entity.name().c_str(),
      wheel_entities.size()
    );
    return;
  }

  auto settings = JPH::VehicleConstraintSettings();
  settings.mUp = JPH::Vec3(component.up.x, component.up.y, component.up.z).NormalizedOr(JPH::Vec3::sAxisY());
  settings.mForward = JPH::Vec3(component.forward.x, component.forward.y, component.forward.z)
                        .NormalizedOr(JPH::Vec3::sAxisZ());
  settings.mMaxPitchRollAngle = JPH::DegreesToRadians(component.max_pitch_roll_angle);

  for (auto wheel_index = 0_u32; wheel_index < wheel_entities.size(); wheel_index++) {
    auto wheel_entity = wheel_entities[wheel_index];
    auto& wheel = wheel_entity.get_mut<VehicleWheelComponent>();

    if (wheel.attachment == glm::vec3(0.f)) {
      const auto& wheel_tc = wheel_entity.get<TransformComponent>();
      const auto up = glm::vec3(settings.mUp.GetX(), settings.mUp.GetY(), settings.mUp.GetZ());
      wheel.attachment = wheel_tc.position + up * glm::max(wheel.suspension_min_length, wheel.suspension_max_length);
      OX_LOG_INFO(
        "Vehicle wheel '{}' had no suspension attachment, derived ({}, {}, {}) from its transform.",
        wheel_entity.name().c_str(),
        wheel.attachment.x,
        wheel.attachment.y,
        wheel.attachment.z
      );
    }

    auto* wheel_settings = new JPH::WheelSettingsWV();
    wheel_settings->mPosition = JPH::Vec3(wheel.attachment.x, wheel.attachment.y, wheel.attachment.z);
    wheel_settings->mRadius = glm::max(0.01f, wheel.radius);
    wheel_settings->mWidth = glm::max(0.01f, wheel.width);
    wheel_settings->mSuspensionMinLength = wheel.suspension_min_length;
    wheel_settings->mSuspensionMaxLength = glm::max(wheel.suspension_min_length, wheel.suspension_max_length);
    wheel_settings->mSuspensionSpring.mFrequency = wheel.suspension_frequency;
    wheel_settings->mSuspensionSpring.mDamping = wheel.suspension_damping;
    wheel_settings->mMaxSteerAngle = JPH::DegreesToRadians(wheel.max_steer_angle);
    wheel_settings->mMaxBrakeTorque = wheel.max_brake_torque;
    wheel_settings->mMaxHandBrakeTorque = wheel.max_hand_brake_torque;

    settings.mWheels.push_back(wheel_settings);
    wheel.runtime_wheel_index = wheel_index;
  }

  auto* controller = new JPH::WheeledVehicleControllerSettings();
  controller->mEngine.mMaxTorque = component.max_engine_torque;
  controller->mEngine.mMinRPM = component.min_engine_rpm;
  controller->mEngine.mMaxRPM = glm::max(component.min_engine_rpm + 1.f, component.max_engine_rpm);
  controller->mEngine.mInertia = component.engine_inertia;
  controller->mTransmission.mMode = component.auto_transmission ? JPH::ETransmissionMode::Auto
                                                                : JPH::ETransmissionMode::Manual;
  controller->mTransmission.mClutchStrength = component.clutch_strength;
  controller->mDifferentialLimitedSlipRatio = component.limited_slip_ratio;

  // One differential per axle, front to back.
  const auto axle_count = static_cast<u32>(wheel_entities.size() / 2);
  for (auto axle = 0_u32; axle < axle_count; axle++) {
    auto differential = JPH::VehicleDifferentialSettings();
    differential.mLeftWheel = static_cast<int>(axle * 2);
    differential.mRightWheel = static_cast<int>(axle * 2 + 1);

    const auto is_front = axle == 0;
    const auto is_rear = axle == axle_count - 1;
    auto powered = true;
    switch (component.drive_mode) {
      case VehicleComponent::DriveMode::FrontWheelDrive: powered = is_front; break;
      case VehicleComponent::DriveMode::RearWheelDrive : powered = is_rear; break;
      case VehicleComponent::DriveMode::AllWheelDrive  : powered = true; break;
    }

    // A wheel opted out of drive still needs a differential slot, it just gets no torque.
    const auto& left = wheel_entities[axle * 2].get<VehicleWheelComponent>();
    const auto& right = wheel_entities[axle * 2 + 1].get<VehicleWheelComponent>();
    differential.mEngineTorqueRatio = (powered && (left.driven || right.driven)) ? 1.f : 0.f;

    controller->mDifferentials.push_back(differential);
  }

  // Normalize torque ratios so total engine torque stays constant regardless of axle count.
  auto total_ratio = 0.f;
  for (const auto& differential : controller->mDifferentials) {
    total_ratio += differential.mEngineTorqueRatio;
  }
  if (total_ratio > 0.f) {
    for (auto& differential : controller->mDifferentials) {
      differential.mEngineTorqueRatio /= total_ratio;
    }
  } else {
    OX_LOG_WARN("Vehicle entity '{}' has no driven axle, it will not accelerate.", entity.name().c_str());
  }

  settings.mController = controller;

  auto* body = static_cast<JPH::Body*>(rb->runtime_body);
  auto* constraint = new JPH::VehicleConstraint(*body, settings);

  switch (component.collision_mode) {
    case VehicleComponent::CollisionMode::Ray:
      constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(PhysicsLayers::MOVING));
      break;
    case VehicleComponent::CollisionMode::SphereCast:
      constraint->SetVehicleCollisionTester(
        new JPH::VehicleCollisionTesterCastSphere(PhysicsLayers::MOVING, 0.5f * settings.mWheels[0]->mWidth)
      );
      break;
    case VehicleComponent::CollisionMode::CylinderCast:
      constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(PhysicsLayers::MOVING));
      break;
  }

  // The system takes ownership through its own reference, and the constraint doubles as the step
  // listener that runs the wheel simulation.
  self.physics_system->AddConstraint(constraint);
  self.physics_system->AddStepListener(constraint);

  component.runtime_constraint = constraint;
}

auto Scene::destroy_vehicle(this Scene& self, VehicleComponent& component) -> void {
  ZoneScoped;

  if (!component.runtime_constraint) {
    return;
  }

  auto* constraint = static_cast<JPH::VehicleConstraint*>(component.runtime_constraint);

  // Step listener first: it must not outlive its place in the constraint list.
  self.physics_system->RemoveStepListener(constraint);
  self.physics_system->RemoveConstraint(constraint);

  component.runtime_constraint = nullptr;
}

auto Scene::render(
  this Scene& self,
  vuk::Value<vuk::ImageAttachment>&& dst_attachment,
  glm::ivec2 viewport_origin,
  glm::ivec2 viewport_size,
  glm::ivec2 surface_size,
  bool keyboard_focused
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto ri = self.get_renderer_instance();
  OX_CHECK_NULL(ri);

  if (self.rml_view) {
    OX_CHECK_GT(viewport_size.x, 0, "viewport_size.x is not set");
    OX_CHECK_GT(viewport_size.y, 0, "viewport_size.y is not set");
    OX_CHECK_GT(surface_size.x, 0, "surface_size.x is not set");
    OX_CHECK_GT(surface_size.y, 0, "surface_size.y is not set");

    // Picked up by the next runtime_update, which is where the geometry is actually collected.
    self.rml_surface_size = surface_size;
    self.rml_view->set_viewport(viewport_origin, viewport_size, keyboard_focused);
  }

  for (auto& [_, system] : self.lua_systems) {
    system->on_scene_render(&self, dst_attachment->extent);
  }

  // Paired with the render below: the GPU-side work is built here rather than in `runtime_update` so
  // that a scene which ticks without rendering never leaves an unsubmitted graph behind.
  self.prepare_render();

  // The prepared frame is consumed here, so the dirty state it covers has now really been submitted.
  self.dirty_transforms.clear();
  self.dirty_mesh_instances.clear();
  self.meshes_dirty = false;

  auto scene_surface = ri->render(
    std::move(dst_attachment),
    viewport_origin,
    viewport_size,
    surface_size,
    self.renderer_cvar
  );

  if (!self.rml_view) {
    return scene_surface;
  }

  return self.rml_view->draw(App::get_rendercontext(), std::move(scene_surface));
}

auto Scene::get_rml_context(this const Scene& self) -> Rml::Context* {
  return self.rml_view ? self.rml_view->context() : nullptr;
}

auto Scene::get_rml_context_name(this const Scene& self) -> std::string_view {
  ZoneScoped;

  return self.rml_view ? self.rml_view->name() : std::string_view{};
}

auto Scene::set_rml_dpi_ratio(this const Scene& self, f32 ratio) -> void {
  ZoneScoped;

  if (self.rml_view) {
    self.rml_view->set_dpi_ratio(ratio);
  }
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

      // Observers must not fill in defaults for fields this loop is about to write.
      const auto was_deserializing = std::exchange(self.deserializing_entity, true);
      e.add(component_id);
      auto* component = e.get_mut(component_id);
      auto deserializer = JsonEntityDeserializer(self.world, field_json.value());
      deserializer.serialize(component_id, component);
      requested_assets.insert_range(requested_assets.end(), std::move(deserializer.requested_assets));
      self.deserializing_entity = was_deserializing;
      e.modified(component_id);
    }
  }

  // Components are notified one at a time in JSON order, so an observer keyed on a pair such as
  // (TransformComponent, MeshComponent) never sees both terms: the transform is notified before the
  // mesh exists, and notifying the mesh does not re-trigger it. Re-notify once the whole set is in
  // place, the same way create_model_entity does after setting its components.
  if (e.has<TransformComponent>()) {
    e.modified<TransformComponent>();
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

  self.renderer_cvar.to_json(writer);

  writer["scripts"].begin_array();
  for (auto& [script_uuid, system] : self.lua_systems) {
    writer.begin_obj();
    writer["uuid"] = script_uuid.str();
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

  // Brush strokes live only in the terrain's GPU edit maps, which the scene JSON does not carry, so
  // the copy would otherwise come up as the freshly generated terrain. `bake_terrain` reuses this
  // instance and `Terrain::create` carries the edits across.
  if (src_scene->terrain != nullptr && !src_scene->terrain->edits_uninitialized) {
    new_scene->terrain = std::make_unique<Terrain>();
    new_scene->terrain->clone_edits_from(*src_scene->terrain, App::get_rendercontext());
  }

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

  auto config_json = doc["config"];
  if (!config_json.error()) {
    self.renderer_cvar.from_json(config_json.value());
  }

  auto& asset_man = App::mod<AssetManager>();

  std::vector<UUID> requested_assets = {};

  std::vector<UUID> requested_scripts = {};
  auto scripts_array = doc["scripts"];
  if (!scripts_array.error()) {
    for (auto script_json : scripts_array.get_array()) {
      auto uuid_json = script_json.value_unsafe();
      auto uuid_str = uuid_json["uuid"].get_string();
      if (!uuid_str.error()) {
        auto script_uuid = UUID::from_string(uuid_str.value_unsafe()).value();
        requested_scripts.emplace_back(script_uuid);
      }
    }
  } else {
    OX_LOG_ERROR("No scripts field found in scene!");
  }

  // on_add callback of scripts should be called before entities are deserialized.
  for (auto& script : requested_scripts) {
    if (!asset_man.is_loaded(script)) {
      asset_man.load_asset(script);
    }

    self.add_lua_system(script);
  }

  auto entities_array = doc["entities"];
  if (!entities_array.error()) {
    for (auto entity_json : entities_array.get_array()) {
      if (
        Scene::json_to_entity(self, flecs::entity::null(), entity_json.value_unsafe(), requested_assets) ==
        flecs::entity::null()
      ) {
        return false;
      }
    }
  } else {
    OX_LOG_ERROR("No entities field found in scene!");
    return false;
  }

  OX_LOG_INFO("Loading scene {} with {} assets...", self.scene_name, requested_assets.size());

  for (const auto& asset_uuid : requested_assets) {
    // Snapshot the type and release the read guard before load_asset()/add_lua_system(),
    // which re-lock the registry.
    auto exists = false;
    if (auto asset = asset_man.get_asset(asset_uuid)) {
      exists = true;
    }
    if (exists) {
      asset_man.load_asset(asset_uuid);
    } else {
      // Not an imported/physical asset
      // Most likely was created on runtime and never written to a file, these should never exist.
      // Otherwise component will be left with an unloaded asset.
      OX_LOG_WARN("Ghost asset found! {}", asset_uuid.str());
    }
  }

  // Assets are only requested after every entity exists, so meshes whose model was still unloaded
  // when their component was set could not be attached. Attach them now that the models are in.
  self.world.query_builder<MeshComponent>().build().each([&self](flecs::entity e, MeshComponent& mc) {
    if (mc.model_uuid && !self.entity_to_mesh_instance_map.contains(e)) {
      self.attach_mesh(e, mc.model_uuid, mc.mesh_index, mc.material_uuid);
    }
  });

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
} // namespace ox
