#pragma once

#include <vuk/Types.hpp>

#include "Asset/Material.hpp"

namespace ox::GPU {
enum class TransformID : u64 { Invalid = ~0_u64 };
struct Transforms {
  alignas(4) glm::mat4 local = {};
  alignas(4) glm::mat4 world = {};
  alignas(4) glm::mat3 normal = {};
};

enum class DebugView : i32 {
  None = 0,
  Triangles,
  Meshlets,
  Overdraw,
  Albedo,
  Normal,
  Emissive,
  Metallic,
  Roughness,
  Occlusion,
  HiZ,

  Count,
};

enum class CullFlags : u32 {
  MeshletFrustum = 1 << 0,
  TriangleBackFace = 1 << 1,
  MicroTriangles = 1 << 2,
  OcclusionCulling = 1 << 3,
  TriangleCulling = 1 << 4,

  All = MeshletFrustum | TriangleBackFace | MicroTriangles | OcclusionCulling | TriangleCulling,
};
consteval void enable_bitmask(CullFlags);

enum class MaterialFlag : u32 {
  None = 0,
  // Image flags
  HasAlbedoImage = 1 << 0,
  HasNormalImage = 1 << 1,
  HasEmissiveImage = 1 << 2,
  HasMetallicRoughnessImage = 1 << 3,
  HasOcclusionImage = 1 << 4,
  // Normal flags
  NormalTwoComponent = 1 << 5,
  NormalFlipY = 1 << 6,
  // Alpha
  AlphaOpaque = 1 << 7,
  AlphaMask = 1 << 8,
  AlphaBlend = 1 << 9,
};
consteval void enable_bitmask(MaterialFlag);

struct Material {
  alignas(4) glm::vec4 albedo_color = {1.0f, 1.0f, 1.0f, 1.0f};
  alignas(4) glm::vec3 emissive_color = {0.0f, 0.0f, 0.0f};
  alignas(4) f32 roughness_factor = 0.0f;
  alignas(4) f32 metallic_factor = 0.0f;
  alignas(4) f32 alpha_cutoff = 0.0f;
  alignas(4) MaterialFlag flags = MaterialFlag::None;
  alignas(4) u32 sampler_index = 0;
  alignas(4) u32 albedo_image_index = 0;
  alignas(4) u32 normal_image_index = 0;
  alignas(4) u32 emissive_image_index = 0;
  alignas(4) u32 metallic_roughness_image_index = 0;
  alignas(4) u32 occlusion_image_index = 0;
  alignas(4) glm::vec2 uv_size = {};
  alignas(4) glm::vec2 uv_offfset = {};
};

struct Bounds {
  alignas(4) glm::vec3 aabb_center = {};
  alignas(4) glm::vec3 aabb_extent = {};
  alignas(4) glm::vec3 sphere_center = {};
  alignas(4) f32 sphere_radius = 0.0f;
};

struct MeshletInstance {
  alignas(4) u32 mesh_instance_index = 0;
  alignas(4) u32 meshlet_index = 0;
};

struct MeshInstance {
  alignas(4) u32 mesh_index = 0;
  alignas(4) u32 lod_index = 0;
  alignas(4) u32 material_index = 0;
  alignas(4) u32 transform_index = 0;
};

struct Meshlet {
  alignas(4) u32 indirect_vertex_index_offset = 0;
  alignas(4) u32 local_triangle_index_offset = 0;
  alignas(4) u32 vertex_count = 0;
  alignas(4) u32 triangle_count = 0;
};

struct MeshLOD {
  alignas(8) u64 indices = 0;
  alignas(8) u64 meshlets = 0;
  alignas(8) u64 meshlet_bounds = 0;
  alignas(8) u64 local_triangle_indices = 0;
  alignas(8) u64 indirect_vertex_indices = 0;

  alignas(4) u32 indices_count = 0;
  alignas(4) u32 meshlet_count = 0;
  alignas(4) u32 meshlet_bounds_count = 0;
  alignas(4) u32 local_triangle_indices_count = 0;
  alignas(4) u32 indirect_vertex_indices_count = 0;

  alignas(4) f32 error = 0.0f;
};

struct Mesh {
  constexpr static auto MAX_LODS = 8_sz;

  alignas(8) u64 vertex_positions = 0;
  alignas(8) u64 vertex_normals = 0;
  alignas(8) u64 texture_coords = 0;
  alignas(4) u32 vertex_count = 0;
  alignas(4) u32 lod_count = 0;
  alignas(8) MeshLOD lods[MAX_LODS] = {};
  alignas(4) Bounds bounds = {};
};

struct Sun {
  alignas(4) glm::vec3 direction = {};
  alignas(4) f32 intensity = 10.0f;
};

constexpr static f32 CAMERA_SCALE_UNIT = 0.01f;
constexpr static f32 INV_CAMERA_SCALE_UNIT = 1.0f / CAMERA_SCALE_UNIT;
constexpr static f32 PLANET_RADIUS_OFFSET = 0.001f;

struct Atmosphere {
  alignas(4) glm::vec3 eye_position = {}; // this is camera pos but its always above planet_radius

