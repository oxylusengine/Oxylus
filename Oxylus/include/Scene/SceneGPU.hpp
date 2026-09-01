#pragma once

#include <array>
#include <glm/gtc/packing.hpp>
#include <glm/mat4x4.hpp>
#include <vuk/Types.hpp>
#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/Types.hpp"
#include "Utils/OxMath.hpp"

namespace ox::GPU {
enum class TransformID : u64 { Invalid = ~0_u64 };
enum class LightID : u64 { Invalid = ~0_u64 };

struct Transforms {
  alignas(4) glm::mat4 world = {};
  alignas(4) glm::mat4 previous_world = {};
};

struct TransformWorld {
  alignas(4) glm::mat4 world = {};
};

struct TransformPrevious {
  alignas(4) glm::mat4 previous_world = {};
};

enum class DebugView : i32 {
  None = 0,
  Triangles,
  Meshlets,
  Overdraw,
  Materials,
  MeshInstances,
  MeshLods,
  Albedo,
  Normal,
  Emissive,
  Metallic,
  Roughness,
  BakedOcclusion,
  GTAO,
  GeometricNormal,
  RMVSM,
  RMVSMPointSpot,
  DDGIProbes,

  Count,
};

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
  alignas(2) glm::u16vec4 albedo_color = {};
  alignas(2) glm::u16vec3 emissive_color = {};
  alignas(2) u16 roughness_factor = 0;
  alignas(2) u16 metallic_factor = 0;
  alignas(2) u16 alpha_cutoff = 0;
  alignas(4) MaterialFlag flags = MaterialFlag::None;
  alignas(4) u32 sampler_index = 0;
  alignas(4) u32 albedo_image_index = 0;
  alignas(4) u32 normal_image_index = 0;
  alignas(4) u32 emissive_image_index = 0;
  alignas(4) u32 metallic_roughness_image_index = 0;
  alignas(4) u32 occlusion_image_index = 0;
  alignas(2) glm::u16vec2 uv_size = {};
  alignas(2) glm::u16vec2 uv_offset = {};
};

struct MeshletBounds {
  alignas(2) glm::u16vec3 aabb_center = {};
  alignas(1) glm::i8vec2 cone_axis_xy = {};
  alignas(2) glm::u16vec3 aabb_extent = {};
  alignas(1) i8 cone_axis_z = {};
  alignas(1) i8 cone_cutoff = {};
};

struct MeshBounds {
  alignas(4) glm::vec3 aabb_center = {};
  alignas(4) glm::vec3 aabb_extent = {};
};

struct MeshletInstanceVisibility {
  // This is incremented __ONLY__ during cull MESHES pass.
  alignas(4) u32 total_visible_meshlet_instances = 0;
  // Number of meshlets that were visible on first cull MESHLETS pass.
  alignas(4) u32 early_visible_meshlet_instances = 0;
  // Same as above, but if requested (used for two pass occlusion tests)
  alignas(4) u32 late_visible_meshlet_instances = 0;
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
  alignas(4) u32 meshlet_instance_visibility_offset = 0;
};

struct Meshlet {
  alignas(4) u32 indirect_vertex_index_offset = 0;
  alignas(4) u32 local_triangle_index_offset = 0;
  alignas(4) u32 vertex_count = 0;
  alignas(4) u32 triangle_count = 0;
};

struct AccelerationStructureInstance {
  alignas(4) f32 transform[12] = {};
  alignas(4) u32 custom_index_and_mask = 0;
  alignas(4) u32 sbt_offset_and_flags = 0;
  alignas(8) u64 blas_address = 0;
};
static_assert(sizeof(AccelerationStructureInstance) == 64);

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
  alignas(8) u64 lods = 0;
  alignas(4) MeshBounds bounds = {};
};

constexpr static f32 CAMERA_SCALE_UNIT = 0.01f;
constexpr static f32 INV_CAMERA_SCALE_UNIT = 1.0f / CAMERA_SCALE_UNIT;
constexpr static f32 PLANET_RADIUS_OFFSET = 0.001f;

