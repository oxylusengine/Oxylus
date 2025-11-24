#pragma once

// clang-format off
#include <expected>

#include "Physics/PhysicsInterfaces.hpp"
#include "Render/DebugRenderer.hpp"

#include <Jolt/Core/JobSystemThreadPool.h>
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

  auto new_system() const -> std::unique_ptr<JPH::PhysicsSystem>;
  auto new_debug_renderer() const -> std::unique_ptr<PhysicsDebugRenderer>;

  auto get_temp_allocator() const -> JPH::TempAllocatorImpl* { return temp_allocator.get(); }
  auto get_job_system() const -> JPH::JobSystemThreadPool* { return job_system.get(); }

private:
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator = nullptr;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system = nullptr;
};
} // namespace ox
