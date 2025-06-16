#include "LuaComponentBindings.hpp"

#include <sol/state.hpp>

#include "LuaHelpers.hpp"
#include "Scene/ECSModule/Core.hpp"

namespace ox {
void LuaBindings::bind_components(const std::shared_ptr<sol::state>& state) {
#define TC TransformComponent
  // REGISTER_COMPONENT(state, TC, FIELD(TC, position), FIELD(TC, rotation), FIELD(TC, scale));
  bind_mesh_component(state);
  bind_camera_component(state);
  bind_light_component(state);
}

void LuaBindings::bind_light_component(const std::shared_ptr<sol::state>& state) {
  // REGISTER_COMPONENT(state, LightComponent, FIELD(LightComponent, color), FIELD(LightComponent, intensity)); // TODO:
  // Rest
}

void LuaBindings::bind_mesh_component(const std::shared_ptr<sol::state>& state) {

  // #define MC MeshComponent
  // REGISTER_COMPONENT(state, MC, FIELD(MC, mesh_base), FIELD(MC, stationary), FIELD(MC, cast_shadows), FIELD(MC,
  // materials), FIELD(MC, aabb));
}

void LuaBindings::bind_camera_component(const std::shared_ptr<sol::state>& state) {
  auto camera_type = state->new_usertype<CameraComponent>("Camera");

  // REGISTER_COMPONENT(state,
  //                    CameraComponent,
  //                    FIELD(CameraComponent, yaw),
  //                    FIELD(CameraComponent, pitch),
  //                    FIELD(CameraComponent, near_clip),
  //                    FIELD(CameraComponent, far_clip),
  //                    FIELD(CameraComponent, fov),
  //                    FIELD(CameraComponent, aspect),
  //                    FIELD(CameraComponent, forward),
  //                    FIELD(CameraComponent, right));
}
} // namespace ox