struct Atmosphere {
  alignas(4) glm::vec3 rayleigh_scatter = {0.005802f, 0.013558f, 0.033100f};
  alignas(4) f32 rayleigh_density = 8.0f;

  alignas(4) glm::vec3 mie_scatter = {0.003996f, 0.003996f, 0.003996f};
  alignas(4) f32 mie_density = 1.2f;
  alignas(4) f32 mie_extinction = 0.004440f;
  alignas(4) f32 mie_asymmetry = 3.6f;
  alignas(4) f32 mie_haze_amount = 0.7f;
  alignas(4) f32 mie_haze_scale_height = 11.0f;

  alignas(4) glm::vec3 ozone_absorption = {0.000650f, 0.001881f, 0.000085f};
  alignas(4) f32 ozone_height = 25.0f;
  alignas(4) f32 ozone_thickness = 15.0f;

  alignas(4) glm::vec3 terrain_albedo = {0.3f, 0.3f, 0.3f};
  alignas(4) f32 planet_radius = 6360.0f;
  alignas(4) f32 atmos_radius = 6460.0f;
  alignas(4) f32 aerial_perspective_start_km = 8.0f;
  alignas(4) f32 aerial_perspective_exposure = 1.0f;

  alignas(4) vuk::Extent3D transmittance_lut_size = {};
  alignas(4) vuk::Extent3D sky_view_lut_size = {};
  alignas(4) vuk::Extent3D multiscattering_lut_size = {};
  alignas(4) vuk::Extent3D aerial_perspective_lut_size = {};
};

static_assert(sizeof(Atmosphere) == 144, "Atmosphere layout drifted from scene.slang");

struct SkyData {
  alignas(4) glm::vec4 solid_color = {0.f, 0.f, 0.f, 1.f};
  alignas(4) glm::vec3 ambient_color = {0.03f, 0.03f, 0.03f};
  alignas(4) u32 has_texture = false;
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
  alignas(4) f32 acceptable_lod_error = 2.0f; // TODO: Make this configurable
};

struct CullCamera {
  glm::mat4 projection_view = {};
  glm::vec3 position = {};
  f32 acceptable_lod_error = {};
  glm::vec2 resolution = {};
  f32 near_clip = {};
  u32 mesh_instance_count = {};
  // only the main geometry pass sets this; culling and shadow users leave it zero
  glm::vec2 jitter = {};
};

constexpr static u32 MAX_POINT_LIGHTS = 128;
constexpr static u32 MAX_SPOT_LIGHTS = 128;
constexpr static u32 MAX_LIGHTS = MAX_POINT_LIGHTS + MAX_SPOT_LIGHTS;

constexpr static u32 MAX_SHADOW_POINT_LIGHTS = 64;
constexpr static u32 MAX_SHADOW_SPOT_LIGHTS = 64;

// per-cell shadow mask, point 0-63, then spot 0-63, packed into uvec4
constexpr static glm::ivec3 LIGHT_GRID_RESOLUTION = {64, 32, 64};
constexpr static f32 LIGHT_GRID_CELL_SIZE = 8.0f;
constexpr static u32 LIGHT_GRID_CELL_COUNT = 64u * 32u * 64u;

struct DirectionalLight {
  alignas(4) glm::vec3 color = {0.02, 0.02, 0.02};
  alignas(4) f32 intensity = 10.0f;
  alignas(4) glm::vec3 direction = {};
};

enum class LightKind : u32 { Directional = 0, Point = 1, Spot = 2 };

struct Light {
  alignas(4) glm::vec3 position = {};
  alignas(4) f32 intensity = 1.0f;
  alignas(4) glm::vec3 color = {1.0f, 1.0f, 1.0f};
  alignas(4) f32 range = 0.0f;
  alignas(4) glm::vec3 direction = {};
  alignas(4) f32 inner_cone_angle = 0.0f; // spot only (radians)
  alignas(4) f32 outer_cone_angle = 0.0f; // spot only (radians)
  alignas(4) LightKind kind = LightKind::Point;
  // -1 disables shadows
  alignas(4) i32 shadow_map_index = -1;
  alignas(4) u32 pad = {};
};

constexpr static u32 DDGI_MAX_IMAGE_DIMENSION = 16384;

