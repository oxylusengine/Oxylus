// clang-format off
#ifndef ECS_REFLECT_TYPES

#include "Audio/AudioEngine.hpp"
#include "Core/UUID.hpp"
#include "Render/Utils/RectPacker.hpp"
#include "Utils/OxMath.hpp"

#ifndef ECS_COMPONENT_BEGIN
#define ECS_COMPONENT_BEGIN(...)
#endif

#ifndef ECS_COMPONENT_END
#define ECS_COMPONENT_END(...)
#endif

#ifndef ECS_COMPONENT_MEMBER
#define ECS_COMPONENT_MEMBER(...)
#endif

#ifndef ECS_COMPONENT_TAG
#define ECS_COMPONENT_TAG(...)
#endif

#endif

#ifndef ECS_REFLECT_TYPES
namespace ox {
#endif

ECS_COMPONENT_BEGIN(TransformComponent)
  ECS_COMPONENT_MEMBER(position, glm::vec3, {})
  ECS_COMPONENT_MEMBER(rotation, glm::vec3, {})
  ECS_COMPONENT_MEMBER(scale, glm::vec3, {1.0f, 1.0f, 1.0f})

#ifndef ECS_REFLECT_TYPES
  TransformComponent() = default;
  TransformComponent(glm::vec3 p, glm::vec3 r, glm::vec3 s) : position(p), rotation(r), scale(s) {}
  TransformComponent(const glm::vec3& translation) : position(translation) {}

  TransformComponent(const glm::mat4& transform_matrix) {
    math::decompose_transform(transform_matrix, position, rotation, scale);
  }

  void set_from_matrix(const glm::mat4& transform_matrix) {
    math::decompose_transform(transform_matrix, position, rotation, scale);
  }

  glm::mat4 get_local_transform() const {
    return glm::translate(glm::mat4(1.0f), position) * glm::toMat4(glm::quat(rotation)) *
           glm::scale(glm::mat4(1.0f), scale);
  }
#endif

ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(LayerComponent)
  ECS_COMPONENT_MEMBER(layer, u16, 1)
ECS_COMPONENT_END();

// Rendering
ECS_COMPONENT_BEGIN(MeshComponent)
  ECS_COMPONENT_MEMBER(mesh_index, u32, {})
  ECS_COMPONENT_MEMBER(cast_shadows, bool, true)
  ECS_COMPONENT_MEMBER(mesh_uuid, UUID, {})

#ifndef ECS_REFLECT_TYPES
  AABB aabb = {};
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(SpriteComponent)
  ECS_COMPONENT_MEMBER(layer, u32, 0)
  ECS_COMPONENT_MEMBER(sort_y, bool, true)
  ECS_COMPONENT_MEMBER(flip_x, bool, false)
  ECS_COMPONENT_MEMBER(material, ox::UUID, {})

#ifndef ECS_REFLECT_TYPES
  AABB rect = {};
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(SpriteAnimationComponent)
  ECS_COMPONENT_MEMBER(num_frames, u32, 0)
  ECS_COMPONENT_MEMBER(loop, bool, true)
  ECS_COMPONENT_MEMBER(inverted, bool, false)
  ECS_COMPONENT_MEMBER(fps, u32, 0)
  ECS_COMPONENT_MEMBER(columns, u32, 1)
  ECS_COMPONENT_MEMBER(frame_size, glm::vec2, {})

#ifndef ECS_REFLECT_TYPES
  float current_time = 0.f;

  void reset() { current_time = 0.f; }

  void set_frame_size(const u32 width,
                      const u32 height) {
    if (num_frames > 0) {
      const auto horizontal = width / num_frames;
      const auto vertical = height;

      frame_size = {horizontal, vertical};

      reset();
    }
  }

  void set_num_frames(u32 value) {
    num_frames = value;
    reset();
  }

  void set_fps(u32 value) {
    fps = value;
    reset();
  }

  void set_columns(u32 value) {
    columns = value;
    reset();
  }
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(CameraComponent)
#ifndef ECS_REFLECT_TYPES
  enum Projection {
    Perspective = 0,
    Orthographic = 1,
  };
#endif
  ECS_COMPONENT_MEMBER(projection, u32, Projection::Perspective)
  ECS_COMPONENT_MEMBER(fov, f32, 60.f)
  ECS_COMPONENT_MEMBER(aspect, f32, 16.f / 9.f)
  ECS_COMPONENT_MEMBER(far_clip, f32, 1000.f)
  ECS_COMPONENT_MEMBER(near_clip, f32, 0.01f)

