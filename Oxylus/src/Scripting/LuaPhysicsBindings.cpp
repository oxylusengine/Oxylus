#include "Scripting/LuaPhysicsBindings.hpp"

// clang-format off
#include "Physics/Physics.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Character/Character.h>
#include <sol/state.hpp>

#include "Core/App.hpp"
#include "Physics/RayCast.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scripting/LuaHelpers.hpp"
#include "Utils/OxMath.hpp"
#include "Render/Camera.hpp"
// clang-format on

namespace ox {
auto PhysicsBinding::bind(sol::state* state) -> void {
  auto raycast_type = state->new_usertype<RayCast>("RayCast", sol::constructors<RayCast(glm::vec3, glm::vec3)>());
  SET_TYPE_FUNCTION(raycast_type, RayCast, get_point_on_ray);
  SET_TYPE_FUNCTION(raycast_type, RayCast, get_direction);
  SET_TYPE_FUNCTION(raycast_type, RayCast, get_origin);

  auto collector_type = state->new_usertype<JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector>>(
      "RayCastCollector");
  collector_type.set_function("had_hit", &JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector>::HadHit);
  collector_type.set_function("sort", &JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector>::Sort);

  auto result_type = state->new_usertype<JPH::BroadPhaseCastResult>("RayCastResult");
  result_type["fraction"] = &JPH::BroadPhaseCastResult::mFraction;

  auto physics_table = state->create_table("Physics");
  physics_table.set_function("cast_ray",
                             [](const RayCast& ray) -> JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector> {
                               return App::get_system<Physics>(EngineSystems::Physics)->cast_ray(ray);
                             });
  physics_table.set_function(
      "get_hits",
      [](const JPH::AllHitCollisionCollector<JPH::RayCastBodyCollector>& collector)
          -> std::vector<JPH::BroadPhaseCastResult> { return {collector.mHits.begin(), collector.mHits.end()}; });

  physics_table.set_function("get_body", [](flecs::entity* e) -> JPH::Body* {
    auto* rb = e->try_get<RigidBodyComponent>();
    OX_CHECK_NULL(rb);
    auto* body = static_cast<JPH::Body*>(rb->runtime_body);
    OX_CHECK_NULL(body);
    return body;
  });

  physics_table.set_function("get_character", [](flecs::entity* e) -> JPH::Character* {
    auto* cc = e->try_get<CharacterControllerComponent>();
    OX_CHECK_NULL(cc);
    auto* character = reinterpret_cast<JPH::Character*>(cc->character);
    OX_CHECK_NULL(character);
    return character;
  });

  physics_table.set_function("get_screen_ray_from_camera",
                             [](flecs::entity* e, glm::vec2 screen_pos, glm::vec2 screen_size) -> RayCast {
                               auto* c = e->try_get<CameraComponent>();
                               OX_CHECK_NULL(c);
                               return Camera::get_screen_ray(*c, screen_pos, screen_size);
                             });

  state->new_usertype<JPH::BodyID>(
      "BodyID",

      "get_index",
      [](JPH::BodyID& body_id) { return body_id.GetIndex(); },

      "get_sequence_number",
      [](JPH::BodyID& body_id) { return body_id.GetSequenceNumber(); },

      "get_index_and_sequence_number",
      [](JPH::BodyID& body_id) { return body_id.GetIndexAndSequenceNumber(); },

      "is_invalid",
      [](JPH::BodyID& body_id) { return body_id.IsInvalid(); },

      sol::meta_function::equal_to,
      [](JPH::BodyID& body_id1, JPH::BodyID& body_id2) { return body_id1 == body_id2; });

  state->new_usertype<JPH::Body>(
      "Body",
      sol::no_constructor,

      "get_id",
      &JPH::Body::GetID,

      "get_body_type",
      &JPH::Body::GetBodyType,

      "is_rigid_body",
      &JPH::Body::IsRigidBody,

      "is_soft_body",
      &JPH::Body::IsSoftBody,

      "is_active",
      &JPH::Body::IsActive,

      "is_static",
      &JPH::Body::IsStatic,

      "is_kinematic",
      &JPH::Body::IsKinematic,

      "is_dynamic",
      &JPH::Body::IsDynamic,

      "can_be_kinematic_or_dynamic",
      &JPH::Body::CanBeKinematicOrDynamic,

      "is_sensor",
      &JPH::Body::IsSensor,

      "set_collide_kinematic_vs_non_dynamic",
      &JPH::Body::SetCollideKinematicVsNonDynamic,

      "get_collide_kinematic_vs_non_dynamic",
      &JPH::Body::GetCollideKinematicVsNonDynamic,

      "set_use_manifold_reduction",
      &JPH::Body::SetUseManifoldReduction,

      "get_use_manifold_reduction",
      &JPH::Body::GetUseManifoldReduction,

      "set_apply_gyroscopic_force",
      &JPH::Body::SetApplyGyroscopicForce,

      "get_apply_gyroscopic_force",
      &JPH::Body::GetApplyGyroscopicForce,

      "set_enhanced_internal_edge_removal",
      &JPH::Body::SetEnhancedInternalEdgeRemoval,

      "get_enhanced_internal_edge_removal",
      &JPH::Body::GetEnhancedInternalEdgeRemoval,

      "get_enhanced_internal_edge_removal_with_body",
      &JPH::Body::GetEnhancedInternalEdgeRemovalWithBody,

      "get_motion_type",
      &JPH::Body::GetMotionType,

      "set_motion_type",
      &JPH::Body::SetMotionType,

      "get_broad_phase_layer",
      &JPH::Body::GetBroadPhaseLayer,

      "get_object_layer",
      &JPH::Body::GetObjectLayer,

      "get_friction",
      &JPH::Body::GetFriction,

      "set_friction",
      &JPH::Body::SetFriction,

      "get_restitution",
      &JPH::Body::GetRestitution,

      "set_restitution",
      &JPH::Body::SetRestitution,

      "get_linear_velocity",
      [](JPH::Body& body) -> glm::vec3 { return math::from_jolt(body.GetLinearVelocity()); },

      "set_linear_velocity",
      [](JPH::Body& body, const glm::vec3& v) { body.SetLinearVelocity(math::to_jolt(v)); },

      "set_linear_velocity_clamped",
      [](JPH::Body& body, const glm::vec3& v) { body.SetLinearVelocityClamped(math::to_jolt(v)); },

      "get_angular_velocity",
      [](JPH::Body& body) -> glm::vec3 { return math::from_jolt(body.GetAngularVelocity()); },

      "set_angular_velocity",
      [](JPH::Body& body, const glm::vec3& v) { body.SetAngularVelocity(math::to_jolt(v)); },

      "set_angular_velocity_clamped",
      [](JPH::Body& body, const glm::vec3& v) { body.SetAngularVelocityClamped(math::to_jolt(v)); },

      "get_point_velocity_com",
      [](JPH::Body& body, const glm::vec3& v) -> glm::vec3 {
        return math::from_jolt(body.GetPointVelocityCOM(math::to_jolt(v)));
      },

      "get_point_velocity",
      [](JPH::Body& body, const glm::vec3& v) -> glm::vec3 {
        return math::from_jolt(body.GetPointVelocity(math::to_jolt(v)));
      },

      "add_force",
      [](JPH::Body& body, const glm::vec3& v) { body.AddForce(math::to_jolt(v)); },

      "add_force_at_position",
      [](JPH::Body& body, const glm::vec3& v, const glm::vec3& v2) {
        body.AddForce(math::to_jolt(v), math::to_jolt(v2));
      },

      "add_torque",
      &JPH::Body::AddTorque,

      "get_accumulated_force",
      &JPH::Body::GetAccumulatedForce,

      "get_accumulated_torque",
      &JPH::Body::GetAccumulatedTorque,

      "add_impulse",
      [](JPH::Body& body, const glm::vec3& v) { body.AddImpulse(math::to_jolt(v)); },

      "add_impulse_at_position",
      [](JPH::Body& body, const glm::vec3& v, const glm::vec3& v2) {
        body.AddImpulse(math::to_jolt(v), math::to_jolt(v2));
      },

      "add_angular_impulse",
      &JPH::Body::AddAngularImpulse,

      "move_kinematic",
      [](JPH::Body& body, glm::vec3 target_position, const glm::quat& target_rotation, f32 delta_time) {
        const auto physics = App::get_system<Physics>(EngineSystems::Physics);
        JPH::BodyInterface& body_interface = physics->get_physics_system()->GetBodyInterface();
        body_interface.MoveKinematic(
            body.GetID(), math::to_jolt(target_position), math::to_jolt(target_rotation), delta_time);
      },

      "get_shape",
      &JPH::Body::GetShape,

      "get_position",
      &JPH::Body::GetPosition,

      "get_rotation",
      &JPH::Body::GetRotation,

      "set_rotation",
      [](JPH::Body* body,
         const glm::quat& rotation,
         sol::optional<JPH::EActivation> activation_mode = JPH::EActivation::Activate) {
        const auto physics = App::get_system<Physics>(EngineSystems::Physics);
        JPH::BodyInterface& body_interface = physics->get_physics_system()->GetBodyInterface();
        body_interface.SetRotation(body->GetID(), math::to_jolt(rotation), *activation_mode);
      },

      "get_world_transform",
      &JPH::Body::GetWorldTransform,

      "get_center_of_mass_position",
      &JPH::Body::GetCenterOfMassPosition,

      "get_center_of_mass_transform",
      &JPH::Body::GetCenterOfMassTransform,

      "get_inverse_center_of_mass_transform",
      &JPH::Body::GetInverseCenterOfMassTransform,

      "get_world_space_bounds",
      &JPH::Body::GetWorldSpaceBounds,

      // "get_motion_properties",
      // &JPH::Body::GetMotionProperties,
      // "get_motion_properties_unchecked",
      // &JPH::Body::GetMotionPropertiesUnchecked,

      "get_world_space_surface_normal",
      &JPH::Body::GetWorldSpaceSurfaceNormal,

      "get_transformed_shape",
      &JPH::Body::GetTransformedShape,

      "get_body_creation_settings",
      &JPH::Body::GetBodyCreationSettings,

      "get_soft_body_creation_settings",
      &JPH::Body::GetSoftBodyCreationSettings);

  state->new_usertype<JPH::Character>(
      "Character",
      sol::no_constructor,

      "activate",
      &JPH::Character::Activate,

      "set_linear_and_angular_velocity",
      [](JPH::Character& character,
         const glm::vec3& inLinearVelocity,
         const glm::vec3& inAngularVelocity,
         sol::optional<bool> inLockBodies = true) {
        character.SetLinearAndAngularVelocity(
            math::to_jolt(inLinearVelocity), math::to_jolt(inAngularVelocity), *inLockBodies);
      },

      "get_linear_velocity",
      [](JPH::Character& character, sol::optional<bool> inLockBodies = true) {
        return math::from_jolt(character.GetLinearVelocity(*inLockBodies));
      },

      "set_linear_velocity",
      [](JPH::Character& character, const glm::vec3& inLinearVelocity, sol::optional<bool> inLockBodies = true) {
        character.SetLinearVelocity(math::to_jolt(inLinearVelocity), *inLockBodies);
      },

      "add_linear_velocity",
      [](JPH::Character& character, const glm::vec3& inLinearVelocity, sol::optional<bool> inLockBodies = true) {
        character.AddLinearVelocity(math::to_jolt(inLinearVelocity), *inLockBodies);
      },

      "add_impulse",
      [](JPH::Character& character, const glm::vec3& inImpulse, sol::optional<bool> inLockBodies = true) {
        character.AddImpulse(math::to_jolt(inImpulse), *inLockBodies);
      },

      "get_body_id",
      &JPH::Character::GetBodyID,

      "get_position",
      [](JPH::Character& character, sol::optional<bool> inLockBodies = true) {
        return math::from_jolt(character.GetPosition(*inLockBodies));
      },

      "set_position",
      [](JPH::Character& character,
         const glm::vec3& position,
         sol::optional<JPH::EActivation> activation_mode = JPH::EActivation::Activate,
         sol::optional<bool> lock_bodies = true) { character.SetPosition(math::to_jolt(position)); },

      "get_rotation",
      [](JPH::Character& character, sol::optional<bool> inLockBodies = true) {
        return math::from_jolt(character.GetRotation(*inLockBodies));
      },

      "set_rotation",
      [](JPH::Character& character,
         const glm::quat& rotation,
         sol::optional<JPH::EActivation> activation_mode = JPH::EActivation::Activate,
         sol::optional<bool> inLockBodies = true) {
        return character.SetRotation(math::to_jolt(rotation), *activation_mode, *inLockBodies);
      });
}
} // namespace ox
