#include "Scripting/LuaPhysicsBindings.hpp"

#include <sol/state.hpp>

#include "Core/App.hpp"
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Physics/Physics.hpp"
#include "Physics/RayCast.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scripting/LuaHelpers.hpp"

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

  // -- Components ---
  const std::initializer_list<std::pair<sol::string_view, RigidbodyComponent::BodyType>> rigidbody_body_type = {
      ENUM_FIELD(RigidbodyComponent::BodyType, Static),
      ENUM_FIELD(RigidbodyComponent::BodyType, Kinematic),
      ENUM_FIELD(RigidbodyComponent::BodyType, Dynamic),
  };
  state->new_enum<RigidbodyComponent::BodyType, true>("RigidbodyBodyType", rigidbody_body_type);

#if 0
  #define RB RigidbodyComponent
  REGISTER_COMPONENT(state, RB, FIELD(RB, type), FIELD(RB, mass), FIELD(RB, linear_drag), FIELD(RB, angular_drag), FIELD(RB, gravity_scale),
                     FIELD(RB, allow_sleep), FIELD(RB, awake), FIELD(RB, continuous), FIELD(RB, interpolation), FIELD(RB, is_sensor));

  #define BCC BoxColliderComponent
  REGISTER_COMPONENT(state, BCC, FIELD(BCC, size), FIELD(BCC, offset), FIELD(BCC, density), FIELD(BCC, friction), FIELD(BCC, restitution));

  #define SCC SphereColliderComponent
  REGISTER_COMPONENT(state, SCC, FIELD(SCC, radius), FIELD(SCC, offset), FIELD(SCC, density), FIELD(SCC, friction), FIELD(SCC, restitution));

  #define CCC CapsuleColliderComponent
  REGISTER_COMPONENT(state, CCC, FIELD(CCC, height), FIELD(CCC, radius), FIELD(CCC, offset), FIELD(CCC, density), FIELD(CCC, friction),
                     FIELD(CCC, restitution));

  #define TCCC TaperedCapsuleColliderComponent
  REGISTER_COMPONENT(state, TCCC, FIELD(TCCC, height), FIELD(TCCC, top_radius), FIELD(TCCC, bottom_radius), FIELD(TCCC, density), FIELD(TCCC, friction),
                     FIELD(TCCC, restitution));

  #define CYCC CylinderColliderComponent
  REGISTER_COMPONENT(state, CYCC, FIELD(CYCC, height), FIELD(CYCC, radius), FIELD(CYCC, offset), FIELD(CYCC, density), FIELD(CYCC, friction),
                     FIELD(CYCC, restitution));

  #define MCC MeshColliderComponent
  REGISTER_COMPONENT(state, MCC, FIELD(MCC, offset), FIELD(MCC, friction), FIELD(MCC, restitution));
#endif
}
} // namespace ox
