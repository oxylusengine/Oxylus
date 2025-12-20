#include "Physics/Physics.hpp"

#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/RegisterTypes.h>
#include <cstdarg>

#include "Physics/RayCast.hpp"
#include "Utils/Log.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
static void TraceImpl(const char* inFMT, ...) {
  va_list list;
  va_start(list, inFMT);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), inFMT, list);
  va_end(list);

  OX_LOG_INFO("{}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine) {
  OX_LOG_ERROR("{0}:{1}:{2} {3}", inFile, inLine, inExpression, inMessage != nullptr ? inMessage : "");
  return true;
};
#endif

auto Physics::init(this Physics& self) -> std::expected<void, std::string> {
  ZoneScoped;

  // TODO: Override default allocators with Oxylus allocators.
  JPH::RegisterDefaultAllocator();

  // Install callbacks
  JPH::Trace = TraceImpl;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = AssertFailedImpl;
#endif

  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();

  self.temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

  self.job_system = std::make_unique<JPH::JobSystemThreadPool>();
  self.job_system->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, (int)std::thread::hardware_concurrency() - 1);

  return {};
}

auto Physics::deinit(this Physics& self) -> std::expected<void, std::string> {
  ZoneScoped;

  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  return {};
}

auto Physics::new_system(this const Physics& self) -> std::unique_ptr<JPH::PhysicsSystem> {
  ZoneScoped;

  auto sys = std::make_unique<JPH::PhysicsSystem>();
  sys->Init(
    MAX_BODIES,
    0,
    MAX_BODY_PAIRS,
    MAX_CONTACT_CONSTRAINS,
    self.layer_interface,
    self.object_vs_broad_phase_layer_filter_interface,
    self.object_layer_pair_filter_interface
  );

  return sys;
}

auto Physics::new_debug_renderer(this const Physics& self) -> std::unique_ptr<PhysicsDebugRenderer> {
  ZoneScoped;

  return std::make_unique<PhysicsDebugRenderer>();
}
} // namespace ox