constexpr static u32 DDGI_MAX_PROBE_COUNT = 1u << 18;
constexpr static u32 DDGI_MAX_CASCADE_COUNT = 8;
// Octahedral tiles, each padded with a one texel border that mirrors the opposite edge so bilinear
// taps stay continuous across the octahedron seam.
constexpr static u32 DDGI_IRRADIANCE_TEXELS = 12;
constexpr static u32 DDGI_RADIANCE_TEXELS = 6;
constexpr static u32 DDGI_DISTANCE_TEXELS = 12;
constexpr static u32 DDGI_TRACE_TEXELS = 12;
constexpr static u32 DDGI_RAYS_PER_PROBE = DDGI_TRACE_TEXELS * DDGI_TRACE_TEXELS;
constexpr static u32 DDGI_PROBES_PER_ATLAS_ROW = 512;

constexpr auto ddgi_atlas_extent(u32 probe_count, u32 interior_texels) -> vuk::Extent3D {
  const auto tile = interior_texels + 2;
  const auto rows = (probe_count + DDGI_PROBES_PER_ATLAS_ROW - 1) / DDGI_PROBES_PER_ATLAS_ROW;
  return {.width = DDGI_PROBES_PER_ATLAS_ROW * tile, .height = rows * tile, .depth = 1};
}

constexpr static u32 DDGI_IRRADIANCE_ATLAS_WIDTH = DDGI_PROBES_PER_ATLAS_ROW * (DDGI_IRRADIANCE_TEXELS + 2);
static_assert(DDGI_IRRADIANCE_ATLAS_WIDTH % (DDGI_RADIANCE_TEXELS + 2) == 0);
constexpr static u32 DDGI_RADIANCE_PROBES_PER_ATLAS_ROW = DDGI_IRRADIANCE_ATLAS_WIDTH / (DDGI_RADIANCE_TEXELS + 2);

constexpr auto ddgi_radiance_atlas_y_offset(u32 probe_count) -> u32 {
  return ddgi_atlas_extent(probe_count, DDGI_IRRADIANCE_TEXELS).height;
}

constexpr auto ddgi_irradiance_atlas_extent(u32 probe_count) -> vuk::Extent3D {
  const auto irradiance = ddgi_atlas_extent(probe_count, DDGI_IRRADIANCE_TEXELS);
  const auto radiance_rows = (probe_count + DDGI_RADIANCE_PROBES_PER_ATLAS_ROW - 1) /
                             DDGI_RADIANCE_PROBES_PER_ATLAS_ROW;
  return {
    .width = irradiance.width,
    .height = irradiance.height + radiance_rows * (DDGI_RADIANCE_TEXELS + 2),
    .depth = 1,
  };
}

constexpr static auto DDGI_MAX_IRRADIANCE_ATLAS_EXTENT = ddgi_irradiance_atlas_extent(DDGI_MAX_PROBE_COUNT);
static_assert(DDGI_MAX_IRRADIANCE_ATLAS_EXTENT.width <= DDGI_MAX_IMAGE_DIMENSION);
static_assert(DDGI_MAX_IRRADIANCE_ATLAS_EXTENT.height <= DDGI_MAX_IMAGE_DIMENSION);

constexpr auto ddgi_probes_per_ray_row(u32 rays_per_probe) -> u32 {
  auto probes = 1_u32;
  while (probes * 2 * rays_per_probe <= DDGI_MAX_IMAGE_DIMENSION) {
    probes *= 2;
  }
  return probes;
}

constexpr auto ddgi_ray_data_extent(u32 probe_count, u32 rays_per_probe) -> vuk::Extent3D {
  const auto probes_per_row = ddgi_probes_per_ray_row(rays_per_probe);
  const auto rows = (probe_count + probes_per_row - 1) / probes_per_row;
  return {.width = probes_per_row * rays_per_probe, .height = rows, .depth = 1};
}

