#include "Scene/Components.hpp"

#include "Core/App.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
CoreComponentsModule::CoreComponentsModule(flecs::world& world) {
  ZoneScoped;

  world
    .component<glm::vec2>("glm::vec2") //
    .member<f32>("x")
    .member<f32>("y");

  world
    .component<glm::ivec2>("glm::ivec2") //
    .member<i32>("x")
    .member<i32>("y");

  world
    .component<glm::vec3>("glm::vec3") //
    .member<f32>("x")
    .member<f32>("y")
    .member<f32>("z");

  world
    .component<glm::vec4>("glm::vec4") //
    .member<f32>("x")
    .member<f32>("y")
    .member<f32>("z")
    .member<f32>("w");

  world
    .component<glm::mat3>("glm::mat3") //
    .member<glm::vec3>("col0")
    .member<glm::vec3>("col1")
    .member<glm::vec3>("col2");

  world
    .component<glm::mat4>("glm::mat4") //
    .member<glm::vec4>("col0")
    .member<glm::vec4>("col1")
    .member<glm::vec4>("col2")
    .member<glm::vec4>("col3");

  world
    .component<glm::quat>("glm::quat") //
    .member<f32>("x")
    .member<f32>("y")
    .member<f32>("z")
    .member<f32>("w");

  world.component<std::string>("std::string")
    .opaque(flecs::String)
    .serialize([](const flecs::serializer* s, const std::string* data) {
      const char* str = data->c_str();
      return s->value(flecs::String, &str);
    })
    .assign_string([](std::string* data, const char* value) { *data = value; });

  world.component<UUID>("ox::UUID")
    .opaque(flecs::String)
    .serialize([](const flecs::serializer* s, const UUID* data) {
      auto str = data->str();
      auto* cstr = str.c_str();
      return s->value(flecs::String, &cstr);
    })
    .assign_string([](UUID* data, const char* value) { *data = UUID::from_string(std::string_view(value)).value(); });

  world.component<TransformComponent>("TransformComponent")
    .member<glm::vec3>("position")
    .member<glm::vec3>("rotation")
    .member<glm::vec3>("scale")
    .add<Networked>();

  // Layer
  world
    .component<LayerComponent>("LayerComponent") //
    .member<u16>("layer");

  // Rendering Components
  world.component<MeshComponent>("MeshComponent")
    .member<u32>("mesh_index")
    .member<bool>("cast_shadows")
    .member<UUID>("mesh_uuid");

  world.component<SpriteComponent>("SpriteComponent")
    .member<u32>("layer")
    .member<bool>("sort_y")
    .member<bool>("flip_x")
    .member<UUID>("material");

  world.component<SpriteAnimationComponent>("SpriteAnimationComponent")
    .member<u32>("num_frames")
    .member<bool>("loop")
    .member<bool>("inverted")
    .member<u32>("fps")
    .member<u32>("columns")
    .member<glm::vec2>("frame_size");

  world.component<CameraComponent>("CameraComponent")
    .member<u32>("projection")
    .member<f32>("fov")
    .member<f32>("aspect")
    .member<f32>("far_clip")
    .member<f32>("near_clip")
    .member<f32>("tilt")
    .member<f32>("zoom");

  world.component<ParticleSystemComponent>("ParticleSystemComponent")
    .member<UUID>("material")
    .member<f32>("duration")
    .member<bool>("looping")
    .member<f32>("start_delay")
    .member<f32>("start_lifetime")
    .member<glm::vec3>("start_velocity")
    .member<glm::vec4>("start_color")
    .member<glm::vec4>("start_size")
    .member<glm::vec4>("start_rotation")
    .member<f32>("gravity_modifier")
    .member<f32>("simulation_speed")
    .member<bool>("play_on_awake")
    .member<u32>("max_particles")
    .member<u32>("rate_over_time")
    .member<u32>("rate_over_distance")
    .member<u32>("burst_count")
    .member<glm::vec3>("position_start")
    .member<glm::vec3>("position_end")
    .member<bool>("velocity_over_lifetime_enabled")
    .member<glm::vec3>("velocity_over_lifetime_start")
    .member<glm::vec3>("velocity_over_lifetime_end")
    .member<bool>("force_over_lifetime_enabled")
    .member<glm::vec3>("force_over_lifetime_start")
    .member<glm::vec3>("force_over_lifetime_end")
    .member<bool>("color_over_lifetime_enabled")
    .member<glm::vec4>("color_over_lifetime_start")
    .member<glm::vec4>("color_over_lifetime_end")
    .member<bool>("color_by_speed_enabled")
    .member<glm::vec4>("color_by_speed_start")
    .member<glm::vec4>("color_by_speed_end")
    .member<f32>("color_by_speed_min_speed")
    .member<f32>("color_by_speed_max_speed")
    .member<bool>("size_over_lifetime_enabled")
    .member<glm::vec3>("size_over_lifetime_start")
    .member<glm::vec3>("size_over_lifetime_end")
    .member<bool>("size_by_speed_enabled")
    .member<glm::vec3>("size_by_speed_start")
    .member<glm::vec3>("size_by_speed_end")
    .member<f32>("size_by_speed_min_speed")
    .member<f32>("size_by_speed_max_speed")
    .member<bool>("rotation_over_lifetime_enabled")
    .member<glm::vec3>("rotation_over_lifetime_start")
    .member<glm::vec3>("rotation_over_lifetime_end")
    .member<bool>("rotation_by_speed_enabled")
    .member<glm::vec3>("rotation_by_speed_start")
    .member<glm::vec3>("rotation_by_speed_end")
    .member<f32>("rotation_by_speed_min_speed")
    .member<f32>("rotation_by_speed_max_speed");

  world.component<ParticleComponent>("ParticleComponent")
    .member<glm::vec4>("color") //
    .member<f32>("life_remaining");

  world.component<LightComponent>("LightComponent")
    .member<u32>("type")
    .member<glm::vec3>("color")
    .member<f32>("intensity")
    .member<f32>("radius")
    .member<f32>("outer_cone_angle")
    .member<f32>("inner_cone_angle")
    .member<bool>("cast_shadows")
    .member<u32>("shadow_map_res")
    .member<u32>("cascade_count")
    .member<f32>("first_cascade_far_bound")
    .member<f32>("maximum_shadow_distance")
    .member<f32>("minimum_shadow_distance")
    .member<f32>("cascade_overlap_propotion")
    .member<f32>("depth_bias")
    .member<f32>("normal_bias");

  world.component<AtmosphereComponent>("AtmosphereComponent")
    .member<glm::vec3>("rayleigh_scattering")
    .member<f32>("rayleigh_density")
    .member<glm::vec3>("mie_scattering")
    .member<f32>("mie_density")
    .member<f32>("mie_extinction")
    .member<f32>("mie_asymmetry")
    .member<glm::vec3>("ozone_absorption")
    .member<f32>("ozone_height")
    .member<f32>("ozone_thickness")
    .member<f32>("aerial_perspective_start_km")
    .member<f32>("aerial_perspective_exposure");

  world.component<AutoExposureComponent>("AutoExposureComponent")
    .member<f32>("min_exposure")
    .member<f32>("max_exposure")
    .member<f32>("adaptation_speed")
    .member<f32>("ev100_bias");

  world
    .component<VignetteComponent>("VignetteComponent") //
    .member<f32>("amount");

  world
    .component<ChromaticAberrationComponent>("ChromaticAberrationComponent") //
    .member<f32>("amount");

  world.component<FilmGrainComponent>("FilmGrainComponent")
    .member<f32>("amount") //
    .member<f32>("scale");

  // Physics Components
  world.component<RigidBodyComponent>("RigidBodyComponent")
    .member<u32>("allowed_dofs")
    .member<u32>("type")
    .member<f32>("mass")
    .member<f32>("linear_drag")
    .member<f32>("angular_drag")
    .member<f32>("gravity_factor")
    .member<f32>("friction")
    .member<f32>("restitution")
    .member<bool>("allow_sleep")
    .member<bool>("awake")
    .member<bool>("continuous")
    .member<bool>("interpolation")
    .member<bool>("is_sensor");

  world.component<BoxColliderComponent>("BoxColliderComponent")
    .member<glm::vec3>("size")
    .member<glm::vec3>("offset")
    .member<f32>("density")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<SphereColliderComponent>("SphereColliderComponent")
    .member<f32>("radius")
    .member<glm::vec3>("offset")
    .member<f32>("density")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<CapsuleColliderComponent>("CapsuleColliderComponent")
    .member<f32>("height")
    .member<f32>("radius")
    .member<glm::vec3>("offset")
    .member<f32>("density")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<TaperedCapsuleColliderComponent>("TaperedCapsuleColliderComponent")
    .member<f32>("height")
    .member<f32>("top_radius")
    .member<f32>("bottom_radius")
    .member<glm::vec3>("offset")
    .member<f32>("density")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<CylinderColliderComponent>("CylinderColliderComponent")
    .member<f32>("height")
    .member<f32>("radius")
    .member<glm::vec3>("offset")
    .member<f32>("density")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<MeshColliderComponent>("MeshColliderComponent")
    .member<glm::vec3>("offset")
    .member<f32>("friction")
    .member<f32>("restitution");

  world.component<CharacterControllerComponent>("CharacterControllerComponent")
    .member<f32>("character_height_standing")
    .member<f32>("character_radius_standing")
    .member<f32>("character_height_crouching")
    .member<f32>("character_radius_crouching")
    .member<bool>("interpolation")
    .member<bool>("control_movement_during_jump")
    .member<f32>("jump_force")
    .member<bool>("auto_bunny_hop")
    .member<f32>("air_control")
    .member<f32>("max_ground_speed")
    .member<f32>("ground_acceleration")
    .member<f32>("ground_deceleration")
    .member<f32>("max_air_speed")
    .member<f32>("air_acceleration")
    .member<f32>("air_deceleration")
    .member<f32>("max_strafe_speed")
    .member<f32>("strafe_acceleration")
    .member<f32>("strafe_deceleration")
    .member<f32>("friction")
    .member<f32>("gravity")
    .member<f32>("collision_tolerance");

  // Audio Components
  world.component<AudioSourceComponent>("AudioSourceComponent")
    .member<UUID>("audio_source")
    .member<u32>("attenuation_model")
    .member<f32>("volume")
    .member<f32>("pitch")
    .member<bool>("play_on_awake")
    .member<bool>("looping")
    .member<bool>("spatialization")
    .member<f32>("roll_off")
    .member<f32>("min_gain")
    .member<f32>("max_gain")
    .member<f32>("min_distance")
    .member<f32>("max_distance")
    .member<f32>("cone_inner_angle")
    .member<f32>("cone_outer_angle")
    .member<f32>("cone_outer_gain")
    .member<f32>("doppler_factor");

  world.component<AudioListenerComponent>("AudioListenerComponent")
    .member<bool>("active")
    .member<u32>("listener_index")
    .member<f32>("cone_inner_angle")
    .member<f32>("cone_outer_angle")
    .member<f32>("cone_outer_gain");

#ifdef OX_LUA_BINDINGS
  auto& lua_manager = App::mod<LuaManager>();
  const auto state = lua_manager.get_state();

  auto core_table = state->create_named_table("Core");
#endif
}
} // namespace ox
