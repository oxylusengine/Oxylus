#include "Physics/Physics.hpp"

#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/RegisterTypes.h>
#include <cstdarg>

#include "Core/App.hpp"
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

class JoltJobSystem final : public JPH::JobSystemWithBarrier {
public:
  auto GetMaxConcurrency() const -> int override {
    auto& job_man = App::get_job_manager();
    return job_man.get_thread_count();
  }

  auto CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 num_dependencies = 0)
    -> JobHandle override {
    Job* job = new Job(name, color, this, fn, num_dependencies);
    JobHandle handle(job);
    if (num_dependencies == 0)
      QueueJob(job);
    return handle;
  }

  auto FreeJob(Job* job) -> void override { delete job; }

protected:
  auto QueueJob(Job* job) -> void override {
    auto& job_man = App::get_job_manager();
    job->AddRef();
    job_man.submit(ox::Job::create([job] {
      job->Execute();
      job->Release();
    }));
  }

  auto QueueJobs(Job** jobs, JPH::uint num_jobs) -> void override {
    for (JPH::uint i = 0; i < num_jobs; ++i)
      QueueJob(jobs[i]);
  }
};

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

  self.job_system = std::make_unique<JoltJobSystem>();
  self.job_system->Init(JPH::cMaxPhysicsBarriers);

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