constexpr static u32 DDGI_PROBE_SELECT_GROUP = 64;
constexpr static u32 DDGI_TEXEL_UPDATE_GROUP = 8;
constexpr static u32 DDGI_TEXEL_UPDATE_THREADS_Y = 48;
constexpr static u32 DDGI_TEXEL_UPDATE_GROUPS_Y = DDGI_TEXEL_UPDATE_THREADS_Y / DDGI_TEXEL_UPDATE_GROUP;

static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_IRRADIANCE_TEXELS == 0);
static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_DISTANCE_TEXELS == 0);
static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_TEXEL_UPDATE_GROUP == 0);

struct ProbeUpdateArgs {
  u32 count = 0;
  vuk::DispatchIndirectCommand irradiance = {};
  vuk::DispatchIndirectCommand distance = {};
  vuk::DispatchIndirectCommand relocate = {};
};

constexpr static u32 DDGI_DEBUG_SPHERE_RINGS = 8;
constexpr static u32 DDGI_DEBUG_SPHERE_SECTORS = 12;
constexpr static u32 DDGI_DEBUG_SPHERE_VERTEX_COUNT = DDGI_DEBUG_SPHERE_RINGS * DDGI_DEBUG_SPHERE_SECTORS * 6;

struct ProbeState {
  alignas(4) glm::vec3 offset = {};
  alignas(4) u32 flags = 0;
};

struct ProbeVolume {
  alignas(4) glm::vec3 origin = {};
  alignas(4) u32 probe_offset = 0;
  alignas(4) glm::vec3 spacing = {};
  alignas(4) u32 probe_count = 0;
  alignas(4) glm::vec3 spacing_rcp = {};
  alignas(4) u32 cascade_index = 0;
  alignas(4) glm::uvec3 counts = {};
  alignas(4) u32 cascade_count = 0;
  alignas(4) glm::ivec3 scroll = {};
  alignas(4) f32 cascade_blend = 0.3f;
  alignas(4) glm::vec3 center = {};
  alignas(4) f32 max_probe_distance = 0.0f;
};

enum class SceneFlags : u32 {
  None = 0,
  HasDirectionalLight = 1 << 0,
  HasAtmosphere = 1 << 1,
  HasEyeAdaptation = 1 << 2,
  HasBloom = 1 << 3,
  HasFXAA = 1 << 4,
  HasGTAO = 1 << 5,
  HasFilmGrain = 1 << 6,
  HasChromaticAberration = 1 << 7,
  HasVignette = 1 << 8,
  HasContactShadows = 1 << 9,
  HasSky = 1 << 10,
  TransparentBackground = 1 << 11,
  HasDDGI = 1 << 12,
  HasParticles = 1 << 13,
  HasParticleSorting = 1 << 14,
};
consteval void enable_bitmask(SceneFlags);

constexpr static u32 HISTOGRAM_THREADS_X = 16;
constexpr static u32 HISTOGRAM_THREADS_Y = 16;
constexpr static u32 HISTOGRAM_BIN_COUNT = HISTOGRAM_THREADS_X * HISTOGRAM_THREADS_Y;

struct HistogramLuminance {
  alignas(4) f32 adapted_luminance;
  alignas(4) f32 exposure;
};

struct EyeAdaptationSettings {
  alignas(4) f32 min_exposure = -6.0f;
  alignas(4) f32 max_exposure = 18.0f;
  alignas(4) f32 adaptation_speed = 1.1f;
  alignas(4) f32 ev100_bias = 1.0f;
};

struct VBGTAOSettings {
  alignas(4) f32 thickness = 0.25f;
  alignas(4) u32 slice_count = 3;
  alignas(4) u32 samples_per_slice_side = 3;
  alignas(4) f32 effect_radius = 0.5f;
  alignas(4) u32 noise_index = 0;
  alignas(4) f32 final_power = 2.2f;
};

struct PostProcessSettings {
  alignas(4) f32 exposure = 1.0f;
  alignas(4) f32 chromatic_aberration_amount = 0.5f;
  alignas(4) f32 vignette_amount = 0.5f;
  alignas(4) f32 film_grain_scale = 1.0f;
  alignas(4) f32 film_grain_amount = 0.5f;
  alignas(4) u32 film_grain_seed = 0;
};

