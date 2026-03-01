#include "Scene/Components.hpp"

#include "Core/App.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
template <typename T>
struct ComponentBinder {
  ComponentBinder(flecs::world& world_, sol::state* state_, sol::table& module_table_, const char* name)
      : world(world_),
        state(state_),
        module_table(module_table_),
        component(world_.component<T>(name)) {
    if (state) {
      usertype = state->create_named_table(name);
      usertype["component_id"] = static_cast<u64>(component.id());
    }
  }

  auto member(this ComponentBinder&& self, const char* name, auto member_pointer) -> ComponentBinder&& {
    self.component.member(name, member_pointer);
    if (self.state) {
      self.usertype[name] = member_pointer;
    }

    return std::move(self);
  }

  template <typename Tag>
  auto add(this ComponentBinder&& self) -> ComponentBinder&& {
    self.component.add<Tag>();
    return std::move(self);
  }

  auto add(this ComponentBinder&& self, flecs::entity e) -> ComponentBinder&& {
    self.component.add(e);
    return std::move(self);
  }

  auto finalize(this ComponentBinder&& self) -> flecs::entity {
    if (self.state) {
      self.module_table[self.component.name().c_str()] = self.usertype;
    }
    return self.component;
  }

  operator flecs::entity() const { return component; }

private:
  flecs::world& world;
  sol::state* state;
  sol::table& module_table;
  flecs::untyped_component component;
  sol::table usertype;
};

template <typename T>
auto bind_component(flecs::world& world, sol::state* state, sol::table& module_table, const char* name) {
  return ComponentBinder<T>(world, state, module_table, name);
}

