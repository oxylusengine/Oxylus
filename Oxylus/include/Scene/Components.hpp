#pragma once

#include <flecs.h>

#include "Audio/AudioEngine.hpp"
#include "Core/UUID.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
struct TransformComponent {
  glm::vec3 position = {};
  glm::quat rotation = glm::quat::wxyz(1.0, 0.0, 0.0, 0.0);
  glm::vec3 scale = {1.0f, 1.0f, 1.0f};

  glm::mat4 get_local_transform() const {
    return glm::translate(glm::mat4(1.0f), position) * glm::toMat4(rotation) * glm::scale(glm::mat4(1.0f), scale);
  }
};

struct LayerComponent {
  u16 layer = 1;
};

// Rendering
struct MeshComponent {
  UUID model_uuid = {};
  u32 mesh_index = {};
  UUID material_uuid = {};
  bool cast_shadows = true;

  AABB aabb = {};
};

struct SpriteComponent {
  u32 layer = 0;
  bool sort_y = true;
  bool flip_x = false;
  ox::UUID material = {};

  AABB rect = {};
};

struct SpriteAnimationComponent {
  u32 num_frames = 0;
  bool loop = true;
  bool inverted = false;
  u32 fps = 0;
  u32 columns = 1;
  glm::vec2 frame_size = {};

  float current_time = 0.f;

  void reset() { current_time = 0.f; }

  void set_frame_size(const u32 width, const u32 height) {
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
};

struct CameraComponent {
  enum Projection {
    Perspective = 0,
    Orthographic = 1,
  };
  u32 projection = Projection::Perspective;
  f32 fov = 60.f;
  f32 aspect = 16.f / 9.f;
  f32 far_clip = 1000.f;
  f32 near_clip = 0.01f;

  f32 tilt = 0.0f;
  f32 zoom = 1.0f;

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
};

struct ParticleSystemComponent {
  UUID material = {};
  f32 duration = 3.f;
  bool looping = true;
  f32 start_delay = 0.f;
  f32 start_lifetime = 3.0f;
  glm::vec3 start_velocity = {0.f, 2.f, 0.f};
  glm::vec4 start_color = {1.f, 1.f, 1.f, 1.f};
  glm::vec4 start_size = {1.f, 1.f, 1.f, 1.f};
  glm::quat start_rotation = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
  f32 gravity_modifier = 0.f;
  f32 simulation_speed = 1.f;
  bool play_on_awake = true;
  u32 max_particles = 100;
  u32 rate_over_time = 10;
  u32 rate_over_distance = 0;
  u32 burst_count = 0;
  glm::vec3 position_start = {-0.2f, 0.f, 0.f};
  glm::vec3 position_end = {0.2f, 0.f, 0.f};

  bool velocity_over_lifetime_enabled = false;
  glm::vec3 velocity_over_lifetime_start = {};
  glm::vec3 velocity_over_lifetime_end = {};

  bool force_over_lifetime_enabled = false;
  glm::vec3 force_over_lifetime_start = {};
  glm::vec3 force_over_lifetime_end = {};

  bool color_over_lifetime_enabled = false;
  glm::vec4 color_over_lifetime_start = {0.8f, 0.2f, 0.2f, 0.0f};
  glm::vec4 color_over_lifetime_end = {0.2f, 0.2f, 0.75f, 1.0f};

  bool color_by_speed_enabled = false;
  glm::vec4 color_by_speed_start = {0.8f, 0.2f, 0.2f, 0.0f};
  glm::vec4 color_by_speed_end = {0.2f, 0.2f, 0.75f, 1.0f};
  f32 color_by_speed_min_speed = 0.f;
  f32 color_by_speed_max_speed = 1.f;

  bool size_over_lifetime_enabled = false;
  glm::vec3 size_over_lifetime_start = glm::vec3(0.2f);
  glm::vec3 size_over_lifetime_end = glm::vec3(1.0f);

  bool size_by_speed_enabled = false;
  glm::vec3 size_by_speed_start = glm::vec3(0.2f);
  glm::vec3 size_by_speed_end = glm::vec3(1.0f);
  f32 size_by_speed_min_speed = 0.f;
  f32 size_by_speed_max_speed = 1.f;

  bool rotation_over_lifetime_enabled = false;
  glm::quat rotation_over_lifetime_start = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::quat rotation_over_lifetime_end = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);

  bool rotation_by_speed_enabled = false;
  glm::quat rotation_by_speed_start = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::quat rotation_by_speed_end = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  f32 rotation_by_speed_min_speed = 0.f;
  f32 rotation_by_speed_max_speed = 1.f;

  std::vector<u64> particles = {};
  u32 pool_index = 0;
  float system_time = 0.0f;
  float burst_time = 0.0f;
  float spawn_time = 0.0f;
  glm::vec3 last_spawned_position = glm::vec3(0.0f);
  uint32_t active_particle_count = 0;
  bool playing = false;
};

struct ParticleComponent {
  glm::vec4 color = {};
  f32 life_remaining = 0.f;
};

struct LightComponent {
  enum LightType : u32 { Directional = 0, Spot, Point };

