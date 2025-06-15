#pragma once

#include "Core/UUID.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/RenderPipeline.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneGPU.hpp"

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

  bool meshes_dirty = false;
  std::vector<GPU::TransformID> dirty_transforms = {};
  SlotMap<GPU::Transforms, GPU::TransformID> transforms = {};
  ankerl::unordered_dense::map<flecs::entity, GPU::TransformID> entity_transforms_map = {};
  ankerl::unordered_dense::map<std::pair<UUID, usize>, std::vector<GPU::TransformID>> rendering_meshes_map = {};

  explicit Scene(const Shared<RenderPipeline>& render_pipeline = nullptr);
  explicit Scene(const std::string& name);

  ~Scene();

  auto init(this Scene& self, const std::string& name, const Shared<RenderPipeline>& render_pipeline = nullptr) -> void;

  auto runtime_start() -> void;
  auto runtime_stop() -> void;
  auto runtime_update(const Timestep& delta_time) -> void;

  auto disable_phases(const std::vector<flecs::entity_t>& phases) -> void;
  auto enable_all_phases() -> void;

  auto is_running() const -> bool { return running; }

  auto create_entity(const std::string& name = "") const -> flecs::entity;

  auto create_mesh_entity(const UUID& asset_uuid) -> flecs::entity;
  auto attach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool;
  auto detach_mesh(this Scene& self, flecs::entity entity, const UUID& mesh_uuid, usize mesh_index) -> bool;

  auto on_render(vuk::Extent3D extent, vuk::Format format) -> void;

  static auto copy(const Shared<Scene>& src_scene) -> Shared<Scene>;

  auto get_world_transform(flecs::entity entity) const -> glm::mat4;
  auto get_local_transform(flecs::entity entity) const -> glm::mat4;

  auto get_entity_transform_id(flecs::entity entity) const -> option<GPU::TransformID>;
  auto get_entity_transform(GPU::TransformID transform_id) const -> const GPU::Transforms*;

  auto set_dirty(this Scene& self, flecs::entity entity) -> void;

  // Physics interfaces
  auto on_contact_added(const JPH::Body& body1,
                        const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        const JPH::ContactSettings& settings) -> void;
  auto on_contact_persisted(const JPH::Body& body1,
                            const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            const JPH::ContactSettings& settings) -> void;

  auto create_rigidbody(flecs::entity entity, const TransformComponent& transform, RigidbodyComponent& component)
      -> void;
  auto create_character_controller(const TransformComponent& transform, CharacterControllerComponent& component) const
      -> void;

  auto get_render_pipeline() -> RenderPipeline* { return _render_pipeline.get(); }

  auto save_to_file(this const Scene& self, std::string path) -> bool;
  auto load_from_file(this Scene& self, const std::string& path) -> bool;

private:
  bool running = false;

  auto add_transform(this Scene& self, flecs::entity entity) -> GPU::TransformID;
  auto remove_transform(this Scene& self, flecs::entity entity) -> void;

  // Renderer
  Shared<RenderPipeline> _render_pipeline = nullptr;

  // Physics
  Physics3DContactListener* contact_listener_3d;
  Physics3DBodyActivationListener* body_activation_listener_3d;
};
} // namespace ox
