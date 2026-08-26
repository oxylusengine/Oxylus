#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Core.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <simdjson.h>

#include "Asset/Model.hpp"
#include "Asset/ParticleSystem.hpp"
#include "Core/UUID.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererCVar.hpp"
#include "Render/RendererInstance.hpp"
#include "Scene/Components.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scene/Terrain.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/Timestep.hpp"

namespace Rml {
class Context;
}

template <>
struct ankerl::unordered_dense::hash<flecs::id> {
  using is_avalanching = void;
  u64 operator()(const flecs::id& v) const noexcept {
    return ankerl::unordered_dense::detail::wyhash::hash(&v, sizeof(flecs::id));
  }
};

template <>
struct ankerl::unordered_dense::hash<flecs::entity> {
  using is_avalanching = void;
  u64 operator()(const flecs::entity& v) const noexcept {
    return ankerl::unordered_dense::detail::wyhash::hash(&v, sizeof(flecs::entity));
  }
};

namespace ox {
struct JsonWriter;
class RmlView;

struct ComponentDB {
  std::vector<flecs::id> components = {};
  std::vector<flecs::entity> imported_modules = {};

  auto import_module(this ComponentDB&, flecs::entity module) -> void;
  auto is_component_known(this ComponentDB&, flecs::id component_id) -> bool;
  auto get_components(this ComponentDB&) -> std::span<flecs::id>;
};

enum class SceneID : u64 { Invalid = std::numeric_limits<u64>::max() };
class Scene {
public:
  std::string scene_name = "Untitled";

  bool tearing_down = false;

  flecs::world world;
  ComponentDB component_db = {};

  f32 physics_interval = 1.f / 60.f; // used only on initialization

  std::vector<GPU::TransformID> dirty_transforms = {};
  std::vector<MeshInstanceID> dirty_mesh_instances = {};
  SlotMap<GPU::Transforms, GPU::TransformID> transforms = {};
  ankerl::unordered_dense::map<flecs::entity, GPU::TransformID> entity_transforms_map = {};
  ankerl::unordered_dense::map<u32, flecs::entity> transform_index_entities_map = {};

  bool input_focused = true;

  RendererCVar renderer_cvar = {};

  SlotMap<MeshInstance, MeshInstanceID> mesh_instances = {};
  ankerl::unordered_dense::map<flecs::entity, MeshInstanceID> entity_to_mesh_instance_map = {};

  SlotMap<GPU::Light, GPU::LightID> lights = {};

  SlotMap<ParticleEmitterState, ParticleEmitterID> particle_emitters = {};
  ankerl::unordered_dense::map<flecs::entity, ParticleEmitterID> entity_particle_emitters_map = {};

  std::unique_ptr<Terrain> terrain = nullptr;
  flecs::entity terrain_entity = {};
  bool terrain_dirty = false;
  UUID terrain_edits_ref = {};

  bool meshes_dirty = false;
  u32 gpu_mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;

  explicit Scene(const std::string& name = "Untitled");

  ~Scene();

  auto init(this Scene& self, const std::string& name) -> void;

  auto physics_init(this Scene& self) -> void;
  auto physics_deinit(this Scene& self) -> void;

  auto runtime_start(this Scene& self) -> void;
  auto runtime_stop(this Scene& self) -> void;
  auto runtime_update(this Scene& self, const Timestep& delta_time) -> void;

  auto prepare_render(this Scene& self) -> void;

  auto defer_function(this Scene& self, const std::function<void(Scene* scene)>& func) -> void;

  auto disable_phases(const std::vector<flecs::entity_t>& phases) -> void;
  auto enable_all_phases() -> void;

  auto is_running() const -> bool { return running; }

  auto create_entity(const std::string& name = "", bool safe_naming = false) const -> flecs::entity;

  auto create_model_entity(this Scene& self, const UUID& asset_uuid) -> flecs::entity;

  auto create_model_entity_async(this Scene& self, const UUID& asset_uuid) -> void;