// mirrors FSR3Constants in Render/Shaders/fsr3/constants.slang, same field order as the SDK's
// cbFSR3Upscaler so the port stays comparable against the reference
struct FSR3Constants {
  alignas(4) glm::ivec2 render_size = {};
  alignas(4) glm::ivec2 previous_frame_render_size = {};

  alignas(4) glm::ivec2 upscale_size = {};
  alignas(4) glm::ivec2 previous_frame_upscale_size = {};

  alignas(4) glm::ivec2 max_render_size = {};
  alignas(4) glm::ivec2 max_upscale_size = {};

  alignas(4) glm::vec4 device_to_view_depth = {};

  alignas(4) glm::vec2 jitter_offset = {};
  alignas(4) glm::vec2 previous_frame_jitter_offset = {};

  alignas(4) glm::vec2 motion_vector_scale = {};
  alignas(4) glm::vec2 downscale_factor = {};

  alignas(4) glm::vec2 motion_vector_jitter_cancellation = {};
  alignas(4) f32 tan_half_fov = 0.0f;
  alignas(4) f32 jitter_phase_count = 1.0f;

  alignas(4) f32 delta_time = 0.0f;
  alignas(4) f32 delta_pre_exposure = 1.0f;
  alignas(4) f32 view_space_to_meters_factor = 1.0f;
  alignas(4) f32 frame_index = 0.0f;

  alignas(4) f32 velocity_factor = 1.0f;
  alignas(4) f32 reactiveness_scale = 1.0f;
  alignas(4) f32 shading_change_scale = 1.0f;
  alignas(4) f32 accumulation_added_per_frame = 1.0f / 3.0f;
  alignas(4) f32 min_disocclusion_accumulation = -1.0f / 3.0f;
};

enum struct TonemapType : u32 {
  None = 0,
  ACES,
  AgX,
  GT7,
};

struct VSMAllocRequest {
  alignas(4) glm::ivec3 page_table_address = {};
  // -1 for directional pages
  alignas(4) i32 mip = -1;
};

struct VSMPageAllocator {
  alignas(4) u32 active_request_count = {};
  alignas(4) u32 dirty_physical_page_count = {};
  alignas(4) u32 free_page_count = {};
  alignas(4) u32 alloc_cursor = {};
  alignas(4) u32 request_capacity = {};
  alignas(4) u32 pad = {};
  alignas(8) u64 requests = {};
  alignas(8) u64 dirty_physical_page_coords = {};
  alignas(8) u64 free_page_list = {};
};

// point layer = light * 6 + face, spot layers follow all point layers
struct VSMPointSpotView {
  alignas(4) glm::mat4 projection_view = {};
  alignas(4) glm::vec3 light_position = {};
  alignas(4) f32 range = 0.0f;             // 0 means the layer is inactive
  alignas(4) u32 light_index = 0;
  alignas(4) f32 z_near = 0.0f;
  alignas(4) f32 texel_world_scale = 1.0f; // tan(fov/2), for world-space texel sizing in shading
  alignas(4) u32 pad = {};
};

struct VSMMeshletInstance {
  alignas(4) u32 mesh_instance_index = 0;
  alignas(4) u32 meshlet_index = 0;
  alignas(4) u32 layer = 0;
};

struct VSMPointSpotContext {
  i32 curr_mip = 0;
  u32 layer_count = 0;
  u32 mesh_instance_count = 0;
  glm::ivec2 depth_extent = {};
  u32 mip_bias_min = 0;
  u32 shadow_point_light_count = 0;
  u32 shadow_spot_light_count = 0;
};

struct VSMContext {
  i32 page_size = 0;
  i32 page_table_size = 0;
  i32 physcial_page_table_size = 0;
  i32 curr_clipmap_index = 0;
  i32 clipmap_count = 0;
  glm::ivec2 depth_extent = {};
  f32 first_clipmap_width = 0;
  f32 clipmap_selection_bias = 0;
  f32 virtual_extent = 0;
  f32 z_length = 0.0f;
  glm::vec3 directional_light_dir = {};
};

struct VirtualClipmap {
  glm::mat4 projection_view_mat = {};
  glm::ivec2 page_offset = {};
  f32 z_near = 0.0f;
};