CoreComponentsModule::CoreComponentsModule(flecs::world& world) {
  ZoneScoped;

#ifdef OX_LUA_BINDINGS
  auto& lua_manager = App::mod<LuaManager>();
  const auto state = lua_manager.get_state();
  auto core_table = state->create_named_table("Core");
#endif

  auto cur_component = flecs::entity{};

  world.module<CoreComponentsModule>("Core");

  world.component<glm::vec2>("glm::vec2")
    .member("x", &glm::vec2::x) //
    .member("y", &glm::vec2::y);

  world.component<glm::ivec2>("glm::ivec2")
    .member("x", &glm::ivec2::x) //
    .member("y", &glm::ivec2::y);

  world.component<glm::vec3>("glm::vec3")
    .member("x", &glm::vec3::x) //
    .member("y", &glm::vec3::y)
    .member("z", &glm::vec3::z);

  world.component<glm::vec4>("glm::vec4")
    .member("x", &glm::vec4::x) //
    .member("y", &glm::vec4::y)
    .member("z", &glm::vec4::z)
    .member("w", &glm::vec4::w);

  world.component<glm::mat3>("glm::mat3")
    .member<glm::vec3>("col0") //
    .member<glm::vec3>("col1")
    .member<glm::vec3>("col2");

  world.component<glm::mat4>("glm::mat4")
    .member<glm::vec4>("col0")
    .member<glm::vec4>("col1")
    .member<glm::vec4>("col2")
    .member<glm::vec4>("col3");

  world.component<glm::quat>("glm::quat")
    .member("x", &glm::quat::x) //
    .member("y", &glm::quat::y)
    .member("z", &glm::quat::z)
    .member("w", &glm::quat::w);

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

  world.component<GPU::TonemapType>("GPU::TonemapType")
    .opaque(flecs::U32)
    .serialize([](const flecs::serializer* s, const GPU::TonemapType* data) {
      //
      return s->value(flecs::U32, data);
    });

  world.component<LightComponent::LightType>("LightComponent::LightType")
    .opaque(flecs::U32)
    .serialize([](const flecs::serializer* s, const LightComponent::LightType* data) {
      //
      return s->value(flecs::U32, data);
    });

  bind_component<TransformComponent>(world, state, core_table, "TransformComponent")
    .member("position", &TransformComponent::position)
    .member("rotation", &TransformComponent::rotation)
    .member("scale", &TransformComponent::scale)
    .add<Networked>()
    .finalize();

  // Layer
  bind_component<LayerComponent>(world, state, core_table, "LayerComponent")
    .member("layer", &LayerComponent::layer)
    .finalize();

  // Rendering Components
  bind_component<MeshComponent>(world, state, core_table, "MeshComponent")
    .member("model_uuid", &MeshComponent::model_uuid)
    .member("mesh_index", &MeshComponent::mesh_index)
    .member("material_uuid", &MeshComponent::material_uuid)
    .member("cast_shadows", &MeshComponent::cast_shadows)
    .finalize();

  bind_component<SpriteComponent>(world, state, core_table, "SpriteComponent")
    .member("layer", &SpriteComponent::layer)
    .member("sort_y", &SpriteComponent::sort_y)
    .member("flip_x", &SpriteComponent::flip_x)
    .member("material", &SpriteComponent::material)
    .add<Networked>()
    .finalize();

  bind_component<SpriteAnimationComponent>(world, state, core_table, "SpriteAnimationComponent")
    .member("num_frames", &SpriteAnimationComponent::num_frames)
    .member("loop", &SpriteAnimationComponent::loop)
    .member("inverted", &SpriteAnimationComponent::inverted)
    .member("fps", &SpriteAnimationComponent::fps)
    .member("columns", &SpriteAnimationComponent::columns)
    .member("frame_size", &SpriteAnimationComponent::frame_size)
    .finalize();

  bind_component<CameraComponent>(world, state, core_table, "CameraComponent")
    .member("projection", &CameraComponent::projection)
    .member("fov", &CameraComponent::fov)
    .member("aspect", &CameraComponent::aspect)
    .member("far_clip", &CameraComponent::far_clip)
    .member("near_clip", &CameraComponent::near_clip)
    .member("tilt", &CameraComponent::tilt)
    .member("zoom", &CameraComponent::zoom)
    .finalize();

  bind_component<ParticleSystemComponent>(world, state, core_table, "ParticleSystemComponent")
    .member("material", &ParticleSystemComponent::material)
    .member("duration", &ParticleSystemComponent::duration)
    .member("looping", &ParticleSystemComponent::looping)
    .member("start_delay", &ParticleSystemComponent::start_delay)
    .member("start_lifetime", &ParticleSystemComponent::start_lifetime)
    .member("start_velocity", &ParticleSystemComponent::start_velocity)
    .member("start_color", &ParticleSystemComponent::start_color)
    .member("start_size", &ParticleSystemComponent::start_size)
    .member("start_rotation", &ParticleSystemComponent::start_rotation)
    .member("gravity_modifier", &ParticleSystemComponent::gravity_modifier)
    .member("simulation_speed", &ParticleSystemComponent::simulation_speed)
    .member("play_on_awake", &ParticleSystemComponent::play_on_awake)
    .member("max_particles", &ParticleSystemComponent::max_particles)
    .member("rate_over_time", &ParticleSystemComponent::rate_over_time)
    .member("rate_over_distance", &ParticleSystemComponent::rate_over_distance)
    .member("burst_count", &ParticleSystemComponent::burst_count)
    .member("position_start", &ParticleSystemComponent::position_start)
    .member("position_end", &ParticleSystemComponent::position_end)
    .member("velocity_over_lifetime_enabled", &ParticleSystemComponent::velocity_over_lifetime_enabled)
    .member("velocity_over_lifetime_start", &ParticleSystemComponent::velocity_over_lifetime_start)
    .member("velocity_over_lifetime_end", &ParticleSystemComponent::velocity_over_lifetime_end)
    .member("force_over_lifetime_enabled", &ParticleSystemComponent::force_over_lifetime_enabled)
    .member("force_over_lifetime_start", &ParticleSystemComponent::force_over_lifetime_start)
    .member("force_over_lifetime_end", &ParticleSystemComponent::force_over_lifetime_end)
    .member("color_over_lifetime_enabled", &ParticleSystemComponent::color_over_lifetime_enabled)
    .member("color_over_lifetime_start", &ParticleSystemComponent::color_over_lifetime_start)
    .member("color_over_lifetime_end", &ParticleSystemComponent::color_over_lifetime_end)
    .member("color_by_speed_enabled", &ParticleSystemComponent::color_by_speed_enabled)
    .member("color_by_speed_start", &ParticleSystemComponent::color_by_speed_start)
    .member("color_by_speed_end", &ParticleSystemComponent::color_by_speed_end)
    .member("color_by_speed_min_speed", &ParticleSystemComponent::color_by_speed_min_speed)
    .member("color_by_speed_max_speed", &ParticleSystemComponent::color_by_speed_max_speed)
    .member("size_over_lifetime_enabled", &ParticleSystemComponent::size_over_lifetime_enabled)
    .member("size_over_lifetime_start", &ParticleSystemComponent::size_over_lifetime_start)
    .member("size_over_lifetime_end", &ParticleSystemComponent::size_over_lifetime_end)
    .member("size_by_speed_enabled", &ParticleSystemComponent::size_by_speed_enabled)
    .member("size_by_speed_start", &ParticleSystemComponent::size_by_speed_start)
    .member("size_by_speed_end", &ParticleSystemComponent::size_by_speed_end)
    .member("size_by_speed_min_speed", &ParticleSystemComponent::size_by_speed_min_speed)
    .member("size_by_speed_max_speed", &ParticleSystemComponent::size_by_speed_max_speed)
    .member("rotation_over_lifetime_enabled", &ParticleSystemComponent::rotation_over_lifetime_enabled)
    .member("rotation_over_lifetime_start", &ParticleSystemComponent::rotation_over_lifetime_start)
    .member("rotation_over_lifetime_end", &ParticleSystemComponent::rotation_over_lifetime_end)
    .member("rotation_by_speed_enabled", &ParticleSystemComponent::rotation_by_speed_enabled)
    .member("rotation_by_speed_start", &ParticleSystemComponent::rotation_by_speed_start)
    .member("rotation_by_speed_end", &ParticleSystemComponent::rotation_by_speed_end)
    .member("rotation_by_speed_min_speed", &ParticleSystemComponent::rotation_by_speed_min_speed)
    .member("rotation_by_speed_max_speed", &ParticleSystemComponent::rotation_by_speed_max_speed)
    .finalize();

  bind_component<ParticleComponent>(world, state, core_table, "ParticleComponent")
    .member("color", &ParticleComponent::color)
    .member("life_remaining", &ParticleComponent::life_remaining)
    .finalize();

  bind_component<LightComponent>(world, state, core_table, "LightComponent")
    .member("type", &LightComponent::type)
    .member("color", &LightComponent::color)
    .member("intensity", &LightComponent::intensity)
    .member("radius", &LightComponent::radius)
    .member("outer_cone_angle", &LightComponent::outer_cone_angle)
    .member("inner_cone_angle", &LightComponent::inner_cone_angle)
    .member("cast_shadows", &LightComponent::cast_shadows)
    .member("shadow_map_res", &LightComponent::shadow_map_res)
    .member("cascade_count", &LightComponent::cascade_count)
    .member("first_cascade_far_bound", &LightComponent::first_cascade_far_bound)
    .member("maximum_shadow_distance", &LightComponent::maximum_shadow_distance)
    .member("minimum_shadow_distance", &LightComponent::minimum_shadow_distance)
    .member("cascade_overlap_propotion", &LightComponent::cascade_overlap_propotion)
    .member("depth_bias", &LightComponent::depth_bias)
    .member("normal_bias", &LightComponent::normal_bias)
    .finalize();

  bind_component<AtmosphereComponent>(world, state, core_table, "AtmosphereComponent")
    .member("rayleigh_scattering", &AtmosphereComponent::rayleigh_scattering)
    .member("rayleigh_density", &AtmosphereComponent::rayleigh_density)
    .member("mie_scattering", &AtmosphereComponent::mie_scattering)
    .member("mie_density", &AtmosphereComponent::mie_density)
    .member("mie_extinction", &AtmosphereComponent::mie_extinction)
    .member("mie_asymmetry", &AtmosphereComponent::mie_asymmetry)
    .member("ozone_absorption", &AtmosphereComponent::ozone_absorption)
    .member("ozone_height", &AtmosphereComponent::ozone_height)
    .member("ozone_thickness", &AtmosphereComponent::ozone_thickness)
    .member("aerial_perspective_start_km", &AtmosphereComponent::aerial_perspective_start_km)
    .member("aerial_perspective_exposure", &AtmosphereComponent::aerial_perspective_exposure)
    .finalize();

  bind_component<AutoExposureComponent>(world, state, core_table, "AutoExposureComponent")
    .member("min_exposure", &AutoExposureComponent::min_exposure)
    .member("max_exposure", &AutoExposureComponent::max_exposure)
    .member("adaptation_speed", &AutoExposureComponent::adaptation_speed)
    .member("ev100_bias", &AutoExposureComponent::ev100_bias)
    .finalize();

  bind_component<VignetteComponent>(world, state, core_table, "VignetteComponent")
    .member("amount", &VignetteComponent::amount)
    .finalize();

  bind_component<ChromaticAberrationComponent>(world, state, core_table, "ChromaticAberrationComponent")
    .member("amount", &ChromaticAberrationComponent::amount)
    .finalize();

  bind_component<FilmGrainComponent>(world, state, core_table, "FilmGrainComponent")
    .member("amount", &FilmGrainComponent::amount)
    .member("scale", &FilmGrainComponent::scale)
    .finalize();

  // Physics Components
  bind_component<RigidBodyComponent>(world, state, core_table, "RigidBodyComponent")
    .member("allowed_dofs", &RigidBodyComponent::allowed_dofs)
    .member("type", &RigidBodyComponent::type)
    .member("mass", &RigidBodyComponent::mass)
    .member("linear_drag", &RigidBodyComponent::linear_drag)
    .member("angular_drag", &RigidBodyComponent::angular_drag)
    .member("gravity_factor", &RigidBodyComponent::gravity_factor)
    .member("friction", &RigidBodyComponent::friction)
    .member("restitution", &RigidBodyComponent::restitution)
    .member("allow_sleep", &RigidBodyComponent::allow_sleep)
    .member("awake", &RigidBodyComponent::awake)
    .member("continuous", &RigidBodyComponent::continuous)
    .member("interpolation", &RigidBodyComponent::interpolation)
    .member("is_sensor", &RigidBodyComponent::is_sensor)
    .finalize();

  bind_component<BoxColliderComponent>(world, state, core_table, "BoxColliderComponent")
    .member("size", &BoxColliderComponent::size)
    .member("offset", &BoxColliderComponent::offset)
    .member("density", &BoxColliderComponent::density)
    .member("friction", &BoxColliderComponent::friction)
    .member("restitution", &BoxColliderComponent::restitution)
    .finalize();

  bind_component<SphereColliderComponent>(world, state, core_table, "SphereColliderComponent")
    .member("radius", &SphereColliderComponent::radius)
    .member("offset", &SphereColliderComponent::offset)
    .member("density", &SphereColliderComponent::density)
    .member("friction", &SphereColliderComponent::friction)
    .member("restitution", &SphereColliderComponent::restitution)
    .finalize();

  bind_component<CapsuleColliderComponent>(world, state, core_table, "CapsuleColliderComponent")
    .member("height", &CapsuleColliderComponent::height)
    .member("radius", &CapsuleColliderComponent::radius)
    .member("offset", &CapsuleColliderComponent::offset)
    .member("density", &CapsuleColliderComponent::density)
    .member("friction", &CapsuleColliderComponent::friction)
    .member("restitution", &CapsuleColliderComponent::restitution)
    .finalize();

  bind_component<TaperedCapsuleColliderComponent>(world, state, core_table, "TaperedCapsuleColliderComponent")
    .member("height", &TaperedCapsuleColliderComponent::height)
    .member("top_radius", &TaperedCapsuleColliderComponent::top_radius)
    .member("bottom_radius", &TaperedCapsuleColliderComponent::bottom_radius)
    .member("offset", &TaperedCapsuleColliderComponent::offset)
    .member("density", &TaperedCapsuleColliderComponent::density)
    .member("friction", &TaperedCapsuleColliderComponent::friction)
    .member("restitution", &TaperedCapsuleColliderComponent::restitution)
    .finalize();

  bind_component<CylinderColliderComponent>(world, state, core_table, "CylinderColliderComponent")
    .member("height", &CylinderColliderComponent::height)
    .member("radius", &CylinderColliderComponent::radius)
    .member("offset", &CylinderColliderComponent::offset)
    .member("density", &CylinderColliderComponent::density)
    .member("friction", &CylinderColliderComponent::friction)
    .member("restitution", &CylinderColliderComponent::restitution)
    .finalize();

  bind_component<MeshColliderComponent>(world, state, core_table, "MeshColliderComponent")
    .member("offset", &MeshColliderComponent::offset)
    .member("friction", &MeshColliderComponent::friction)
    .member("restitution", &MeshColliderComponent::restitution)
    .finalize();

  bind_component<CharacterControllerComponent>(world, state, core_table, "CharacterControllerComponent")
    .member("character_height_standing", &CharacterControllerComponent::character_height_standing)
    .member("character_radius_standing", &CharacterControllerComponent::character_radius_standing)
    .member("character_height_crouching", &CharacterControllerComponent::character_height_crouching)
    .member("character_radius_crouching", &CharacterControllerComponent::character_radius_crouching)
    .member("interpolation", &CharacterControllerComponent::interpolation)
    .member("control_movement_during_jump", &CharacterControllerComponent::control_movement_during_jump)
    .member("jump_force", &CharacterControllerComponent::jump_force)
    .member("auto_bunny_hop", &CharacterControllerComponent::auto_bunny_hop)
    .member("air_control", &CharacterControllerComponent::air_control)
    .member("max_ground_speed", &CharacterControllerComponent::max_ground_speed)
    .member("ground_acceleration", &CharacterControllerComponent::ground_acceleration)
    .member("ground_deceleration", &CharacterControllerComponent::ground_deceleration)
    .member("max_air_speed", &CharacterControllerComponent::max_air_speed)
    .member("air_acceleration", &CharacterControllerComponent::air_acceleration)
    .member("air_deceleration", &CharacterControllerComponent::air_deceleration)
    .member("max_strafe_speed", &CharacterControllerComponent::max_strafe_speed)
    .member("strafe_acceleration", &CharacterControllerComponent::strafe_acceleration)
    .member("strafe_deceleration", &CharacterControllerComponent::strafe_deceleration)
    .member("friction", &CharacterControllerComponent::friction)
    .member("gravity", &CharacterControllerComponent::gravity)
    .member("collision_tolerance", &CharacterControllerComponent::collision_tolerance)
    .finalize();

  // Audio Components
  bind_component<AudioSourceComponent>(world, state, core_table, "AudioSourceComponent")
    .member("audio_source", &AudioSourceComponent::audio_source)
    .member("attenuation_model", &AudioSourceComponent::attenuation_model)
    .member("volume", &AudioSourceComponent::volume)
    .member("pitch", &AudioSourceComponent::pitch)
    .member("play_on_awake", &AudioSourceComponent::play_on_awake)
    .member("looping", &AudioSourceComponent::looping)
    .member("spatialization", &AudioSourceComponent::spatialization)
    .member("roll_off", &AudioSourceComponent::roll_off)
    .member("min_gain", &AudioSourceComponent::min_gain)
    .member("max_gain", &AudioSourceComponent::max_gain)
    .member("min_distance", &AudioSourceComponent::min_distance)
    .member("max_distance", &AudioSourceComponent::max_distance)
    .member("cone_inner_angle", &AudioSourceComponent::cone_inner_angle)
    .member("cone_outer_angle", &AudioSourceComponent::cone_outer_angle)
    .member("cone_outer_gain", &AudioSourceComponent::cone_outer_gain)
    .member("doppler_factor", &AudioSourceComponent::doppler_factor)
    .finalize();

  bind_component<AudioListenerComponent>(world, state, core_table, "AudioListenerComponent")
    .member("active", &AudioListenerComponent::active)
    .member("listener_index", &AudioListenerComponent::listener_index)
    .member("cone_inner_angle", &AudioListenerComponent::cone_inner_angle)
    .member("cone_outer_angle", &AudioListenerComponent::cone_outer_angle)
    .member("cone_outer_gain", &AudioListenerComponent::cone_outer_gain)
    .finalize();

  bind_component<TonemappingComponent>(world, state, core_table, "TonemappingComponent")
    .member("tonemap_type", &TonemappingComponent::tonemap_type)
    .finalize();
}
} // namespace ox
