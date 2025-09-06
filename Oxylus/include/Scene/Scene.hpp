#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Core.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <simdjson.h>

#include "Core/UUID.hpp"
#include "Render/RendererInstance.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/Timestep.hpp"
// clang-format on

template <>
struct ankerl::unordered_dense::hash<flecs::id> {
  using is_avalanching = void;
  ox::u64 operator()(const flecs::id& v) const noexcept {
    return ankerl::unordered_dense::detail::wyhash::hash(&v, sizeof(flecs::id));
  }
};

template <>
struct ankerl::unordered_dense::hash<flecs::entity> {
  using is_avalanching = void;
  ox::u64 operator()(const flecs::entity& v) const noexcept {
    return ankerl::unordered_dense::detail::wyhash::hash(&v, sizeof(flecs::entity));
  }
};

namespace ox {
struct JsonWriter;
class Physics3DContactListener;
class Physics3DBodyActivationListener;

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

  flecs::world world;
  ComponentDB component_db = {};

  f32 physics_interval = 1.f / 60.f; // used only on initalization

  std::vector<GPU::TransformID> dirty_transforms = {};
  SlotMap<GPU::Transforms, GPU::TransformID> transforms = {};
  ankerl::unordered_dense::map<flecs::entity, GPU::TransformID> entity_transforms_map = {};
  ankerl::unordered_dense::map<std::pair<UUID, usize>, std::vector<GPU::TransformID>> rendering_meshes_map = {};

  std::vector<GPU::Material> gpu_materials = {};

  bool meshes_dirty = false;
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;

  explicit Scene(const std::string& name = "Untitled");

  ~Scene();

  auto init(this Scene& self, const std::string& name) -> void;

  auto physics_init(this Scene& self) -> void;
  auto physics_deinit(this Scene& self) -> void;

  auto runtime_start(this Scene& self) -> void;
  auto runtime_stop(this Scene& self) -> void;
  auto runtime_update(this Scene& self, const Timestep& delta_time) -> void;

  auto defer_function(this Scene& self, const std::function<void(Scene* scene)>& func) -> void;

  auto disable_phases(const std::vector<flecs::entity_t>& phases) -> void;
  auto enable_all_phases() -> void;

  auto is_running() const -> bool { return running; }

  auto create_entity(const std::string& name = "", bool safe_naming = false) const -> flecs::entity;

  auto create_mesh_entity(this Scene& self, const UUID& asset_uuid) -> flecs::entity;
  auto attach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool;
  auto detach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool;

  auto on_render(vuk::Extent3D extent, vuk::Format format) -> void;
  auto on_viewport_render(vuk::Extent3D extent, vuk::Format format) -> void;

  static auto copy(const std::shared_ptr<Scene>& src_scene) -> std::shared_ptr<Scene>;

  static auto get_world_position(flecs::entity entity) -> glm::vec3;
  static auto get_world_transform(flecs::entity entity) -> glm::mat4;
  static auto get_local_transform(flecs::entity entity) -> glm::mat4;

  auto get_entity_transform_id(flecs::entity entity) const -> option<GPU::TransformID>;
  auto get_entity_transform(GPU::TransformID transform_id) const -> const GPU::Transforms*;

  auto set_dirty(this Scene& self, flecs::entity entity) -> void;

  auto safe_entity_name(this const Scene& self, std::string prefix) -> std::string;

  auto get_lua_system(this const Scene& self, const UUID& lua_script) -> LuaSystem*;
  auto get_lua_systems(this const Scene& self) -> const ankerl::unordered_dense::map<UUID, LuaSystem*>&;
  auto add_lua_system(this Scene& self, const UUID& lua_script) -> void;
  auto remove_lua_system(this Scene& self, const UUID& lua_script) -> void;

  // Physics interfaces
  auto on_contact_added(const JPH::Body& body1,
                        const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        const JPH::ContactSettings& settings) -> void;
  auto on_contact_persisted(const JPH::Body& body1,
                            const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            const JPH::ContactSettings& settings) -> void;
  auto on_contact_removed(const JPH::SubShapeIDPair& sub_shape_pair) -> void;

  auto on_body_activated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void;
  auto on_body_deactivated(const JPH::BodyID& body_id, JPH::uint64 body_user_data) -> void;

  auto create_rigidbody(flecs::entity entity, const TransformComponent& transform, RigidBodyComponent& component)
      -> void;
  auto create_character_controller(flecs::entity entity, const TransformComponent& transform, CharacterControllerComponent& component) const
      -> void;

  auto get_renderer_instance() -> RendererInstance* { return renderer_instance.get(); }

  static auto entity_to_json(JsonWriter& writer, flecs::entity e) -> void;
  static auto json_to_entity(Scene& self, //
                             flecs::entity root,
                             simdjson::ondemand::value& json,
                             std::vector<UUID>& requested_assets) -> std::pair<flecs::entity, bool>;

  auto save_to_file(this const Scene& self, std::string path) -> bool;
  auto load_from_file(this Scene& self, const std::string& path) -> bool;

private:
  bool running = false;

  std::shared_mutex physics_mutex = {};

  auto add_transform(this Scene& self, flecs::entity entity) -> GPU::TransformID;
  auto remove_transform(this Scene& self, flecs::entity entity) -> void;

  std::vector<std::function<void(Scene* scene)>> deferred_functions_ = {};
  auto run_deferred_functions(this Scene& self) -> void;

  // Lua
  ankerl::unordered_dense::map<UUID, LuaSystem*> lua_systems = {};

  // Renderer
  std::unique_ptr<RendererInstance> renderer_instance = nullptr;

  // Physics
  std::unique_ptr<Physics3DContactListener> contact_listener_3d;
  std::unique_ptr<Physics3DBodyActivationListener> body_activation_listener_3d;
};
} // namespace ox
