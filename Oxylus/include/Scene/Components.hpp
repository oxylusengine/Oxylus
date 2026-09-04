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

  AABB baked_aabb = {};
  AABB world_aabb = {};
};

struct SpriteComponent {
  u32 layer = 0;
  bool sort_y = true;
  bool flip_x = false;
  UUID material = {};

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
  Projection projection = Projection::Perspective;
  f32 fov = 60.f;
  f32 aspect = 16.f / 9.f;
  f32 far_clip = 1000.f;
  f32 near_clip = 0.01f;

  f32 tilt = 0.0f;
  f32 zoom = 1.0f;

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
  UUID particle_system = {};
  bool play_on_awake = true;
  f32 simulation_speed = 1.0f;
  u32 seed = 0;
  f32 emission_rate_scale = 1.0f;
  // world-space velocity added to every particle at spawn, on top of the spawn graph
  glm::vec3 velocity_offset = {};

  bool override_parameters = false;
  glm::vec4 param0 = {};
  glm::vec4 param1 = {};
  glm::vec4 param2 = {};
  glm::vec4 param3 = {};

  template <typename Self>
  auto parameter(this Self& self, const u32 index) -> auto& {
    switch (index) {
      case 1 : return self.param1;
      case 2 : return self.param2;
      case 3 : return self.param3;
      default: return self.param0;
    }
  }
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
  f32 first_cascade_far_bound = 10.0f;
  f32 maximum_shadow_distance = 1000.0f;
  f32 minimum_shadow_distance = 0.01f;
  f32 first_clipmap_width = 10.0f;
  f32 clipmap_selection_bias = -0.5f;
};

struct ProbeVolumeComponent {
  glm::uvec3 probe_counts = {32, 32, 32};
  glm::vec3 probe_range = {64.0f, 64.0f, 64.0f};
  u32 cascade_count = 6;
  f32 cascade_blend = 0.3f;
  bool follow_camera = true;
};

struct SkyComponent {
  glm::vec4 solid_color = glm::vec4{0.f, 0.f, 0.f, 1.0f};
  glm::vec3 ambient_color = glm::vec3{0.03f};
  UUID texture = {};
};

struct TerrainComponent {
  glm::vec2 world_size = {1024.0f, 1024.0f};
  glm::vec2 height_range = {0.0f, 400.0f};
  u32 resolution = 2048;
  u32 patch_count = 64;

  f32 target_edge_pixels = 16.0f;
  f32 max_tessellation = 64.0f;

  f32 domain_size = 2.0f;
  f32 height_frequency = 3.0f;
  f32 height_amplitude = 0.125f;
  f32 height_lacunarity = 2.0f;
  f32 height_gain = 0.1f;
  u32 height_octaves = 3;
  u32 seed = 0;

  f32 erosion_scale = 0.15f;
  f32 erosion_strength = 0.22f;
  f32 gully_weight = 0.5f;
  f32 detail = 1.5f;
  f32 ridge_rounding = 0.1f;
  f32 crease_rounding = 0.0f;
  u32 erosion_octaves = 5;

  // Splat thresholds.
  f32 slope_rock_begin = 0.55f;
  f32 slope_rock_end = 0.8f;
  f32 altitude_snow_begin = 0.7f;
  f32 altitude_snow_end = 0.85f;

  UUID terrain_edits = {};

  UUID layer_grass = {};
  UUID layer_rock = {};
  UUID layer_drainage = {};
  UUID layer_snow = {};
  // World-space size, in metres, of one tile of each layer texture.
  f32 layer_tiling = 8.0f;
  // Slope above which triplanar projection takes over, so cliffs do not stretch.
  f32 triplanar_begin = 0.5f;

  bool collision_enabled = true;
  u32 collision_resolution = 256;
  f32 collision_friction = 0.5f;
  f32 collision_restitution = 0.0f;
};