enum struct CullFlag : u32 {
  None = 0,
  TestFrustum = 1 << 0,
  SelectLOD = 1 << 1,
  TestOcclusion = 1 << 2,
  LatePass = 1 << 3,

  TestAll = TestFrustum | SelectLOD | TestOcclusion,
};
consteval void enable_bitmask(CullFlag);

constexpr static u32 TERRAIN_MAX_LAYERS = 4;

struct TerrainErosion {
  f32 scale = 0.15f;
  f32 strength = 0.22f;
  f32 gully_weight = 0.5f;
  f32 detail = 1.5f;
  glm::vec4 rounding = {0.1f, 0.0f, 0.1f, 2.0f};
  glm::vec4 onset = {1.25f, 1.25f, 2.8f, 1.5f};
  glm::vec2 assumed_slope = {0.7f, 1.0f};
  f32 cell_scale = 0.7f;
  f32 gain = 0.5f;
  f32 lacunarity = 2.0f;
  f32 normalization = 0.5f;
  u32 octaves = 5;
  u32 seed = 0;
};

struct TerrainGenerate {
  TerrainErosion erosion = {};
  glm::uvec2 resolution = {};
  glm::vec2 height_offset = {-0.65f, 0.0f};
  f32 domain_size = 2.0f;
  f32 height_frequency = 3.0f;
  f32 height_amplitude = 0.125f;
  f32 height_lacunarity = 2.0f;
  f32 height_gain = 0.1f;
  u32 height_octaves = 3;
};

struct TerrainDerive {
  glm::uvec2 resolution = {};
  glm::vec2 texel_world_size = {};
  f32 height_range = 0.0f;
  f32 slope_rock_begin = 0.55f;
  f32 slope_rock_end = 0.8f;
  f32 altitude_snow_begin = 0.7f;
  f32 altitude_snow_end = 0.85f;
  f32 ridge_drainage_scale = 1.0f;
};

struct TerrainMinMax {
  glm::uvec2 resolution = {};
  glm::uvec2 patch_count = {};
};

struct TerrainRegion {
  glm::uvec2 texel_origin = {};
  glm::uvec2 patch_origin = {};
};

enum struct TerrainBrushMode : u32 {
  Raise = 0,
  Smooth,
  Flatten,
  Noise,
  PaintLayer,
};

struct TerrainBrushHit {
  glm::vec3 world_position = {};
  u32 valid = 0;
};

struct TerrainBrushParams {
  glm::vec3 ray_origin = {};
  f32 radius_texels = 0.0f;
  glm::vec3 ray_direction = {};
  f32 strength = 0.0f;
  glm::uvec2 resolution = {};
  glm::vec2 world_min = {};
  glm::vec2 world_size = {};
  glm::vec2 inv_world_size = {};
  glm::uvec2 patch_count = {};
  f32 base_height = 0.0f;
  f32 height_scale = 0.0f;
  f32 falloff = 1.0f;
  f32 flatten_height = 0.0f;
  u32 mode = 0;
  u32 layer = 0;
};

constexpr u32 TERRAIN_INVALID_LAYER_MATERIAL = ~0_u32;

struct TerrainData {
  glm::vec2 world_min = {};
  glm::vec2 world_size = {};
  glm::vec2 inv_world_size = {};
  glm::uvec2 patch_count = {};
  f32 base_height = 0.0f;
  f32 height_scale = 0.0f;
  f32 target_edge_pixels = 16.0f;
  f32 max_tessellation = 64.0f;
  f32 layer_tiling = 8.0f;
  f32 triplanar_begin = 0.5f;
  glm::uvec4 layer_material_indices = glm::uvec4(TERRAIN_INVALID_LAYER_MATERIAL);
  f32 brush_radius = 0.0f;
};

enum RenderFlags2D : u32 {
  RENDER_FLAGS_2D_NONE = 0,

  RENDER_FLAGS_2D_SORT_Y = 1 << 0,
  RENDER_FLAGS_2D_FLIP_X = 1 << 1,
};

