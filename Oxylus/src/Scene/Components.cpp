#include "Scene/Components.hpp"

#include "Scene/ComponentRegistry.hpp"

#ifdef OX_LUA_BINDINGS
  #include "Core/App.hpp"
  #include "Scripting/LuaManager.hpp"
#endif

namespace ox {
CoreComponentsModule::CoreComponentsModule(flecs::world& world) {
  ZoneScoped;

  world.module<CoreComponentsModule>("Core");

#ifdef OX_LUA_BINDINGS
  auto* state = App::mod<LuaManager>().get_state();
  auto registry = ComponentRegistry{world, state, state->create_named_table("Core")};
#else
  auto registry = ComponentRegistry{world};
#endif

  // Math types
  registry.bind_value<&glm::vec2::x, &glm::vec2::y>("glm::vec2");
  registry.bind_value<&glm::ivec2::x, &glm::ivec2::y>("glm::ivec2");
  registry.bind_value<&glm::vec3::x, &glm::vec3::y, &glm::vec3::z>("glm::vec3");
  registry.bind_value<&glm::vec4::x, &glm::vec4::y, &glm::vec4::z, &glm::vec4::w>("glm::vec4");
  registry.bind_matrix<glm::mat3, glm::vec3, 3>("glm::mat3");
  registry.bind_matrix<glm::mat4, glm::vec4, 4>("glm::mat4");
  registry.bind_value<&glm::quat::x, &glm::quat::y, &glm::quat::z, &glm::quat::w>("glm::quat");

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

  // Enums, bound before the components that use them so flecs registers their constants under these names
  // instead of implicitly deriving a name from the C++ path at first use.
  registry.bind_enum<CameraComponent::Projection>("CameraProjection");
  registry.bind_enum<LightComponent::LightType>("LightType");
  registry.bind_enum<RigidBodyComponent::BodyType>("RigidBodyType");
  registry.bind_enum<GPU::TonemapType>("TonemapType");

  {
    using C = TransformComponent;
    registry.bind<&C::position, &C::rotation, &C::scale>().tags<Networked>();
  }

  // Layer
  {
    using C = LayerComponent;
    registry.bind<&C::layer>();
  }

  // Rendering Components
  {
    using C = MeshComponent;
    registry.bind<&C::model_uuid, &C::mesh_index, &C::material_uuid, &C::cast_shadows>();
  }

  {
    using C = SpriteComponent;
    registry.bind<&C::layer, &C::sort_y, &C::flip_x, &C::material>().tags<Networked>();
  }

  {
    using C = SpriteAnimationComponent;
    registry.bind<&C::num_frames, &C::loop, &C::inverted, &C::fps, &C::columns, &C::frame_size>();
  }

  {
    using C = CameraComponent;
    registry.bind<&C::projection, &C::fov, &C::aspect, &C::far_clip, &C::near_clip, &C::tilt, &C::zoom>();
  }

  {
    using C = ParticleSystemComponent;
    registry.bind<
      &C::material,
      &C::duration,
      &C::looping,
      &C::start_delay,
      &C::start_lifetime,
      &C::start_velocity,
      &C::start_color,
      &C::start_size,
      &C::start_rotation,
      &C::gravity_modifier,
      &C::simulation_speed,
      &C::play_on_awake,
      &C::max_particles,
      &C::rate_over_time,
      &C::rate_over_distance,
      &C::burst_count,
      &C::position_start,
      &C::position_end,
      &C::velocity_over_lifetime_enabled,
      &C::velocity_over_lifetime_start,
      &C::velocity_over_lifetime_end,
      &C::force_over_lifetime_enabled,
      &C::force_over_lifetime_start,
      &C::force_over_lifetime_end,
      &C::color_over_lifetime_enabled,
      &C::color_over_lifetime_start,
      &C::color_over_lifetime_end,
      &C::color_by_speed_enabled,
      &C::color_by_speed_start,
      &C::color_by_speed_end,
      &C::color_by_speed_min_speed,
      &C::color_by_speed_max_speed,
      &C::size_over_lifetime_enabled,
      &C::size_over_lifetime_start,
      &C::size_over_lifetime_end,
      &C::size_by_speed_enabled,
      &C::size_by_speed_start,
      &C::size_by_speed_end,
      &C::size_by_speed_min_speed,
      &C::size_by_speed_max_speed,
      &C::rotation_over_lifetime_enabled,
      &C::rotation_over_lifetime_start,
      &C::rotation_over_lifetime_end,
      &C::rotation_by_speed_enabled,
      &C::rotation_by_speed_start,
      &C::rotation_by_speed_end,
      &C::rotation_by_speed_min_speed,
      &C::rotation_by_speed_max_speed>();
  }

  {
    using C = ParticleComponent;
    registry.bind<&C::color, &C::life_remaining>();
  }

  {
    using C = LightComponent;
    registry.bind<
      &C::type,
      &C::color,
      &C::intensity,
      &C::radius,
      &C::outer_cone_angle,
      &C::inner_cone_angle,
      &C::cast_shadows,
      &C::first_cascade_far_bound,
      &C::maximum_shadow_distance,
      &C::minimum_shadow_distance,
      &C::first_clipmap_width,
      &C::clipmap_selection_bias>();
  }

  {
    using C = SkyComponent;
    registry.bind<&C::solid_color, &C::texture>();
  }

  {
    using C = AtmosphereComponent;
    registry.bind<
      &C::rayleigh_scattering,
      &C::rayleigh_density,
      &C::mie_scattering,
      &C::mie_density,
      &C::mie_extinction,
      &C::mie_asymmetry,
      &C::ozone_absorption,
      &C::ozone_height,
      &C::ozone_thickness,
      &C::aerial_perspective_start_km,
      &C::aerial_perspective_exposure>();
  }

  {
    using C = AutoExposureComponent;
    registry.bind<&C::min_exposure, &C::max_exposure, &C::adaptation_speed, &C::ev100_bias>();
  }

  {
    using C = VignetteComponent;
    registry.bind<&C::amount>();
  }

  {
    using C = ChromaticAberrationComponent;
    registry.bind<&C::amount>();
  }

  {
    using C = FilmGrainComponent;
    registry.bind<&C::amount, &C::scale>();
  }

  // Physics Components
  {
    using C = RigidBodyComponent;
    registry.bind<
      &C::allowed_dofs,
      &C::type,
      &C::mass,
      &C::linear_drag,
      &C::angular_drag,
      &C::gravity_factor,
      &C::friction,
      &C::restitution,
      &C::allow_sleep,
      &C::awake,
      &C::continuous,
      &C::interpolation,
      &C::is_sensor>();
  }

  {
    using C = BoxColliderComponent;
    registry.bind<&C::size, &C::offset, &C::density, &C::friction, &C::restitution>();
  }

  {
    using C = SphereColliderComponent;
    registry.bind<&C::radius, &C::offset, &C::density, &C::friction, &C::restitution>();
  }

  {
    using C = CapsuleColliderComponent;
    registry.bind<&C::height, &C::radius, &C::offset, &C::density, &C::friction, &C::restitution>();
  }

  {
    using C = TaperedCapsuleColliderComponent;
    registry
      .bind<&C::height, &C::top_radius, &C::bottom_radius, &C::offset, &C::density, &C::friction, &C::restitution>();
  }

  {
    using C = CylinderColliderComponent;
    registry.bind<&C::height, &C::radius, &C::offset, &C::density, &C::friction, &C::restitution>();
  }

  {
    using C = MeshColliderComponent;
    registry.bind<&C::offset, &C::friction, &C::restitution>();
  }

  {
    using C = CharacterControllerComponent;
    registry.bind<
      &C::character_height_standing,
      &C::character_radius_standing,
      &C::character_height_crouching,
      &C::character_radius_crouching,
      &C::interpolation,
      &C::control_movement_during_jump,
      &C::jump_force,
      &C::auto_bunny_hop,
      &C::air_control,
      &C::max_ground_speed,
      &C::ground_acceleration,
      &C::ground_deceleration,
      &C::max_air_speed,
      &C::air_acceleration,
      &C::air_deceleration,
      &C::max_strafe_speed,
      &C::strafe_acceleration,
      &C::strafe_deceleration,
      &C::friction,
      &C::gravity,
      &C::collision_tolerance>();
  }

  // Audio Components
  {
    using C = AudioSourceComponent;
    registry.bind<
      &C::audio_source,
      &C::attenuation_model,
      &C::volume,
      &C::pitch,
      &C::play_on_awake,
      &C::looping,
      &C::spatialization,
      &C::roll_off,
      &C::min_gain,
      &C::max_gain,
      &C::min_distance,
      &C::max_distance,
      &C::cone_inner_angle,
      &C::cone_outer_angle,
      &C::cone_outer_gain,
      &C::doppler_factor>();
  }

  {
    using C = AudioListenerComponent;
    registry.bind<&C::active, &C::listener_index, &C::cone_inner_angle, &C::cone_outer_angle, &C::cone_outer_gain>();
  }

  {
    using C = TonemappingComponent;
    registry.bind<&C::tonemap_type>();
  }
}
} // namespace ox
