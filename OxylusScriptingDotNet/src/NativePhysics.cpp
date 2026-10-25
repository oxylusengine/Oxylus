// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <ankerl/unordered_dense.h>
#include <cmath>
#include <flecs.h>

#include "DotNetHosts.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"

namespace ox {
// a ray longer than this is as good as unbounded, and keeps an infinite distance out of jolt's math
constexpr auto MAX_RAY_DISTANCE = 100'000.0f;

static auto scenes = ankerl::unordered_dense::map<ecs_world_t*, Scene*>{};

static auto to_jolt(const OxVec3& v) -> JPH::Vec3 { return JPH::Vec3(v.x, v.y, v.z); }

static auto to_ox(JPH::Vec3Arg v) -> OxVec3 { return OxVec3{.x = v.GetX(), .y = v.GetY(), .z = v.GetZ()}; }

static auto to_ox(JPH::QuatArg q) -> OxQuat {
  return OxQuat{.x = q.GetX(), .y = q.GetY(), .z = q.GetZ(), .w = q.GetW()};
}

static auto physics_of(ecs_world_t* world) -> JPH::PhysicsSystem* {
  const auto it = scenes.find(world);
  return it == scenes.end() ? nullptr : it->second->get_physics_system();
}

static auto body_of(ecs_world_t* world, u64 entity) -> JPH::BodyID {
  if (entity == 0 || !ecs_is_alive(world, entity)) {
    return {};
  }

  const auto* rigid_body = flecs::entity(world, entity).try_get<RigidBodyComponent>();
  if (!rigid_body || !rigid_body->runtime_body) {
    return {};
  }

  return static_cast<const JPH::Body*>(rigid_body->runtime_body)->GetID();
}

static auto character_of(ecs_world_t* world, u64 entity) -> JPH::Character* {
  if (entity == 0 || !ecs_is_alive(world, entity)) {
    return nullptr;
  }

  const auto* controller = flecs::entity(world, entity).try_get<CharacterControllerComponent>();
  return controller ? static_cast<JPH::Character*>(controller->character) : nullptr;
}

class LayerMaskFilter final : public JPH::ObjectLayerFilter {
public:
  explicit LayerMaskFilter(u32 mask) : mask_(mask) {}

  auto ShouldCollide(JPH::ObjectLayer layer) const -> bool override {
    return layer < 32 && ((mask_ >> layer) & 1) != 0;
  }

private:
  u32 mask_ = 0;
};

class IgnoreBodyFilter final : public JPH::BodyFilter {
public:
  explicit IgnoreBodyFilter(JPH::BodyID ignored) : ignored_(ignored) {}

  auto ShouldCollide(const JPH::BodyID& body) const -> bool override { return body != ignored_; }

private:
  JPH::BodyID ignored_ = {};
};

static auto physics_raycast(ecs_world_t* world, const OxRay* ray, OxRayHit* hit) -> bool {
  ZoneScoped;

  *hit = {};
  auto* physics = physics_of(world);
  const auto direction = to_jolt(ray->direction);
  if (!physics || direction.LengthSq() < 1.0e-12f) {
    return false;
  }

  const auto distance = std::isfinite(ray->max_distance) && ray->max_distance > 0.0f
                          ? std::min(ray->max_distance, MAX_RAY_DISTANCE)
                          : MAX_RAY_DISTANCE;
  const auto cast = JPH::RRayCast(to_jolt(ray->origin), direction.Normalized() * distance);
  auto result = JPH::RayCastResult();
  const auto layer_filter = LayerMaskFilter(ray->layer_mask);
  const auto body_filter = IgnoreBodyFilter(body_of(world, ray->ignore_entity));
  if (!physics->GetNarrowPhaseQuery().CastRay(cast, result, {}, layer_filter, body_filter)) {
    return false;
  }

  const auto point = cast.GetPointOnRay(result.mFraction);
  const auto lock = JPH::BodyLockRead(physics->GetBodyLockInterface(), result.mBodyID);
  if (!lock.Succeeded()) {
    return false;
  }

  const auto& body = lock.GetBody();
  *hit = OxRayHit{
    .entity = body.GetUserData(),
    .point = to_ox(point),
    .distance = result.mFraction * distance,
    .normal = to_ox(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point)),
  };
  return true;
}