  auto attach_mesh(
    this Scene& self, flecs::entity entity, const UUID& model_uuid, usize mesh_index, const UUID& material_uuid = {}
  ) -> bool;
  auto detach_mesh(this Scene& self, flecs::entity entity) -> bool;

  // --- Particles ---
  // Runtime control over an entity's emitter. Stopping only halts spawning; particles already alive
  // keep simulating until their lifetime runs out.
  auto particle_emitter_state(this Scene& self, flecs::entity entity) -> ParticleEmitterState*;
  auto play_particles(this Scene& self, flecs::entity entity) -> void;
  auto stop_particles(this Scene& self, flecs::entity entity) -> void;
  // Rewinds emitter time and clears trigger state, so Once nodes and the duration window fire again.
  // Live particles are untouched.
  auto restart_particles(this Scene& self, flecs::entity entity) -> void;
  auto is_particles_playing(this Scene& self, flecs::entity entity) -> bool;
  // Queues an extra spawn for the next frame. An emitter graph holding a Pulse node receives the
  // count and decides what it means; without one the particles are spawned directly, whether or not
  // the emitter is playing.
  auto emit_particle_burst(this Scene& self, flecs::entity entity, u32 count) -> void;
  auto set_particle_parameter(this Scene& self, flecs::entity entity, u32 index, const glm::vec4& value) -> void;
  // Resolves the name against the asset's parameter list. Returns false when no such parameter exists.
  auto set_particle_parameter(this Scene& self, flecs::entity entity, std::string_view name, const glm::vec4& value)
    -> bool;

  static auto copy(const std::shared_ptr<Scene>& src_scene) -> std::shared_ptr<Scene>;

  static auto get_world_position(flecs::entity entity) -> glm::vec3;
  static auto get_world_transform(flecs::entity entity) -> glm::mat4;
  static auto get_local_transform(flecs::entity entity) -> glm::mat4;

  auto get_entity_transform_id(flecs::entity entity) const -> option<GPU::TransformID>;
  auto get_entity_transform(GPU::TransformID transform_id) const -> const GPU::Transforms*;

  auto set_dirty(this Scene& self, flecs::entity entity) -> void;

  auto safe_entity_name(this const Scene& self, std::string prefix, flecs::entity parent = {}) -> std::string;

  auto get_lua_system(this const Scene& self, const UUID& lua_script) -> LuaSystem*;
  auto get_lua_systems(this const Scene& self) -> const ankerl::unordered_dense::map<UUID, std::unique_ptr<LuaSystem>>&;
  auto add_lua_system(this Scene& self, const UUID& lua_script) -> void;
  auto remove_lua_system(this Scene& self, const UUID& lua_script) -> void;