struct DrawBatch2D {
  vuk::Name pipeline_name = {};
  u32 offset = 0;
  u32 count = 0;
};

struct SpriteGPUData {
  alignas(4) u32 material_id16_ypos16 = 0;
  alignas(4) u32 flags16_distance16 = 0;
  alignas(4) u32 transform_id = 0;

  // A half's raw bits only compare correctly while it is positive: the sign bit makes every negative
  // value look larger than every positive one. Flip so negatives order below positives.
  static auto half_sort_key(u32 bits) -> u32 { return (bits & 0x8000u) ? (~bits & 0xFFFFu) : (bits | 0x8000u); }

  bool operator>(const SpriteGPUData& other) const {
    union SortKey {
      struct {
        // The order of members is important here, it means the sort priority (low to high)!
        u64 distance_y : 32;
        u64 distance_z : 32;
      } bits;

      u64 value;
    };
    static_assert(sizeof(SortKey) == sizeof(u64));
    const SortKey a = {
      .bits = {
        .distance_y = math::unpack_u32_low(flags16_distance16) & RENDER_FLAGS_2D_SORT_Y
                        ? half_sort_key(math::unpack_u32_high(material_id16_ypos16))
                        : 0u,
        .distance_z = math::unpack_u32_high(flags16_distance16),
      },
    };
    const SortKey b = {
      .bits = {
        .distance_y = math::unpack_u32_low(other.flags16_distance16) & RENDER_FLAGS_2D_SORT_Y
                        ? half_sort_key(math::unpack_u32_high(other.material_id16_ypos16))
                        : 0u,
        .distance_z = math::unpack_u32_high(other.flags16_distance16),
      },
    };
    return a.value > b.value;
  }
};

// --- Particles ---

constexpr static u32 PARTICLE_REGISTER_COUNT = 16;
constexpr static u32 PARTICLE_SIMULATE_GROUP = 64;
constexpr static u32 PARTICLE_SORT_GROUP = 256;
constexpr static u32 PARTICLE_USER_PARAM_COUNT = 4;

enum class ParticleOperandKind : u32 {
  Register = 0,
  Constant = 1,
  Immediate = 2,
};

enum class ParticleOpCode : u8 {
  Nop = 0,
  Mov,
  Swizzle,
  Add,
  Sub,
  Mul,
  Div,
  Mad,
  Min,
  Max,
  Clamp,
  Lerp,
  Abs,
  Floor,
  Frac,
  Pow,
  Dot,
  Cross,
  Normalize,
  Length,
  Sin,
  Cos,
  Step,
  Smoothstep,
  Select,
  Curve,
  Gradient,
  Noise,
  Random,
  Time,
  AgeNorm,
  Param,
  // CPU-only, executed by the emitter program interpreter. The GPU never sees these.
  LoadState,
  StoreState,
  Count,
};

struct ParticleInstruction {
  alignas(4) u32 op_dst = 0;
  alignas(4) u32 src0 = 0;
  alignas(4) u32 src1 = 0;
  alignas(4) u32 src2 = 0;

  static auto encode_op(ParticleOpCode op, u32 dst_register, u32 write_mask) -> u32 {
    return static_cast<u32>(op) | (dst_register << 8u) | (write_mask << 12u);
  }

  static auto encode_operand(ParticleOperandKind kind, u32 payload) -> u32 {
    return (static_cast<u32>(kind) << 30u) | (payload & 0x3FFFFFFFu);
  }
};

static_assert(sizeof(ParticleInstruction) == 16, "ParticleInstruction layout drifted from particles.slang");

constexpr static u32 PARTICLE_REG_POSITION_LIFE = 0;  // xyz position, w life_remaining
constexpr static u32 PARTICLE_REG_VELOCITY_TOTAL = 1; // xyz velocity, w life_total
constexpr static u32 PARTICLE_REG_SIZE_ROT_SEED = 2;  // xy size, z rotation, w seed
constexpr static u32 PARTICLE_REG_COLOR = 3;          // rgba
constexpr static u32 PARTICLE_REG_CONTEXT = 4;        // x age_norm, y emitter time, z delta time, w flipbook frame
constexpr static u32 PARTICLE_ATTRIBUTE_REGISTERS = 5;