struct AtmosphereComponent {
  glm::vec3 rayleigh_scattering = {5.802f, 13.558f, 33.100f};
  f32 rayleigh_density = 8.0;
  glm::vec3 mie_scattering = {3.996f, 3.996f, 3.996f};
  f32 mie_density = 1.2f;
  f32 mie_extinction = 4.44f;
  f32 mie_asymmetry = 3.6f;
  f32 mie_haze_amount = 0.7f;
  f32 mie_haze_scale_height = 11.0f;
  glm::vec3 ozone_absorption = {0.650f, 1.881f, 0.085f};
  f32 ozone_height = 25.0f;
  f32 ozone_thickness = 15.0f;
  f32 aerial_perspective_start_km = 8.0f;
  f32 aerial_perspective_exposure = 1.0f;
};

struct AutoExposureComponent {
  f32 min_exposure = -11.5f;
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
  BodyType type = BodyType::Dynamic;
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

// Collides against the triangles of the entity's MeshComponent. Jolt's triangle mesh shape has no
// inertia, so it only works on a Static or Kinematic body: set `convex` for a Dynamic one and the
// mesh is replaced by its convex hull.
struct MeshColliderComponent {
  glm::vec3 offset = {0.f, 0.f, 0.f};
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
  bool convex = false;
  f32 density = 1000.f;
};

// Goes on the chassis entity, which must also carry a dynamic RigidBodyComponent and a collider.
// Wheels are child entities carrying VehicleWheelComponent: their local transform gives the
// suspension attachment point, and the constraint drives it back so a wheel mesh animates for free.
struct VehicleComponent {
  enum DriveMode : u32 { FrontWheelDrive = 0, RearWheelDrive, AllWheelDrive };
  // How each wheel probes the ground. Ray is cheapest, cylinder is most accurate over rough terrain.
  enum CollisionMode : u32 { Ray = 0, SphereCast, CylinderCast };

  DriveMode drive_mode = DriveMode::RearWheelDrive;
  CollisionMode collision_mode = CollisionMode::CylinderCast;

  // Local-space chassis axes, must match how the model is authored.
  glm::vec3 up = {0.f, 1.f, 0.f};
  glm::vec3 forward = {0.f, 0.f, 1.f};
  // Degrees. Caps how far the rig can pitch or roll before the constraint fights it. 180 disables.
  f32 max_pitch_roll_angle = 60.f;

  // Engine
  f32 max_engine_torque = 500.f;
  f32 min_engine_rpm = 1000.f;
  f32 max_engine_rpm = 6000.f;
  f32 engine_inertia = 0.5f;

  // Transmission
  bool auto_transmission = true;
  f32 clutch_strength = 10.f;

  // Ratio max/min average wheel speed per differential before torque is shifted to the slower one.
  f32 limited_slip_ratio = 1.4f;

  // Driver input, written every frame by gameplay. forward and right are [-1,1], brakes are [0,1].
  f32 input_forward = 0.f;
  f32 input_right = 0.f;
  f32 input_brake = 0.f;
  f32 input_hand_brake = 0.f;

  // Stored as JPH::VehicleConstraint. Owned by the physics system once added, not by this component.
  void* runtime_constraint = nullptr;
};

struct VehicleWheelComponent {
  glm::vec3 attachment = {0.f, 0.f, 0.f};

  f32 radius = 0.5f;
  f32 width = 0.3f;

  f32 suspension_min_length = 0.3f;
  f32 suspension_max_length = 0.5f;
  f32 suspension_frequency = 1.5f;
  f32 suspension_damping = 0.5f;

  // Degrees. Zero leaves the wheel fixed, which is what you want on a trailer or rear axle.
  f32 max_steer_angle = 0.f;
  f32 max_brake_torque = 1500.f;
  f32 max_hand_brake_torque = 0.f;
  // Whether this wheel is on a differential and receives engine torque.
  bool driven = true;

  // Index into the constraint's wheel array, assigned when the vehicle is created.
  u32 runtime_wheel_index = 0;
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

struct Networked {};

struct CoreComponentsModule {
  CoreComponentsModule(flecs::world& world);
};

} // namespace ox