  ECS_COMPONENT_MEMBER(tilt, f32, 0.0f)
  ECS_COMPONENT_MEMBER(zoom, f32, 1.0f)

#ifndef ECS_REFLECT_TYPES
  glm::vec2 jitter = {};
  glm::vec2 jitter_prev = {};
  f32 yaw = -1.5708f;
  f32 pitch = 0.f;

  struct Matrices {
    glm::mat4 view_matrix = {};
    glm::mat4 projection_matrix = {};
  };

  Matrices matrices = {};
  Matrices matrices_prev = {};

  glm::vec3 position = {};
  glm::vec3 forward = {};
  glm::vec3 up = {};
  glm::vec3 right = {};

  glm::mat4 get_projection_matrix() const { return matrices.projection_matrix; }
  glm::mat4 get_inv_projection_matrix() const { return glm::inverse(matrices.projection_matrix); }
  glm::mat4 get_view_matrix() const { return matrices.view_matrix; }
  glm::mat4 get_inv_view_matrix() const { return glm::inverse(matrices.view_matrix); }
  glm::mat4 get_inverse_projection_view() const {
    return glm::inverse(matrices.projection_matrix * matrices.view_matrix);
  }

  glm::mat4 get_previous_projection_matrix() const { return matrices_prev.projection_matrix; }
  glm::mat4 get_previous_inv_projection_matrix() const { return glm::inverse(matrices_prev.projection_matrix); }
  glm::mat4 get_previous_view_matrix() const { return matrices_prev.view_matrix; }
  glm::mat4 get_previous_inv_view_matrix() const { return glm::inverse(matrices_prev.view_matrix); }
  glm::mat4 get_previous_inverse_projection_view() const {
    return glm::inverse(matrices_prev.projection_matrix * matrices_prev.view_matrix);
  }
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(ParticleSystemComponent)
  ECS_COMPONENT_MEMBER(material, UUID, {})
  ECS_COMPONENT_MEMBER(duration, f32, 3.f)
  ECS_COMPONENT_MEMBER(looping, bool, true)
  ECS_COMPONENT_MEMBER(start_delay, f32, 0.f)
  ECS_COMPONENT_MEMBER(start_lifetime, f32, 3.0f)
  ECS_COMPONENT_MEMBER(start_velocity, glm::vec3, {0.f, 2.f, 0.f})
  ECS_COMPONENT_MEMBER(start_color, glm::vec4, {1.f, 1.f, 1.f, 1.f})
  ECS_COMPONENT_MEMBER(start_size, glm::vec4, {1.f, 1.f, 1.f, 1.f})
  ECS_COMPONENT_MEMBER(start_rotation, glm::vec4, {1.f, 1.f, 1.f, 1.f})
  ECS_COMPONENT_MEMBER(gravity_modifier, f32, 0.f)
  ECS_COMPONENT_MEMBER(simulation_speed, f32, 1.f)
  ECS_COMPONENT_MEMBER(play_on_awake, bool, true)
  ECS_COMPONENT_MEMBER(max_particles, u32, 100)
  ECS_COMPONENT_MEMBER(rate_over_time, u32, 10)
  ECS_COMPONENT_MEMBER(rate_over_distance, u32, 0)
  ECS_COMPONENT_MEMBER(burst_count, u32, 0)
  ECS_COMPONENT_MEMBER(position_start, glm::vec3, {-0.2f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(position_end, glm::vec3, {0.2f, 0.f, 0.f})

  ECS_COMPONENT_MEMBER(velocity_over_lifetime_enabled, bool, false)
  ECS_COMPONENT_MEMBER(velocity_over_lifetime_start, glm::vec3, {})
  ECS_COMPONENT_MEMBER(velocity_over_lifetime_end, glm::vec3, {})

  ECS_COMPONENT_MEMBER(force_over_lifetime_enabled, bool, false)
  ECS_COMPONENT_MEMBER(force_over_lifetime_start, glm::vec3, {})
  ECS_COMPONENT_MEMBER(force_over_lifetime_end, glm::vec3, {})

  ECS_COMPONENT_MEMBER(color_over_lifetime_enabled, bool, false)
  ECS_COMPONENT_MEMBER(color_over_lifetime_start, glm::vec4, {0.8f, 0.2f, 0.2f, 0.0f})
  ECS_COMPONENT_MEMBER(color_over_lifetime_end, glm::vec4, {0.2f, 0.2f, 0.75f, 1.0f})

  ECS_COMPONENT_MEMBER(color_by_speed_enabled, bool, false)
  ECS_COMPONENT_MEMBER(color_by_speed_start, glm::vec4, {0.8f, 0.2f, 0.2f, 0.0f})
  ECS_COMPONENT_MEMBER(color_by_speed_end, glm::vec4, {0.2f, 0.2f, 0.75f, 1.0f})
  ECS_COMPONENT_MEMBER(color_by_speed_min_speed, f32, 0.f)
  ECS_COMPONENT_MEMBER(color_by_speed_max_speed, f32, 1.f)

  ECS_COMPONENT_MEMBER(size_over_lifetime_enabled, bool, false)
  ECS_COMPONENT_MEMBER(size_over_lifetime_start, glm::vec3, glm::vec3(0.2f))
  ECS_COMPONENT_MEMBER(size_over_lifetime_end, glm::vec3, glm::vec3(1.0f))

  ECS_COMPONENT_MEMBER(size_by_speed_enabled, bool, false)
  ECS_COMPONENT_MEMBER(size_by_speed_start, glm::vec3, glm::vec3(0.2f))
  ECS_COMPONENT_MEMBER(size_by_speed_end, glm::vec3, glm::vec3(1.0f))
  ECS_COMPONENT_MEMBER(size_by_speed_min_speed, f32, 0.f)
  ECS_COMPONENT_MEMBER(size_by_speed_max_speed, f32, 1.f)

  ECS_COMPONENT_MEMBER(rotation_over_lifetime_enabled, bool, false)
  ECS_COMPONENT_MEMBER(rotation_over_lifetime_start, glm::vec3, {})
  ECS_COMPONENT_MEMBER(rotation_over_lifetime_end, glm::vec3, {})

  ECS_COMPONENT_MEMBER(rotation_by_speed_enabled, bool, false)
  ECS_COMPONENT_MEMBER(rotation_by_speed_start, glm::vec3, {})
  ECS_COMPONENT_MEMBER(rotation_by_speed_end, glm::vec3, {})
  ECS_COMPONENT_MEMBER(rotation_by_speed_min_speed, f32, 0.f)
  ECS_COMPONENT_MEMBER(rotation_by_speed_max_speed, f32, 1.f)

#ifndef ECS_REFLECT_TYPES
  std::vector<u64> particles = {};
  u32 pool_index = 0;
  float system_time = 0.0f;
  float burst_time = 0.0f;
  float spawn_time = 0.0f;
  glm::vec3 last_spawned_position = glm::vec3(0.0f);
  uint32_t active_particle_count = 0;
  bool playing = false;
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(ParticleComponent)
  ECS_COMPONENT_MEMBER(color, glm::vec4, {})
  ECS_COMPONENT_MEMBER(life_remaining, f32, 0.f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(LightComponent)
#ifndef ECS_REFLECT_TYPES
  enum LightType : u32 { Directional = 0, Point, Spot };
#endif
  ECS_COMPONENT_MEMBER(type, u32, LightType::Point)
  ECS_COMPONENT_MEMBER(color, glm::vec3, {1.0f, 1.0f, 1.0f})
  ECS_COMPONENT_MEMBER(intensity, f32, 1.0f)
  ECS_COMPONENT_MEMBER(radius, f32, 1.0f)
  ECS_COMPONENT_MEMBER(outer_cone_angle, f32, 70)
  ECS_COMPONENT_MEMBER(inner_cone_angle, f32, 0.0f)
  ECS_COMPONENT_MEMBER(cast_shadows, bool, true)
  ECS_COMPONENT_MEMBER(shadow_map_res, u32, 2048)
  ECS_COMPONENT_MEMBER(cascade_count, u32, 4)
  ECS_COMPONENT_MEMBER(first_cascade_far_bound, f32, 30.0f)
  ECS_COMPONENT_MEMBER(maximum_shadow_distance, f32, 150.0f)
  ECS_COMPONENT_MEMBER(minimum_shadow_distance, f32, 0.01f)
  ECS_COMPONENT_MEMBER(cascade_overlap_propotion, f32, 0.2f)
  ECS_COMPONENT_MEMBER(depth_bias, f32, 0.002f)
  ECS_COMPONENT_MEMBER(normal_bias, f32, 1.8f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(AtmosphereComponent)
  ECS_COMPONENT_MEMBER(rayleigh_scattering, glm::vec3, { 5.802f, 13.558f, 33.100f })
  ECS_COMPONENT_MEMBER(rayleigh_density, f32, 8.0)
  ECS_COMPONENT_MEMBER(mie_scattering, glm::vec3, { 3.996f, 3.996f, 3.996f })
  ECS_COMPONENT_MEMBER(mie_density, f32, 1.2f)
  ECS_COMPONENT_MEMBER(mie_extinction, f32, 4.44f)
  ECS_COMPONENT_MEMBER(mie_asymmetry, f32, 3.6f)
  ECS_COMPONENT_MEMBER(ozone_absorption, glm::vec3, { 0.650f, 1.881f, 0.085f })
  ECS_COMPONENT_MEMBER(ozone_height, f32, 25.0f)
  ECS_COMPONENT_MEMBER(ozone_thickness, f32, 15.0f)
  ECS_COMPONENT_MEMBER(aerial_perspective_start_km, f32, 8.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(AutoExposureComponent)
  ECS_COMPONENT_MEMBER(min_exposure, f32, -6.f)
  ECS_COMPONENT_MEMBER(max_exposure, f32, 18.f)
  ECS_COMPONENT_MEMBER(adaptation_speed, f32, 1.1f)
  ECS_COMPONENT_MEMBER(ev100_bias, f32, 1.f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(VignetteComponent)
  ECS_COMPONENT_MEMBER(amount, f32, 0.5f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(ChromaticAberrationComponent)
  ECS_COMPONENT_MEMBER(amount, f32, 0.5f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(FilmGrainComponent)
  ECS_COMPONENT_MEMBER(amount, f32, 0.6f)
  ECS_COMPONENT_MEMBER(scale, f32, 0.7f)
ECS_COMPONENT_END();

// Physics
ECS_COMPONENT_BEGIN(RigidBodyComponent)
#ifndef ECS_REFLECT_TYPES
  enum BodyType { Static = 0, Kinematic, Dynamic };
  enum AllowedDOFs : u32 {
    None = 0b000000, ///< No degrees of freedom are allowed. Note that this is not valid and will crash. Use a static
                     ///< body instead.
    All = 0b111111,  ///< All degrees of freedom are allowed
    TranslationX = 0b000001,                           ///< Body can move in world space X axis
    TranslationY = 0b000010,                           ///< Body can move in world space Y axis
    TranslationZ = 0b000100,                           ///< Body can move in world space Z axis
    RotationX = 0b001000,                              ///< Body can rotate around world space X axis
    RotationY = 0b010000,                              ///< Body can rotate around world space Y axis
    RotationZ = 0b100000,                              ///< Body can rotate around world space Z axis
    Plane2D = TranslationX | TranslationY | RotationZ, ///< Body can only move in X and Y axis and rotate around Z axis
  };
#endif
  ECS_COMPONENT_MEMBER(allowed_dofs, u32, AllowedDOFs::All)
  ECS_COMPONENT_MEMBER(type, u32, BodyType::Dynamic)
  ECS_COMPONENT_MEMBER(mass, f32, 1.0f)
  ECS_COMPONENT_MEMBER(linear_drag, f32, 0.0f)
  ECS_COMPONENT_MEMBER(angular_drag, f32, 0.05f)
  ECS_COMPONENT_MEMBER(gravity_scale, f32, 1.0f)
  ECS_COMPONENT_MEMBER(allow_sleep, bool, true)
  ECS_COMPONENT_MEMBER(awake, bool, true)
  ECS_COMPONENT_MEMBER(continuous, bool, false)
  ECS_COMPONENT_MEMBER(interpolation, bool, false)
  ECS_COMPONENT_MEMBER(is_sensor, bool, false)

#ifndef ECS_REFLECT_TYPES
  // Stored as JPH::Body
  void* runtime_body = nullptr;

  // For interpolation/extrapolation
  glm::vec3 previous_translation = glm::vec3(0.0f);
  glm::quat previous_rotation = glm::vec3(0.0f);
  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::vec3(0.0f);
#endif
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(BoxColliderComponent)
  ECS_COMPONENT_MEMBER(size, glm::vec3, {0.5f, 0.5f, 0.5f})
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(density, f32, 1.0f)
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(SphereColliderComponent)
  ECS_COMPONENT_MEMBER(radius, f32, .5f)
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(density, f32, 1.0f)
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(CapsuleColliderComponent)
  ECS_COMPONENT_MEMBER(height, f32, 1.f)
  ECS_COMPONENT_MEMBER(radius, f32, .5f)
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(density, f32, 1.0f)
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(TaperedCapsuleColliderComponent)
  ECS_COMPONENT_MEMBER(height, f32, 1.f)
  ECS_COMPONENT_MEMBER(top_radius, f32, .5f)
  ECS_COMPONENT_MEMBER(bottom_radius, f32, .5f)
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(density, f32, 1.0f)
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(CylinderColliderComponent)
  ECS_COMPONENT_MEMBER(height, f32, 1.f)
  ECS_COMPONENT_MEMBER(radius, f32, .5f)
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(density, f32, 1.0f)
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(MeshColliderComponent)
  ECS_COMPONENT_MEMBER(offset, glm::vec3, {0.f, 0.f, 0.f})
  ECS_COMPONENT_MEMBER(friction, f32, 0.5f)
  ECS_COMPONENT_MEMBER(restitution, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(CharacterControllerComponent)
  // Size
  ECS_COMPONENT_MEMBER(character_height_standing, f32, 1.35f)
  ECS_COMPONENT_MEMBER(character_radius_standing, f32, 0.3f)
  ECS_COMPONENT_MEMBER(character_height_crouching, f32, 0.8f)
  ECS_COMPONENT_MEMBER(character_radius_crouching, f32, 0.3f)

  ECS_COMPONENT_MEMBER(interpolation, bool, true)

  ECS_COMPONENT_MEMBER(control_movement_during_jump, bool, true)
  ECS_COMPONENT_MEMBER(jump_force, f32, 8.0f)
  ECS_COMPONENT_MEMBER(auto_bunny_hop, bool, false)
  ECS_COMPONENT_MEMBER(air_control, f32, 0.3f)

  ECS_COMPONENT_MEMBER(max_ground_speed, f32, 7.f)
  ECS_COMPONENT_MEMBER(ground_acceleration, f32, 14.f)
  ECS_COMPONENT_MEMBER(ground_deceleration, f32, 10.f)

  ECS_COMPONENT_MEMBER(max_air_speed, f32, 7.f)
  ECS_COMPONENT_MEMBER(air_acceleration, f32, 2.f)
  ECS_COMPONENT_MEMBER(air_deceleration, f32, 2.f)

  ECS_COMPONENT_MEMBER(max_strafe_speed, f32, 0.0f)
  ECS_COMPONENT_MEMBER(strafe_acceleration, f32, 50.f)
  ECS_COMPONENT_MEMBER(strafe_deceleration, f32, 50.f)

  ECS_COMPONENT_MEMBER(friction, f32, 6.0f)
  ECS_COMPONENT_MEMBER(gravity, f32, 20.f)
  ECS_COMPONENT_MEMBER(collision_tolerance, f32, 0.05f)

#ifndef ECS_REFLECT_TYPES
  void* character = nullptr; // Stored as JPHCharacter

  // For interpolation/extrapolation
  glm::vec3 previous_translation = glm::vec3(0.0f);
  glm::quat previous_rotation = glm::vec3(0.0f);
  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::vec3(0.0f);
#endif
ECS_COMPONENT_END();

// Audio
ECS_COMPONENT_BEGIN(AudioSourceComponent)
  ECS_COMPONENT_MEMBER(audio_source, UUID, {})

  ECS_COMPONENT_MEMBER(attenuation_model, u32, AudioEngine::AttenuationModelType::Inverse)
  ECS_COMPONENT_MEMBER(volume, f32, 1.0f)
  ECS_COMPONENT_MEMBER(pitch, f32, 1.0f)
  ECS_COMPONENT_MEMBER(play_on_awake, bool, true)
  ECS_COMPONENT_MEMBER(looping, bool, false)

  ECS_COMPONENT_MEMBER(spatialization, bool , false)
  ECS_COMPONENT_MEMBER(roll_off, f32, 1.0f)
  ECS_COMPONENT_MEMBER(min_gain, f32, 0.0f)
  ECS_COMPONENT_MEMBER(max_gain, f32, 1.0f)
  ECS_COMPONENT_MEMBER(min_distance, f32, 0.3f)
  ECS_COMPONENT_MEMBER(max_distance, f32, 1000.0f)

  ECS_COMPONENT_MEMBER(cone_inner_angle, f32, glm::radians(360.0f))
  ECS_COMPONENT_MEMBER(cone_outer_angle, f32, glm::radians(360.0f))
  ECS_COMPONENT_MEMBER(cone_outer_gain, f32, 0.0f)

  ECS_COMPONENT_MEMBER(doppler_factor, f32, 1.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_BEGIN(AudioListenerComponent)
  ECS_COMPONENT_MEMBER(active, bool, false)
  ECS_COMPONENT_MEMBER(listener_index, u32, 0)
  ECS_COMPONENT_MEMBER(cone_inner_angle, f32, glm::radians(360.0f))
  ECS_COMPONENT_MEMBER(cone_outer_angle, f32, glm::radians(360.0f))
  ECS_COMPONENT_MEMBER(cone_outer_gain, f32, 0.0f)
ECS_COMPONENT_END();

ECS_COMPONENT_TAG(Hidden);

ECS_COMPONENT_TAG(AssetOwner);

#ifndef ECS_REFLECT_TYPES
} // namespace ox
#endif
// clang-format on
