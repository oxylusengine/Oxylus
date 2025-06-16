#pragma once

// clang-format off
#include "Core/ESystem.hpp"
#include "PhysicsInterfaces.hpp"
#include "Render/DebugRenderer.hpp"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace ox {
class RayCast;

class Physics : public ESystem {
public:
  using EntityLayer = uint16_t;

  struct EntityLayerData {
    std::string name = "Layer";
    EntityLayer flags = 0xFFFF;
    uint8_t index = 1;
  };

  // TODO: Make a way to add/change layers via editor and api
  std::map<EntityLayer, EntityLayerData> layer_collision_mask = {
      {BIT(0), {"Static", static_cast<uint16_t>(0xFFFF), 0}},
      {BIT(1), {"Default", static_cast<uint16_t>(0xFFFF), 1}},
      {BIT(2), {"Player", static_cast<uint16_t>(0xFFFF), 2}},
      {BIT(3), {"Sensor", static_cast<uint16_t>(0xFFFF), 3}},
  };

  static constexpr uint32_t MAX_BODIES = 1024;
  static constexpr uint32_t MAX_BODY_PAIRS = 1024;
  static constexpr uint32_t MAX_CONTACT_CONSTRAINS = 1024;
  BPLayerInterfaceImpl layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter_interface;
  ObjectLayerPairFilterImpl object_layer_pair_filter_interface;

  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;
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