  // Physics
  auto get_physics_system(this const Scene& self) -> JPH::PhysicsSystem*;
  auto cast_ray(this const Scene& self, const RayCast& ray_cast)
    -> JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector>;
  auto on_contact_added(
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void;
  auto on_contact_persisted(
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void;
  auto on_contact_removed(const JPH::SubShapeIDPair& sub_shape_pair) -> void;

  auto on_body_activated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void;
  auto on_body_deactivated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void;

  // Both read the entity's world transform, so neither takes a TransformComponent: a body on a child
  // entity has to be placed in world space, not at its local offset.
  auto create_rigidbody(this Scene& self, flecs::entity entity, RigidBodyComponent& component) -> void;
  auto create_character_controller(flecs::entity entity, CharacterControllerComponent& component) const -> void;

  auto create_terrain_collision(this Scene& self) -> void;
  auto destroy_terrain_collision(this Scene& self) -> void;
  auto sync_terrain_edits(this Scene& self) -> void;
  auto set_terrain_edits_ref(this Scene& self, const UUID& uuid) -> void;
  auto clear_terrain_edits(this Scene& self) -> void;

  // Needs the chassis rigidbody to exist already, so it runs after create_rigidbody. Wheels are read
  // from child entities carrying VehicleWheelComponent, in hierarchy order.
  auto create_vehicle(this Scene& self, flecs::entity entity, VehicleComponent& component) -> void;
  auto destroy_vehicle(this Scene& self, VehicleComponent& component) -> void;

  auto render(
    this Scene& self,
    vuk::Value<vuk::ImageAttachment>&& dst_attachment,
    glm::ivec2 viewport_origin,
    glm::ivec2 viewport_size,
    glm::ivec2 surface_size,
    bool keyboard_focused = true
  ) -> vuk::Value<vuk::ImageAttachment>;
  auto get_renderer_instance() const -> RendererInstance* { return renderer_instance.get(); }
  auto get_rml_context(this const Scene& self) -> Rml::Context*;
  auto get_rml_context_name(this const Scene& self) -> std::string_view;
  auto set_rml_dpi_ratio(this const Scene& self, f32 ratio) -> void;

  static auto entity_to_json(JsonWriter& writer, flecs::entity e) -> void;
  static auto json_to_entity(
    Scene& self, //
    flecs::entity root,
    simdjson::ondemand::value& json,
    std::vector<UUID>& requested_assets
  ) -> flecs::entity;

  auto to_json(this const Scene& self) -> JsonWriter;
  auto from_json(this Scene& self, const std::string& json) -> bool;
  auto save_to_file(this const Scene& self, const std::filesystem::path& path) -> bool;
  auto load_from_file(this Scene& self, const std::filesystem::path& path) -> bool;

  auto get_uuid(this const Scene& self) -> const UUID& { return self.uuid; }

private:
  UUID uuid = {};

  struct PendingModelSpawn {
    struct MeshEntity {
      usize mesh_index = 0;
      usize mesh_group_index = 0;
      flecs::entity parent = {};
    };

    UUID model_uuid = {};
    std::vector<MeshEntity> mesh_entities = {};
    bool hierarchy_spawned = false;
  };

  std::vector<PendingModelSpawn> pending_model_spawns = {};

  bool running = false;
  bool deserializing_entity = false;

  std::vector<std::function<void(Scene* scene)>> deferred_functions_ = {};

  // Lua. Owned per scene, not borrowed from the asset: a shared instance means two scenes share one environment.
  ankerl::unordered_dense::map<UUID, std::unique_ptr<LuaSystem>> lua_systems = {};

  // Renderer
  std::unique_ptr<RendererInstance> renderer_instance = nullptr;
  std::unique_ptr<RmlView> rml_view;
  glm::ivec2 rml_surface_size = {};

  // Physics
  std::shared_mutex physics_mutex = {};
  std::unique_ptr<JPH::PhysicsSystem> physics_system = nullptr;
  std::unique_ptr<PhysicsDebugRenderer> physics_debug_renderer = nullptr;
  std::unique_ptr<Physics3DContactListener> contact_listener_3d = nullptr;
  std::unique_ptr<Physics3DBodyActivationListener> body_activation_listener_3d = nullptr;
  JPH::BodyID terrain_body_id = {};

  auto add_transform(this Scene& self, flecs::entity entity) -> GPU::TransformID;
  auto remove_transform(this Scene& self, flecs::entity entity) -> void;

  struct MeshSpawnInfo {
    usize mesh_index = 0;
    flecs::entity parent = {};
    std::string name = {};

    UUID material_uuid = {};
    AABB aabb = {};
  };

  auto spawn_model_hierarchy(this Scene& self, Model& model, PendingModelSpawn& spawn) -> flecs::entity;
  auto resolve_mesh_spawn(this Scene& self, Model& model, const PendingModelSpawn::MeshEntity& mesh_entity)
    -> MeshSpawnInfo;
  auto spawn_model_mesh_entity(this Scene& self, const UUID& model_uuid, const MeshSpawnInfo& info) -> void;
  auto update_pending_model_spawns(this Scene& self) -> void;

  auto bake_terrain(this Scene& self) -> void;

  auto run_deferred_functions(this Scene& self) -> void;
};
} // namespace ox
