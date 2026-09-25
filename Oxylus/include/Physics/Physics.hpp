#pragma once

// clang-format off
#include <expected>

#include "Physics/PhysicsInterfaces.hpp"
#include "Physics/PhysicsDebugRenderer.hpp"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace ox {
class RayCast;
class Timestep;

class Physics {
public:
  constexpr static auto MODULE_NAME = "Physics";

  // Jolt preallocates from these, so they are hard caps: exceeding max_bodies fatals in
  // create_rigidbody. Raise them before creating a scene if the world is dense.
  struct SystemLimits {
    u32 max_bodies = 10240;
    u32 max_body_pairs = 10240;
    u32 max_contact_constraints = 10240;
  };
  SystemLimits limits = {};
  BPLayerInterfaceImpl layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter_interface;
  ObjectLayerPairFilterImpl object_layer_pair_filter_interface;
  std::unique_ptr<PhysicsDebugRenderer> debug_renderer = nullptr;

  auto init(this Physics& self) -> std::expected<void, std::string>;
  auto deinit(this Physics& self) -> std::expected<void, std::string>;
  auto update(this Physics& self, const Timestep& timestep) -> void;

  auto new_system(this const Physics& self) -> std::unique_ptr<JPH::PhysicsSystem>;

  auto get_temp_allocator(this const Physics& self) -> JPH::TempAllocatorImpl* { return self.temp_allocator.get(); }
  auto get_job_system(this const Physics& self) -> JPH::JobSystemWithBarrier* { return self.job_system.get(); }

private:
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator = nullptr;
  std::unique_ptr<JPH::JobSystemWithBarrier> job_system = nullptr;
};
} // namespace ox
