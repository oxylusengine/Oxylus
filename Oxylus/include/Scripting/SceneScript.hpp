#pragma once

#include <expected>
#include <glm/vec3.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace JPH {
class ContactSettings;
class ContactManifold;
class Body;
class BodyID;
class SubShapeIDPair;
} // namespace JPH

namespace vuk {
struct Extent3D;
}

namespace ox {
class Scene;

// a lua script is keyed by its asset, a managed one by its type name, never both
struct ScriptKey {
  UUID asset_uuid = {};
  std::string type_name = {};

  auto operator==(const ScriptKey&) const -> bool = default;
};

enum class ContactEvent : u32 {
  Added = 0,
  Persisted = 1,
  Removed = 2,
};

// blittable, C# reads it as is. Entities are flecs ids, 0 when the body belongs to none. The point and normal are in
// world space, the normal points from entity1 into entity2, and a removed contact carries neither
struct PhysicsContact {
  u64 entity1 = 0;
  u64 entity2 = 0;
  glm::vec3 point = {};
  f32 penetration_depth = 0.0f;
  glm::vec3 normal = {};
  ContactEvent event = ContactEvent::Added;
};

// one scene's live instance of a script, whichever language it is written in
class SceneScript {
public:
  virtual ~SceneScript() = default;

  virtual auto display_name() const -> std::string = 0;

  virtual auto on_add(Scene*) -> void {}
  virtual auto on_remove(Scene*) -> void {}

  virtual auto on_scene_start(Scene*) -> void {}
  virtual auto on_scene_stop(Scene*) -> void {}
  virtual auto on_scene_update(Scene*, f32) -> void {}
  // once per physics step, before it, with the fixed step length
  virtual auto on_scene_fixed_update(Scene*, f32) -> void {}
  // defined out of line, a default body would need vuk::Extent3D complete in every includer
  virtual auto on_scene_render(Scene*, vuk::Extent3D) -> void;

  // called from jolt's worker threads with the scene's physics_mutex held
  virtual auto on_contact_added(
    Scene*, const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&, const JPH::ContactSettings&
  ) -> void {}
  virtual auto on_contact_persisted(
    Scene*, const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&, const JPH::ContactSettings&
  ) -> void {}
  virtual auto on_contact_removed(Scene*, const JPH::SubShapeIDPair&) -> void {}

  // the same contacts, buffered during a physics step and handed over on the main thread after it
  virtual auto on_contacts(Scene*, std::span<const PhysicsContact>) -> void {}

  virtual auto on_body_activated(Scene*, const JPH::BodyID&, u64) -> void {}
  virtual auto on_body_deactivated(Scene*, const JPH::BodyID&, u64) -> void {}
};

// how a scripting module Oxylus doesn't link against plugs into scenes. Its runtime is created with every scene and
// gets each hook before the scene's scripts do, and it creates the scripts a scene names by type
class SceneScriptHost {
public:
  virtual ~SceneScriptHost() = default;

  virtual auto create_runtime(Scene& scene) -> std::unique_ptr<SceneScript> = 0;
  virtual auto create_script(Scene& scene, std::string_view type_name)
    -> std::expected<std::unique_ptr<SceneScript>, std::string> = 0;
};
} // namespace ox

template <>
struct ankerl::unordered_dense::hash<ox::ScriptKey> {
  using is_avalanching = void;
  auto operator()(const ox::ScriptKey& key) const noexcept -> u64 {
    const auto uuid_hash = ankerl::unordered_dense::hash<ox::UUID>{}(key.asset_uuid);
    const auto name_hash = ankerl::unordered_dense::detail::wyhash::hash(key.type_name.data(), key.type_name.size());
    return ankerl::unordered_dense::detail::wyhash::mix(uuid_hash, name_hash);
  }
};