  LightType type = LightType::Point;
  glm::vec3 color = {0.02f, 0.02f, 0.02f};
  f32 intensity = 10.0f;
  f32 radius = 1.0f;
  f32 outer_cone_angle = 70;
  f32 inner_cone_angle = 0.0f;
  bool cast_shadows = true;
  u32 shadow_map_res = 2048;
  u32 cascade_count = 4;
  f32 first_cascade_far_bound = 10.0f;
  f32 maximum_shadow_distance = 1000.0f;
  f32 minimum_shadow_distance = 0.01f;
  f32 cascade_overlap_propotion = 0.2f;
  f32 depth_bias = 0.002f;
  f32 normal_bias = 1.8f;
};

struct AtmosphereComponent {
  glm::vec3 rayleigh_scattering = {5.802f, 13.558f, 33.100f};
  f32 rayleigh_density = 8.0;
  glm::vec3 mie_scattering = {3.996f, 3.996f, 3.996f};
  f32 mie_density = 1.2f;
  f32 mie_extinction = 4.44f;
  f32 mie_asymmetry = 3.6f;
  glm::vec3 ozone_absorption = {0.650f, 1.881f, 0.085f};
  f32 ozone_height = 25.0f;
  f32 ozone_thickness = 15.0f;
  f32 aerial_perspective_start_km = 8.0f;
  f32 aerial_perspective_exposure = 1.0f;
};

struct AutoExposureComponent {
  f32 min_exposure = -6.f;
  f32 max_exposure = 18.f;
  f32 adaptation_speed = 1.1f;
  f32 ev100_bias = 1.f;
};

struct VignetteComponent {
  f32 amount = 0.5f;
};

struct ChromaticAberrationComponent {
  f32 amount = 0.5f;
};

struct FilmGrainComponent {
  f32 amount = 0.6f;
  f32 scale = 0.7f;
};

struct TonemappingComponent {
  GPU::TonemapType tonemap_type = GPU::TonemapType::AgX;
};

// Physics
struct RigidBodyComponent {
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
  u32 allowed_dofs = AllowedDOFs::All;
  u32 type = BodyType::Dynamic;
  f32 mass = 1.0f;
  f32 linear_drag = 0.05f;
  f32 angular_drag = 0.05f;
  f32 gravity_factor = 1.0f;
  f32 friction = 0.2f;
  f32 restitution = 0.0f;
  bool allow_sleep = true;
  bool awake = true;
  bool continuous = false;
  bool interpolation = false;
  bool is_sensor = false;

  // Stored as JPH::Body
  void* runtime_body = nullptr;

  // For interpolation/extrapolation
  glm::vec3 previous_translation = glm::vec3(0.0f);
  glm::quat previous_rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
};

struct BoxColliderComponent {
  glm::vec3 size = {0.5f, 0.5f, 0.5f};
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 density = 1.0f;
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct SphereColliderComponent {
  f32 radius = .5f;
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 density = 1.0f;
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct CapsuleColliderComponent {
  f32 height = 1.f;
  f32 radius = .5f;
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 density = 1.0f;
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct TaperedCapsuleColliderComponent {
  f32 height = 1.f;
  f32 top_radius = .5f;
  f32 bottom_radius = .5f;
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 density = 1.0f;
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct CylinderColliderComponent {
  f32 height = 1.f;
  f32 radius = .5f;
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 density = 1.0f;
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct MeshColliderComponent {
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
};

struct CharacterControllerComponent {
  // Size
  f32 character_height_standing = 1.35f;
  f32 character_radius_standing = 0.3f;
  f32 character_height_crouching = 0.8f;
  f32 character_radius_crouching = 0.3f;

  bool interpolation = true;

  bool control_movement_during_jump = true;
  f32 jump_force = 8.0f;
  bool auto_bunny_hop = false;
  f32 air_control = 0.3f;

  f32 max_ground_speed = 7.f;
  f32 ground_acceleration = 14.f;
  f32 ground_deceleration = 10.f;

  f32 max_air_speed = 7.f;
  f32 air_acceleration = 2.f;
  f32 air_deceleration = 2.f;

  f32 max_strafe_speed = 0.0f;
  f32 strafe_acceleration = 50.f;
  f32 strafe_deceleration = 50.f;

  f32 friction = 6.0f;
  f32 gravity = 20.f;
  f32 collision_tolerance = 0.05f;

  void* character = nullptr; // Stored as JPHCharacter

  // For interpolation/extrapolation
  glm::vec3 previous_translation = glm::vec3(0.0f);
  glm::quat previous_rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
};

// Audio
struct AudioSourceComponent {
  UUID audio_source = {};

  u32 attenuation_model = AudioEngine::AttenuationModelType::Inverse;
  f32 volume = 1.0f;
  f32 pitch = 1.0f;
  bool play_on_awake = true;
  bool looping = false;

  bool spatialization = false;
  f32 roll_off = 1.0f;
  f32 min_gain = 0.0f;
  f32 max_gain = 1.0f;
  f32 min_distance = 0.3f;
  f32 max_distance = 1000.0f;

  f32 cone_inner_angle = glm::radians(360.0f);
  f32 cone_outer_angle = glm::radians(360.0f);
  f32 cone_outer_gain = 0.0f;

  f32 doppler_factor = 1.0f;
};

struct AudioListenerComponent {
  bool active = false;
  u32 listener_index = 0;
  f32 cone_inner_angle = glm::radians(360.0f);
  f32 cone_outer_angle = glm::radians(360.0f);
  f32 cone_outer_gain = 0.0f;
};

struct Hidden {};

struct AssetOwner {};

struct Networked {};

struct CoreComponentsModule {
  CoreComponentsModule(flecs::world& world);
};

} // namespace ox