// The emitter program is a third bytecode program run once per emitter per frame, on the CPU, over
// the same instruction encoding. Its registers mean something else entirely.
constexpr static u32 PARTICLE_EMITTER_REG_OUTPUT = 0;  // x spawn count, y spawn rate
constexpr static u32 PARTICLE_EMITTER_REG_CONTEXT = 1; // x time, y delta time, z cycle norm, w queued pulses
constexpr static u32 PARTICLE_EMITTER_ATTRIBUTE_REGISTERS = 2;
constexpr static u32 PARTICLE_EMITTER_STATE_COUNT = 8;

struct Particle {
  alignas(4) glm::vec3 position = {};
  alignas(4) f32 life_remaining = 0.0f;
  alignas(4) glm::vec3 velocity = {};
  alignas(4) f32 life_total = 0.0f;
  alignas(4) glm::vec2 size = {};
  alignas(4) f32 rotation = 0.0f;
  alignas(4) f32 seed = 0.0f;
  alignas(4) u32 color = 0;
  alignas(4) u32 flipbook_frame = 0;
  alignas(4) u32 emitter_index = 0;
  alignas(4) u32 flags = 0;
};

static_assert(sizeof(Particle) == 64, "Particle layout drifted from particles.slang");

enum class ParticleEmitterFlags : u32 {
  None = 0,
  LocalSpace = 1 << 0,
  Additive = 1 << 1,
  VelocityStretched = 1 << 2,
  SoftParticles = 1 << 3,
  DepthCollision = 1 << 4,
  MeshRenderer = 1 << 5,
  HorizontalPlane = 1 << 6,
  VerticalPlane = 1 << 7,
};
consteval void enable_bitmask(ParticleEmitterFlags);

struct ParticleEmitter {
  alignas(4) glm::mat4 transform = glm::mat4(1.0f);
  alignas(4) glm::vec4 rotation = {0.0f, 0.0f, 0.0f, 1.0f};

  alignas(4) u32 pool_offset = 0;
  alignas(4) u32 capacity = 0;
  alignas(4) u32 spawn_count = 0;
  alignas(4) u32 spawn_offset = 0;

  alignas(4) u32 spawn_program_offset = 0;
  alignas(4) u32 spawn_program_count = 0;
  alignas(4) u32 update_program_offset = 0;
  alignas(4) u32 update_program_count = 0;

  alignas(4) u32 constants_offset = 0;
  alignas(4) u32 curve_atlas_index = ~0_u32;
  alignas(4) u32 curve_sampler_index = 0;
  alignas(4) u32 curve_row_count = 0;

  alignas(4) u32 atlas_row_count = 0;
  alignas(4) u32 material_index = 0;
  alignas(4) u32 flipbook_x = 1;
  alignas(4) u32 flipbook_y = 1;

  alignas(4) ParticleEmitterFlags flags = ParticleEmitterFlags::None;
  alignas(4) u32 shape = 0;
  alignas(4) u32 seed = 0;
  alignas(4) f32 time = 0.0f;

  alignas(4) f32 delta_time = 0.0f;
  alignas(4) f32 restitution = 0.4f;
  alignas(4) f32 soft_particle_distance = 0.0f;
  alignas(4) f32 velocity_stretch = 1.0f;

  alignas(4) glm::vec2 lifetime = {1.0f, 1.0f};
  alignas(4) glm::vec4 shape_params = {};
  alignas(4) glm::vec4 velocity_offset = {};
  alignas(4) std::array<glm::vec4, PARTICLE_USER_PARAM_COUNT> user_params = {};
};

static_assert(sizeof(ParticleEmitter) == 280, "ParticleEmitter layout drifted from particles.slang");

struct ParticleSortKey {
  alignas(4) u32 key = ~0_u32;
  alignas(4) u32 index = ~0_u32;
};

struct ParticleCounters {
  alignas(4) u32 alive_count = 0;
  alignas(4) u32 alive_count_next = 0;
  alignas(4) u32 draw_count = 0;
  alignas(4) u32 pad = 0;
};

} // namespace ox::GPU
