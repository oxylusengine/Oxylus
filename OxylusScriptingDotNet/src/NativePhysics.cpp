// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
// clang-format on

#include <ankerl/unordered_dense.h>
#include <cmath>
#include <flecs.h>
#include <utility>

#include "DotNetHosts.hpp"
#include "Physics/PhysicsInterfaces.hpp"
#include "Render/Camera.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"

namespace ox {
// a ray longer than this is as good as unbounded, and keeps an infinite distance out of jolt's math
constexpr auto MAX_RAY_DISTANCE = 100'000.0f;

static auto scenes = ankerl::unordered_dense::map<ecs_world_t*, Scene*>{};

static auto to_jolt(const OxVec3& v) -> JPH::Vec3 { return JPH::Vec3(v.x, v.y, v.z); }

static auto to_jolt(const OxQuat& q) -> JPH::Quat { return JPH::Quat(q.x, q.y, q.z, q.w); }

static auto to_ox(JPH::Vec3Arg v) -> OxVec3 { return OxVec3{.x = v.GetX(), .y = v.GetY(), .z = v.GetZ()}; }

static auto to_ox(JPH::QuatArg q) -> OxQuat {
  return OxQuat{.x = q.GetX(), .y = q.GetY(), .z = q.GetZ(), .w = q.GetW()};
}

static auto is_finite(const glm::vec3& v) -> bool {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static auto physics_of(ecs_world_t* world) -> JPH::PhysicsSystem* {
  auto* scene = scene_of(world);
  return scene ? scene->get_physics_system() : nullptr;
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

static auto vehicle_constraint_of(ecs_world_t* world, u64 entity) -> JPH::VehicleConstraint* {
  if (entity == 0 || !ecs_is_alive(world, entity)) {
    return nullptr;
  }

  const auto* vehicle = flecs::entity(world, entity).try_get<VehicleComponent>();
  return vehicle ? static_cast<JPH::VehicleConstraint*>(vehicle->runtime_constraint) : nullptr;
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

  const auto lock = JPH::BodyLockRead(physics->GetBodyLockInterface(), body);
  if (!lock.Succeeded()) {
    return OxStatus::NotFound;
  }

  const auto& target = lock.GetBody();
  const auto& bounds = target.GetWorldSpaceBounds();
  const auto flag = [](bool is_set, OxBodyFlag bit) {
    return is_set ? std::to_underlying(bit) : 0_u32;
  };
  const auto is_dynamic = target.IsDynamic();
  *state = OxBodyState{
    .rotation = to_ox(target.GetRotation()),
    .position = to_ox(target.GetPosition()),
    .is_active = target.IsActive() ? 1_u32 : 0_u32,
    .linear_velocity = to_ox(target.GetLinearVelocity()),
    .motion_type = static_cast<u32>(target.GetMotionType()),
    .angular_velocity = to_ox(target.GetAngularVelocity()),
    .object_layer = static_cast<u32>(target.GetObjectLayer()),
    .center_of_mass = to_ox(target.GetCenterOfMassPosition()),
    .friction = target.GetFriction(),
    .bounds_min = to_ox(bounds.mMin),
    .restitution = target.GetRestitution(),
    .bounds_max = to_ox(bounds.mMax),
    .flags = flag(target.IsSensor(), OxBodyFlag::IsSensor) |
             flag(target.GetCollideKinematicVsNonDynamic(), OxBodyFlag::CollideKinematicVsNonDynamic) |
             flag(target.GetUseManifoldReduction(), OxBodyFlag::UseManifoldReduction) |
             flag(target.GetApplyGyroscopicForce(), OxBodyFlag::ApplyGyroscopicForce) |
             flag(target.GetEnhancedInternalEdgeRemoval(), OxBodyFlag::EnhancedInternalEdgeRemoval),
    .accumulated_force = is_dynamic ? to_ox(target.GetAccumulatedForce()) : OxVec3{},
    .accumulated_torque = is_dynamic ? to_ox(target.GetAccumulatedTorque()) : OxVec3{},
  };
  return OxStatus::Ok;
}

static auto physics_body_command(ecs_world_t* world, u64 entity, OxBodyCommand command, const OxBodyArgs* args)
  -> OxStatus {
  ZoneScoped;

  auto* physics = physics_of(world);
  const auto body = body_of(world, entity);
  if (!physics || body.IsInvalid()) {
    return OxStatus::NotFound;
  }

  auto& bodies = physics->GetBodyInterface();
  const auto motion = bodies.GetMotionType(body);
  const auto vector = to_jolt(args->vector);
  const auto position = to_jolt(args->position);
  const auto rotation = to_jolt(args->rotation);
  // jolt asserts instead of failing, so what it would reject is turned away here: a static body has no velocity,
  // and only a dynamic one responds to forces
  switch (command) {
    case OxBodyCommand::SetLinearVelocity :
    case OxBodyCommand::SetAngularVelocity:
    case OxBodyCommand::AddLinearVelocity:
      if (motion == JPH::EMotionType::Static) {
        return OxStatus::InvalidArgument;
      }

      break;
    case OxBodyCommand::MoveKinematic:
      if (motion == JPH::EMotionType::Static || !(args->delta_time > 0.0f)) {
        return OxStatus::InvalidArgument;
      }

      break;
    case OxBodyCommand::AddForce          :
    case OxBodyCommand::AddImpulse        :
    case OxBodyCommand::AddTorque         :
    case OxBodyCommand::AddAngularImpulse :
    case OxBodyCommand::AddForceAtPosition:
    case OxBodyCommand::AddImpulseAtPosition:
      if (motion != JPH::EMotionType::Dynamic) {
        return OxStatus::InvalidArgument;
      }

      break;
    case OxBodyCommand::Activate              :
    case OxBodyCommand::Deactivate            :
    case OxBodyCommand::SetPosition           :
    case OxBodyCommand::SetRotation           :
    case OxBodyCommand::SetPositionAndRotation: break;
  }

  const auto is_rotating = command == OxBodyCommand::MoveKinematic || command == OxBodyCommand::SetRotation ||
                           command == OxBodyCommand::SetPositionAndRotation;
  if (is_rotating && !(rotation.LengthSq() > 1.0e-12f)) {
    return OxStatus::InvalidArgument;
  }

  const auto target_rotation = is_rotating ? rotation.Normalized() : JPH::Quat::sIdentity();
  switch (command) {
    case OxBodyCommand::SetLinearVelocity   : bodies.SetLinearVelocity(body, vector); break;
    case OxBodyCommand::SetAngularVelocity  : bodies.SetAngularVelocity(body, vector); break;
    case OxBodyCommand::AddForce            : bodies.AddForce(body, vector); break;
    case OxBodyCommand::AddImpulse          : bodies.AddImpulse(body, vector); break;
    case OxBodyCommand::AddTorque           : bodies.AddTorque(body, vector); break;
    case OxBodyCommand::AddAngularImpulse   : bodies.AddAngularImpulse(body, vector); break;
    case OxBodyCommand::Activate            : bodies.ActivateBody(body); break;
    case OxBodyCommand::Deactivate          : bodies.DeactivateBody(body); break;
    case OxBodyCommand::AddLinearVelocity   : bodies.AddLinearVelocity(body, vector); break;
    case OxBodyCommand::AddForceAtPosition  : bodies.AddForce(body, vector, position); break;
    case OxBodyCommand::AddImpulseAtPosition: bodies.AddImpulse(body, vector, position); break;
    case OxBodyCommand::MoveKinematic: bodies.MoveKinematic(body, position, target_rotation, args->delta_time); break;
    case OxBodyCommand::SetPosition  : bodies.SetPosition(body, position, JPH::EActivation::Activate); break;
    case OxBodyCommand::SetRotation  : bodies.SetRotation(body, target_rotation, JPH::EActivation::Activate); break;
    case OxBodyCommand::SetPositionAndRotation:
      bodies.SetPositionAndRotation(body, position, target_rotation, JPH::EActivation::Activate);
      break;
  }

  return OxStatus::Ok;
}

static auto physics_body_set(ecs_world_t* world, u64 entity, OxBodyProperty property, f32 value) -> OxStatus {
  ZoneScoped;

  auto* physics = physics_of(world);
  const auto body = body_of(world, entity);
  if (!physics || body.IsInvalid()) {
    return OxStatus::NotFound;
  }

  auto& bodies = physics->GetBodyInterface();
  const auto is_set = value != 0.0f;
  const auto is_index = value >= 0.0f && std::floor(value) == value;
  switch (property) {
    case OxBodyProperty::MotionType: {
      if (!is_index || value > 2.0f) {
        return OxStatus::InvalidArgument;
      }

      // released before the set, which takes the write lock itself
      auto can_change = false;
      {
        const auto lock = JPH::BodyLockRead(physics->GetBodyLockInterface(), body);
        if (!lock.Succeeded()) {
          return OxStatus::NotFound;
        }

        can_change = lock.GetBody().CanBeKinematicOrDynamic();
      }

      if (!can_change) {
        return OxStatus::InvalidArgument;
      }

      bodies.SetMotionType(body, static_cast<JPH::EMotionType>(static_cast<u32>(value)), JPH::EActivation::Activate);
      return OxStatus::Ok;
    }
    case OxBodyProperty::ObjectLayer:
      if (!is_index || value >= static_cast<f32>(PhysicsLayers::NUM_LAYERS)) {
        return OxStatus::InvalidArgument;
      }

      bodies.SetObjectLayer(body, static_cast<JPH::ObjectLayer>(value));
      return OxStatus::Ok;
    case OxBodyProperty::Friction:
    case OxBodyProperty::Restitution:
      if (!std::isfinite(value) || value < 0.0f) {
        return OxStatus::InvalidArgument;
      }

      if (property == OxBodyProperty::Friction) {
        bodies.SetFriction(body, value);
      } else {
        bodies.SetRestitution(body, value);
      }

      return OxStatus::Ok;
    case OxBodyProperty::IsSensor            : bodies.SetIsSensor(body, is_set); return OxStatus::Ok;
    case OxBodyProperty::UseManifoldReduction: bodies.SetUseManifoldReduction(body, is_set); return OxStatus::Ok;
    case OxBodyProperty::CollideKinematicVsNonDynamic:
    case OxBodyProperty::ApplyGyroscopicForce        :
    case OxBodyProperty::EnhancedInternalEdgeRemoval : {
      const auto lock = JPH::BodyLockWrite(physics->GetBodyLockInterface(), body);
      if (!lock.Succeeded()) {
        return OxStatus::NotFound;
      }

      auto& target = lock.GetBody();
      if (property == OxBodyProperty::CollideKinematicVsNonDynamic) {
        target.SetCollideKinematicVsNonDynamic(is_set);
      } else if (property == OxBodyProperty::ApplyGyroscopicForce) {
        target.SetApplyGyroscopicForce(is_set);
      } else {
        target.SetEnhancedInternalEdgeRemoval(is_set);
      }

      return OxStatus::Ok;
    }
  }

  return OxStatus::InvalidArgument;
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

static auto physics_vehicle_state(ecs_world_t* world, u64 entity, OxVehicleState* state) -> OxStatus {
  *state = {};
  auto* constraint = physics_of(world) ? vehicle_constraint_of(world, entity) : nullptr;
  if (!constraint) {
    return OxStatus::NotFound;
  }

  const auto* controller = static_cast<JPH::WheeledVehicleController*>(constraint->GetController());
  const auto* chassis = constraint->GetVehicleBody();
  *state = OxVehicleState{
    .engine_rpm = controller->GetEngine().GetCurrentRPM(),
    .engine_angular_velocity = controller->GetEngine().GetAngularVelocity(),
    .gear = controller->GetTransmission().GetCurrentGear(),
    .forward_speed = (chassis->GetRotation() * constraint->GetLocalForward()).Dot(chassis->GetLinearVelocity()),
  };
  return OxStatus::Ok;
}

static auto physics_wheel_state(ecs_world_t* world, u64 entity, OxWheelState* state) -> OxStatus {
  *state = {};
  if (!physics_of(world) || entity == 0 || !ecs_is_alive(world, entity)) {
    return OxStatus::NotFound;
  }

  const auto wheel_entity = flecs::entity(world, entity);
  const auto* component = wheel_entity.try_get<VehicleWheelComponent>();
  const auto parent = wheel_entity.parent();
  auto* constraint = component && parent.id() != 0 ? vehicle_constraint_of(world, parent.id()) : nullptr;
  if (!constraint || component->runtime_wheel_index >= constraint->GetWheels().size()) {
    return OxStatus::NotFound;
  }

  const auto* wheel = static_cast<const JPH::WheelWV*>(constraint->GetWheel(component->runtime_wheel_index));
  if (wheel->HasContact()) {
    *state = OxWheelState{
      .is_contacting = 1,
      .longitudinal_slip = wheel->mLongitudinalSlip,
      .lateral_slip = wheel->mLateralSlip,
    };
  }

  return OxStatus::Ok;
}

static auto physics_screen_ray(
  ecs_world_t* world, u64 entity, const OxVec2* screen_position, const OxVec2* screen_size, OxRay* ray
) -> OxStatus {
  *ray = {};
  if (entity == 0 || !ecs_is_alive(world, entity)) {
    return OxStatus::NotFound;
  }

  const auto* camera = flecs::entity(world, entity).try_get<CameraComponent>();
  if (!camera) {
    return OxStatus::NotFound;
  }

  const auto screen_ray = Camera::get_screen_ray(
    *camera,
    glm::vec2(screen_position->x, screen_position->y),
    glm::vec2(screen_size->x, screen_size->y)
  );
  const auto& origin = screen_ray.get_origin();
  const auto& direction = screen_ray.get_direction();
  if (is_finite(origin) && is_finite(direction)) {
    ray->origin = OxVec3{.x = origin.x, .y = origin.y, .z = origin.z};
    ray->direction = OxVec3{.x = direction.x, .y = direction.y, .z = direction.z};
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

auto scene_of(ecs_world_t* world) -> Scene* {
  const auto it = scenes.find(world);
  return it == scenes.end() ? nullptr : it->second;
}

auto fill_physics_api(OxNativeApi& api) -> void {
  api.physics_raycast = physics_raycast;
  api.physics_body_state = physics_body_state;
  api.physics_body_command = physics_body_command;
  api.physics_body_set = physics_body_set;
  api.physics_character_state = physics_character_state;
  api.physics_character_command = physics_character_command;
  api.physics_vehicle_state = physics_vehicle_state;
  api.physics_wheel_state = physics_wheel_state;
  api.physics_screen_ray = physics_screen_ray;
}
} // namespace ox