  alignas(4) glm::vec3 rayleigh_scatter = {0.005802f, 0.013558f, 0.033100f};
  alignas(4) f32 rayleigh_density = 8.0f;

  alignas(4) glm::vec3 mie_scatter = {0.003996f, 0.003996f, 0.003996f};
  alignas(4) f32 mie_density = 1.2f;
  alignas(4) f32 mie_extinction = 0.004440f;
  alignas(4) f32 mie_asymmetry = 3.6f;

  alignas(4) glm::vec3 ozone_absorption = {0.000650f, 0.001881f, 0.000085f};
  alignas(4) f32 ozone_height = 25.0f;
  alignas(4) f32 ozone_thickness = 15.0f;

  alignas(4) glm::vec3 terrain_albedo = {0.3f, 0.3f, 0.3f};
  alignas(4) f32 planet_radius = 6360.0f;
  alignas(4) f32 atmos_radius = 6460.0f;
  alignas(4) f32 aerial_perspective_start_km = 8.0f;

  alignas(4) vuk::Extent3D transmittance_lut_size = {};
  alignas(4) vuk::Extent3D sky_view_lut_size = {};
  alignas(4) vuk::Extent3D multiscattering_lut_size = {};
  alignas(4) vuk::Extent3D aerial_perspective_lut_size = {};
};

struct CameraData {
  alignas(4) glm::vec4 position = {};

  alignas(4) glm::mat4 projection = {};
  alignas(4) glm::mat4 inv_projection = {};
  alignas(4) glm::mat4 view = {};
  alignas(4) glm::mat4 inv_view = {};
  alignas(4) glm::mat4 projection_view = {};
  alignas(4) glm::mat4 inv_projection_view = {};

  alignas(4) glm::mat4 previous_projection = {};
  alignas(4) glm::mat4 previous_inv_projection = {};
  alignas(4) glm::mat4 previous_view = {};
  alignas(4) glm::mat4 previous_inv_view = {};
  alignas(4) glm::mat4 previous_projection_view = {};
  alignas(4) glm::mat4 previous_inv_projection_view = {};

  alignas(4) glm::vec2 temporalaa_jitter = {};
  alignas(4) glm::vec2 temporalaa_jitter_prev = {};

  alignas(4) glm::vec4 frustum_planes[6] = {};

  alignas(4) glm::vec3 up = {};
  alignas(4) f32 near_clip = 0;
  alignas(4) glm::vec3 forward = {};
  alignas(4) f32 far_clip = 0;
  alignas(4) glm::vec3 right = {};
  alignas(4) f32 fov = 0;
  alignas(4) u32 output_index = 0;
  alignas(4) glm::vec2 resolution = {};
};

struct PointLight {
  glm::vec3 position;
  glm::vec3 color;
  f32 intensity;
  f32 cutoff;
};

struct SpotLight {
  alignas(4) glm::vec3 position;
  alignas(4) glm::vec3 direction;
  alignas(4) glm::vec3 color;
  alignas(4) f32 intensity;
  alignas(4) f32 cutoff;
  alignas(4) f32 inner_cone_angle;
  alignas(4) f32 outer_cone_angle;
};

enum class SceneFlags : u32 {
  None = 0,
  HasSun = 1 << 0,
  HasAtmosphere = 1 << 1,
  HasEyeAdaptation = 1 << 2,
};
consteval void enable_bitmask(SceneFlags);

struct LightSettings {
  alignas(4) u32 point_light_count = 0;
  alignas(4) u32 spot_light_count = 0;
};

struct Scene {
  alignas(4) SceneFlags scene_flags;
  alignas(4) LightSettings light_settings;

  alignas(4) Atmosphere atmosphere;
  alignas(4) Sun sun;
  alignas(8) u64 point_lights;
  alignas(8) u64 spot_lights;
};

constexpr static u32 HISTOGRAM_THREADS_X = 16;
constexpr static u32 HISTOGRAM_THREADS_Y = 16;
constexpr static u32 HISTOGRAM_BIN_COUNT = HISTOGRAM_THREADS_X * HISTOGRAM_THREADS_Y;

struct HistogramLuminance {
  alignas(4) f32 adapted_luminance;
  alignas(4) f32 exposure;
};

struct HistogramInfo {
  alignas(4) f32 min_exposure = -6.0f;
  alignas(4) f32 max_exposure = 18.0f;
  alignas(4) f32 adaptation_speed = 1.1f;
  alignas(4) f32 ev100_bias = 1.0f;
};

struct VBGTAOSettings {
  alignas(4) f32 thickness = 0.25;
  alignas(4) u32 slice_count = 3;
  alignas(4) u32 samples_per_slice_side = 3;
  alignas(4) f32 effect_radius = 0.5;
  alignas(4) u32 noise_index = 0;
};
} // namespace ox::GPU