static auto physics_body_state(ecs_world_t* world, u64 entity, OxBodyState* state) -> OxStatus {
  *state = {};
  auto* physics = physics_of(world);
  const auto body = body_of(world, entity);
  if (!physics || body.IsInvalid()) {
    return OxStatus::NotFound;
  }

  const auto& bodies = physics->GetBodyInterface();
  *state = OxBodyState{
    .rotation = to_ox(bodies.GetRotation(body)),
    .position = to_ox(bodies.GetPosition(body)),
    .is_active = bodies.IsActive(body) ? 1_u32 : 0_u32,
    .linear_velocity = to_ox(bodies.GetLinearVelocity(body)),
    .angular_velocity = to_ox(bodies.GetAngularVelocity(body)),
  };
  return OxStatus::Ok;
}

static auto physics_body_command(ecs_world_t* world, u64 entity, OxBodyCommand command, const OxVec3* value)
  -> OxStatus {
  ZoneScoped;

  auto* physics = physics_of(world);
  const auto body = body_of(world, entity);
  if (!physics || body.IsInvalid()) {
    return OxStatus::NotFound;
  }

  auto& bodies = physics->GetBodyInterface();
  const auto motion = bodies.GetMotionType(body);
  const auto vector = to_jolt(*value);
  // jolt asserts instead of failing, so what it would reject is turned away here: a static body has no velocity,
  // and only a dynamic one responds to forces
  switch (command) {
    case OxBodyCommand::SetLinearVelocity:
    case OxBodyCommand::SetAngularVelocity:
      if (motion == JPH::EMotionType::Static) {
        return OxStatus::InvalidArgument;
      }

      break;
    case OxBodyCommand::AddForce  :
    case OxBodyCommand::AddImpulse:
    case OxBodyCommand::AddTorque :
    case OxBodyCommand::AddAngularImpulse:
      if (motion != JPH::EMotionType::Dynamic) {
        return OxStatus::InvalidArgument;
      }

      break;
    case OxBodyCommand::Activate  :
    case OxBodyCommand::Deactivate: break;
  }

  switch (command) {
    case OxBodyCommand::SetLinearVelocity : bodies.SetLinearVelocity(body, vector); break;
    case OxBodyCommand::SetAngularVelocity: bodies.SetAngularVelocity(body, vector); break;
    case OxBodyCommand::AddForce          : bodies.AddForce(body, vector); break;
    case OxBodyCommand::AddImpulse        : bodies.AddImpulse(body, vector); break;
    case OxBodyCommand::AddTorque         : bodies.AddTorque(body, vector); break;
    case OxBodyCommand::AddAngularImpulse : bodies.AddAngularImpulse(body, vector); break;
    case OxBodyCommand::Activate          : bodies.ActivateBody(body); break;
    case OxBodyCommand::Deactivate        : bodies.DeactivateBody(body); break;
  }

  return OxStatus::Ok;
}

static auto physics_character_state(ecs_world_t* world, u64 entity, OxCharacterState* state) -> OxStatus {
  *state = {};
  auto* character = physics_of(world) ? character_of(world, entity) : nullptr;
  if (!character) {
    return OxStatus::NotFound;
  }

  *state = OxCharacterState{
    .position = to_ox(character->GetPosition()),
    .ground_state = static_cast<u32>(character->GetGroundState()),
    .linear_velocity = to_ox(character->GetLinearVelocity()),
    .ground_normal = to_ox(character->GetGroundNormal()),
  };
  return OxStatus::Ok;
}

static auto physics_character_command(ecs_world_t* world, u64 entity, OxCharacterCommand command, const OxVec3* value)
  -> OxStatus {
  ZoneScoped;

  auto* character = physics_of(world) ? character_of(world, entity) : nullptr;
  if (!character) {
    return OxStatus::NotFound;
  }

  const auto vector = to_jolt(*value);
  switch (command) {
    case OxCharacterCommand::SetLinearVelocity: character->SetLinearVelocity(vector); break;
    case OxCharacterCommand::AddLinearVelocity: character->AddLinearVelocity(vector); break;
    case OxCharacterCommand::AddImpulse       : character->AddImpulse(vector); break;
    case OxCharacterCommand::SetPosition      : character->SetPosition(vector); break;
  }

  return OxStatus::Ok;
}

auto set_scene(ecs_world_t* world, Scene* scene) -> void {
  if (scene) {
    scenes[world] = scene;
  } else {
    scenes.erase(world);
  }
}

auto fill_physics_api(OxNativeApi& api) -> void {
  api.physics_raycast = physics_raycast;
  api.physics_body_state = physics_body_state;
  api.physics_body_command = physics_body_command;
  api.physics_character_state = physics_character_state;
  api.physics_character_command = physics_character_command;
}
} // namespace ox
