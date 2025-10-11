#pragma once

// clang-format off
#include <expected>

#include "Physics/PhysicsInterfaces.hpp"
#include "Render/DebugRenderer.hpp"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace ox {
class RayCast;

class Physics {
public:
  constexpr static auto MODULE_NAME = "Physics";

  static constexpr uint32_t MAX_BODIES = 1024;
  static constexpr uint32_t MAX_BODY_PAIRS = 1024;
  static constexpr uint32_t MAX_CONTACT_CONSTRAINS = 1024;
  BPLayerInterfaceImpl layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter_interface;
  ObjectLayerPairFilterImpl object_layer_pair_filter_interface;

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;
  void step(float physicsTs);
  void debug_draw();

  JPH::PhysicsSystem* get_physics_system() { return physics_system; };
  JPH::BodyInterface& get_body_interface() { return physics_system->GetBodyInterface(); }
  const JPH::BroadPhaseQuery& get_broad_phase_query() { return physics_system->GetBroadPhaseQuery(); }
  const JPH::BodyLockInterface& get_body_interface_lock() { return physics_system->GetBodyLockInterface(); }
  PhysicsDebugRenderer* get_debug_renderer() { return debug_renderer; }

  JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector> cast_ray(const RayCast& ray_cast);

private:
  JPH::PhysicsSystem* physics_system = nullptr;
  JPH::TempAllocatorImpl* temp_allocator = nullptr;
  JPH::JobSystemThreadPool* job_system = nullptr;
  PhysicsDebugRenderer* debug_renderer = nullptr;
};
} // namespace ox
