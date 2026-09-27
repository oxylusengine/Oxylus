#pragma once

// every type here is read by both C++ and Slang (through shared.slang), and GPULayoutCheck asserts
// that each field lands at the offset slang computes. rules: no bool, no field that exists on only
// one side, and defaults both languages parse the same way

#include "Prelude.hpp"

OX_GPU_BEGIN

// --- Commands ---

struct DispatchIndirectCommand {
  u32 x = 0;
  u32 y = 0;
  u32 z = 0;
};

struct DrawIndexedIndirectCommand {
  u32 index_count = 0;
  u32 instance_count = 0;
  u32 first_index = 0;
  i32 vertex_offset = 0;
  u32 first_instance = 0;
};

struct DrawIndirectCommand {
  u32 vertex_count = 0;
  u32 instance_count = 0;
  u32 first_vertex = 0;
  u32 first_instance = 0;
};

struct AccelerationStructureInstance {
  f32 transform[12] = {};
  u32 custom_index_and_mask = 0;
  u32 sbt_offset_and_flags = 0;
  u64 blas_address = 0;
};

// --- Mesh ---

struct MeshletBounds {
  u16x3 aabb_center = {};
  i8x2 cone_axis_xy = {};
  u16x3 aabb_extent = {};
  i8 cone_axis_z = {};
  i8 cone_cutoff = {};
};

struct MeshBounds {
  f32x3 aabb_center = {};
  f32x3 aabb_extent = {};
};

struct Meshlet {
  u32 indirect_vertex_index_offset = 0;
  u32 local_triangle_index_offset = 0;
  u32 vertex_count = 0;
  u32 triangle_count = 0;
};

// Every pointer here is a device address at runtime, but a blob-relative offset on disk. Anything that
// changes the layout of this struct, Mesh, Meshlet or MeshletBounds invalidates every compiled
// model, so bump AssetFileHeader::VERSION with it.
struct MeshLOD {
  OX_PTR(u32) indices = {};
  OX_PTR(Meshlet) meshlets = {};
  OX_PTR(MeshletBounds) meshlet_bounds = {};
  // u8 triangle corners on the CPU, read four at a time on the GPU
  OX_PTR(u32) local_triangle_indices = {};
  OX_PTR(u32) indirect_vertex_indices = {};

  u32 indices_count = 0;
  u32 meshlet_count = 0;
  u32 meshlet_bounds_count = 0;
  u32 local_triangle_indices_count = 0;
  u32 indirect_vertex_indices_count = 0;

  f32 error = 0.0f;
};

struct Mesh {
  OX_CONST u32 MAX_LODS = 8;
  OX_CONST u32 MAX_MESHLET_INDICES = 64;
  OX_CONST u32 MAX_MESHLET_PRIMITIVES = 64;

  OX_PTR(u16x4) vertex_positions = {};
  OX_PTR(u32) vertex_normals = {};
  OX_PTR(u16x2) texture_coords = {};
  u32 vertex_count = 0;
  u32 lod_count = 0;
  OX_PTR(MeshLOD) lods = {};
  MeshBounds bounds = {};
};

// --- Scene ---

struct TransformWorld {
  mat4 world = {};
};

struct TransformPrevious {
  mat4 previous_world = {};
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
OX_BITMASK(MaterialFlag)

struct Material {
  u16x4 albedo_color = {};
  u16x3 emissive_color = {};
  u16 roughness_factor = 0;
  u16 metallic_factor = 0;
  u16 alpha_cutoff = 0;
  u16 normal_scale = 0;
  u16 occlusion_strength = 0;
  MaterialFlag flags = MaterialFlag::None;
  u32 sampler_index = 0;
  u32 albedo_image_index = 0;
  u32 normal_image_index = 0;
  u32 emissive_image_index = 0;
  u32 metallic_roughness_image_index = 0;
  u32 occlusion_image_index = 0;
  u16x2 uv_size = {};
  u16x2 uv_offset = {};
};

struct MeshletInstanceVisibility {
  // This is incremented __ONLY__ during cull MESHES pass.
  u32 total_visible_meshlet_instances = 0;
  // Number of meshlets that were visible on first cull MESHLETS pass.
  u32 early_visible_meshlet_instances = 0;
  // Same as above, but if requested (used for two pass occlusion tests)
  u32 late_visible_meshlet_instances = 0;
};

struct MeshletInstance {
  u32 mesh_instance_index = 0;
  u32 meshlet_index = 0;
};

struct MeshInstance {
  u32 mesh_index = 0;
  u32 lod_index = 0;
  u32 material_index = 0;
  u32 transform_index = 0;
  u32 meshlet_instance_visibility_offset = 0;
};

OX_CONST f32 CAMERA_SCALE_UNIT = 0.01f;
OX_CONST f32 INV_CAMERA_SCALE_UNIT = 1.0f / CAMERA_SCALE_UNIT;
OX_CONST f32 PLANET_RADIUS_OFFSET = 0.001f;

struct Atmosphere {
  f32x3 rayleigh_scatter = f32x3(0.005802f, 0.013558f, 0.033100f);
  f32 rayleigh_density = 8.0f;

  f32x3 mie_scatter = f32x3(0.003996f, 0.003996f, 0.003996f);
  f32 mie_density = 1.2f;
  f32 mie_extinction = 0.004440f;
  f32 mie_asymmetry = 3.6f;
  f32 mie_haze_amount = 0.7f;
  f32 mie_haze_scale_height = 11.0f;

  f32x3 ozone_absorption = f32x3(0.000650f, 0.001881f, 0.000085f);
  f32 ozone_height = 25.0f;
  f32 ozone_thickness = 15.0f;

  f32x3 terrain_albedo = f32x3(0.3f, 0.3f, 0.3f);
  f32 planet_radius = 6360.0f;
  f32 atmos_radius = 6460.0f;
  f32 aerial_perspective_start_km = 8.0f;
  f32 aerial_perspective_exposure = 1.0f;

  i32x3 transmittance_lut_size = {};
  i32x3 sky_view_lut_size = {};
  i32x3 multiscattering_lut_size = {};
  i32x3 aerial_perspective_lut_size = {};
};

struct Sky {
  f32x4 solid_color = f32x4(0.0f, 0.0f, 0.0f, 1.0f);
  f32x3 ambient_color = f32x3(0.03f, 0.03f, 0.03f);
  u32 has_texture = 0;
};

struct Camera {
  f32x4 position = {};

  mat4 projection = {};
  mat4 inv_projection = {};
  mat4 view = {};
  mat4 inv_view = {};
  mat4 projection_view = {};
  mat4 inv_projection_view = {};

  mat4 previous_projection = {};
  mat4 previous_inv_projection = {};
  mat4 previous_view = {};
  mat4 previous_inv_view = {};
  mat4 previous_projection_view = {};
  mat4 previous_inv_projection_view = {};

  f32x2 temporalaa_jitter = {};
  f32x2 temporalaa_jitter_prev = {};

  f32x4 frustum_planes[6] = {};

  f32x3 up = {};
  f32 near_clip = 0.0f;
  f32x3 forward = {};
  f32 far_clip = 0.0f;
  f32x3 right = {};
  f32 fov = 0.0f;
  u32 output_index = 0;
  f32x2 resolution = {};
  f32 acceptable_lod_error = 2.0f; // TODO: Make this configurable
  // negative when upscaling, so material textures keep display resolution detail
  f32 texture_mip_bias = 0.0f;
};

struct CullCamera {
  mat4 projection_view = {};
  f32x3 position = {};
  f32 acceptable_lod_error = {};
  f32x2 resolution = {};
  f32 near_clip = {};
  u32 mesh_instance_count = {};
  // only the main geometry pass sets this, culling and shadow users leave it zero
  f32x2 jitter = {};
  f32 texture_mip_bias = {};
};

// --- Lights ---

OX_CONST u32 MAX_POINT_LIGHTS = 128;
OX_CONST u32 MAX_SPOT_LIGHTS = 128;
OX_CONST u32 MAX_LIGHTS = MAX_POINT_LIGHTS + MAX_SPOT_LIGHTS;

OX_CONST u32 MAX_SHADOW_POINT_LIGHTS = 64;
OX_CONST u32 MAX_SHADOW_SPOT_LIGHTS = 64;

// per-cell shadow mask, point 0-63, then spot 0-63, packed into u32x4
OX_CONST i32x3 LIGHT_GRID_RESOLUTION = i32x3(64, 32, 64);
OX_CONST f32 LIGHT_GRID_CELL_SIZE = 8.0f;
OX_CONST u32 LIGHT_GRID_CELL_COUNT = 64u * 32u * 64u;

struct DirectionalLight {
  f32x3 color = f32x3(0.02f, 0.02f, 0.02f);
  f32 intensity = 10.0f;
  f32x3 direction = {};
};

enum class LightKind : u32 { Directional = 0, Point = 1, Spot = 2 };

struct Light {
  f32x3 position = {};
  f32 intensity = 1.0f;
  f32x3 color = f32x3(1.0f, 1.0f, 1.0f);
  f32 range = 0.0f;
  f32x3 direction = {};
  f32 inner_cone_angle = 0.0f; // spot only (radians)
  f32 outer_cone_angle = 0.0f; // spot only (radians)
  LightKind kind = LightKind::Point;
  // -1 disables shadows
  i32 shadow_map_index = -1;
  u32 pad = 0;
};

// --- DDGI ---

OX_CONST u32 DDGI_MAX_IMAGE_DIMENSION = 16384;

OX_CONST u32 DDGI_MAX_PROBE_COUNT = 1u << 18;
OX_CONST u32 DDGI_MAX_CASCADE_COUNT = 8;
// Octahedral tiles, each padded with a one texel border that mirrors the opposite edge so bilinear
// taps stay continuous across the octahedron seam.
OX_CONST u32 DDGI_IRRADIANCE_TEXELS = 12;
OX_CONST u32 DDGI_RADIANCE_TEXELS = 6;
OX_CONST u32 DDGI_DISTANCE_TEXELS = 12;
OX_CONST u32 DDGI_TRACE_TEXELS = 12;
OX_CONST u32 DDGI_RAYS_PER_PROBE = DDGI_TRACE_TEXELS * DDGI_TRACE_TEXELS;
OX_CONST u32 DDGI_PROBES_PER_ATLAS_ROW = 512;
OX_CONST u32 DDGI_IRRADIANCE_ATLAS_WIDTH = DDGI_PROBES_PER_ATLAS_ROW * (DDGI_IRRADIANCE_TEXELS + 2);
OX_CONST u32 DDGI_RADIANCE_PROBES_PER_ATLAS_ROW = DDGI_IRRADIANCE_ATLAS_WIDTH / (DDGI_RADIANCE_TEXELS + 2);

OX_CONST u32 DDGI_PROBE_SELECT_GROUP = 64;
OX_CONST u32 DDGI_TEXEL_UPDATE_GROUP = 8;
OX_CONST u32 DDGI_TEXEL_UPDATE_THREADS_Y = 48;
OX_CONST u32 DDGI_TEXEL_UPDATE_GROUPS_Y = DDGI_TEXEL_UPDATE_THREADS_Y / DDGI_TEXEL_UPDATE_GROUP;

OX_CONST u32 DDGI_DEBUG_SPHERE_RINGS = 8;
OX_CONST u32 DDGI_DEBUG_SPHERE_SECTORS = 12;
OX_CONST u32 DDGI_DEBUG_SPHERE_VERTEX_COUNT = DDGI_DEBUG_SPHERE_RINGS * DDGI_DEBUG_SPHERE_SECTORS * 6;

struct ProbeUpdateArgs {
  u32 count = 0;
  DispatchIndirectCommand irradiance = {};
  DispatchIndirectCommand distance = {};
  DispatchIndirectCommand relocate = {};
};

struct ProbeState {
  f32x3 offset = {};
  u32 flags = 0;
};

struct ProbeVolume {
  f32x3 origin = {};
  u32 probe_offset = 0;
  f32x3 spacing = {};
  u32 probe_count = 0;
  f32x3 spacing_rcp = {};
  u32 cascade_index = 0;
  u32x3 counts = {};
  u32 cascade_count = 0;
  i32x3 scroll = {};
  f32 cascade_blend = 0.3f;
  f32x3 center = {};
  f32 max_probe_distance = 0.0f;
};

// --- Post processing ---

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
OX_BITMASK(SceneFlags)

// the exposure multiplier is the second field, the first is the adapted scene luminance
struct HistogramLuminance {
  f32 adapted_luminance = 0.0f;
  f32 exposure = 0.0f;
};

struct VBGTAOSettings {
  f32 thickness = 0.25f;
  u32 slice_count = 3;
  u32 samples_per_slice_side = 3;
  f32 effect_radius = 0.5f;
  u32 noise_index = 0;
  f32 final_power = 2.2f;
};

struct PostProcessSettings {
  f32 exposure = 1.0f;
  f32 chromatic_aberration_amount = 0.5f;
  f32 vignette_amount = 0.5f;
  f32 film_grain_scale = 1.0f;
  f32 film_grain_amount = 0.5f;
  u32 film_grain_seed = 0;
};

// same field order as the SDK's cbFSR3Upscaler so the port stays comparable against the reference
struct FSR3Constants {
  i32x2 render_size = {};
  i32x2 previous_frame_render_size = {};

  i32x2 upscale_size = {};
  i32x2 previous_frame_upscale_size = {};

  i32x2 max_render_size = {};
  i32x2 max_upscale_size = {};

  f32x4 device_to_view_depth = {};

  f32x2 jitter_offset = {};
  f32x2 previous_frame_jitter_offset = {};

  f32x2 motion_vector_scale = {};
  f32x2 downscale_factor = {};

  f32x2 motion_vector_jitter_cancellation = {};
  f32 tan_half_fov = 0.0f;
  f32 jitter_phase_count = 1.0f;

  f32 delta_time = 0.0f;
  f32 delta_pre_exposure = 1.0f;
  f32 view_space_to_meters_factor = 1.0f;
  f32 frame_index = 0.0f;

  f32 velocity_factor = 1.0f;
  f32 reactiveness_scale = 1.0f;
  f32 shading_change_scale = 1.0f;
  f32 accumulation_added_per_frame = 1.0f / 3.0f;
  f32 min_disocclusion_accumulation = -1.0f / 3.0f;
};

enum class TonemapType : u32 {
  None = 0,
  ACES,
  AgX,
  GT7,
};

// --- Virtual shadow maps ---

struct VSMAllocRequest {
  i32x3 page_table_address = {};
  // -1 for directional pages
  i32 mip = -1;
};

struct VSMPageAllocator {
  u32 active_request_count = 0;
  u32 dirty_physical_page_count = 0;
  u32 free_page_count = 0;
  u32 alloc_cursor = 0;
  u32 request_capacity = 0;
  u32 pad = 0;
  OX_PTR(VSMAllocRequest) requests = {};
  OX_PTR(u32x2) dirty_physical_page_coords = {};
  OX_PTR(u32) free_page_list = {};
};

// point layer = light * 6 + face, spot layers follow all point layers
struct VSMPointSpotView {
  mat4 projection_view = {};
  f32x3 light_position = {};
  f32 range = 0.0f;             // 0 means the layer is inactive
  u32 light_index = 0;
  f32 z_near = 0.0f;
  f32 texel_world_scale = 1.0f; // tan(fov/2), for world-space texel sizing in shading
  u32 pad = 0;
};

struct VSMMeshletInstance {
  u32 mesh_instance_index = 0;
  u32 meshlet_index = 0;
  u32 layer = 0;
};

struct VSMPointSpotContext {
  i32 curr_mip = 0;
  u32 layer_count = 0;
  u32 mesh_instance_count = 0;
  i32x2 depth_extent = {};
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
  i32x2 depth_extent = {};
  f32 first_clipmap_width = 0.0f;
  f32 clipmap_selection_bias = 0.0f;
  f32 virtual_extent = 0.0f;
  f32 z_length = 0.0f;
  f32x3 directional_light_dir = {};
};

struct VirtualClipmap {
  mat4 projection_view_mat = {};
  i32x2 page_offset = {};
  f32 z_near = 0.0f;
};

enum class CullFlag : u32 {
  None = 0,
  TestFrustum = 1 << 0,
  SelectLOD = 1 << 1,
  TestOcclusion = 1 << 2,
  LatePass = 1 << 3,

  TestAll = (1 << 0) | (1 << 1) | (1 << 2),
};
OX_BITMASK(CullFlag)

// --- Terrain ---

OX_CONST u32 TERRAIN_MAX_LAYERS = 4;

struct TerrainErosion {
  f32 scale = 0.15f;
  f32 strength = 0.22f;
  f32 gully_weight = 0.5f;
  f32 detail = 1.5f;
  // x: ridge rounding, y: crease rounding, z: input height rounding, w: per-octave multiplier
  f32x4 rounding = f32x4(0.1f, 0.0f, 0.1f, 2.0f);
  // x: input slope -> mask, y: octave slope -> mask, z: input slope -> ridge mask, w: octave -> ridge mask
  f32x4 onset = f32x4(1.25f, 1.25f, 2.8f, 1.5f);
  // x: the pretended input slope magnitude, y: blend between real and assumed slope
  f32x2 assumed_slope = f32x2(0.7f, 1.0f);
  f32 cell_scale = 0.7f;
  f32 gain = 0.5f;
  f32 lacunarity = 2.0f;
  f32 normalization = 0.5f;
  u32 octaves = 5;
  u32 seed = 0;
};

struct TerrainGenerate {
  TerrainErosion erosion = {};
  u32x2 resolution = {};
  f32x2 height_offset = f32x2(-0.65f, 0.0f);
  f32 domain_size = 2.0f;
  f32 height_frequency = 3.0f;
  f32 height_amplitude = 0.125f;
  f32 height_lacunarity = 2.0f;
  f32 height_gain = 0.1f;
  u32 height_octaves = 3;
};

struct TerrainDerive {
  u32x2 resolution = {};
  f32x2 texel_world_size = {};
  f32 height_range = 0.0f;
  f32 slope_rock_begin = 0.55f;
  f32 slope_rock_end = 0.8f;
  f32 altitude_snow_begin = 0.7f;
  f32 altitude_snow_end = 0.85f;
  f32 ridge_drainage_scale = 1.0f;
};

struct TerrainMinMax {
  u32x2 resolution = {};
  u32x2 patch_count = {};
};

struct TerrainRegion {
  u32x2 texel_origin = {};
  u32x2 patch_origin = {};
};

enum class TerrainBrushMode : u32 {
  Raise = 0,
  Smooth,
  Flatten,
  Noise,
  PaintLayer,
};

struct TerrainBrushHit {
  f32x3 world_position = {};
  u32 valid = 0;
};

struct TerrainBrushParams {
  f32x3 ray_origin = {};
  f32 radius_texels = 0.0f;
  f32x3 ray_direction = {};
  f32 strength = 0.0f;
  u32x2 resolution = {};
  f32x2 world_min = {};
  f32x2 world_size = {};
  f32x2 inv_world_size = {};
  u32x2 patch_count = {};
  f32 base_height = 0.0f;
  f32 height_scale = 0.0f;
  f32 falloff = 1.0f;
  f32 flatten_height = 0.0f;
  u32 mode = 0;
  u32 layer = 0;
};

OX_CONST u32 TERRAIN_INVALID_LAYER_MATERIAL = ~0u;

struct TerrainData {
  f32x2 world_min = {};
  f32x2 world_size = {};
  f32x2 inv_world_size = {};
  u32x2 patch_count = {};
  f32 base_height = 0.0f;
  f32 height_scale = 0.0f;
  f32 target_edge_pixels = 16.0f;
  f32 max_tessellation = 64.0f;
  f32 layer_tiling = 8.0f;
  f32 triplanar_begin = 0.5f;
  u32x4 layer_material_indices = u32x4(TERRAIN_INVALID_LAYER_MATERIAL);
  f32 brush_radius = 0.0f;
};

// --- 2D ---

OX_CONST u32 RENDER_FLAGS_2D_NONE = 0;
OX_CONST u32 RENDER_FLAGS_2D_SORT_Y = 1u << 0u;
OX_CONST u32 RENDER_FLAGS_2D_FLIP_X = 1u << 1u;

// also the sprite vertex input, one attribute per field
struct SpriteGPUData {
  u32 material_id16_ypos16 = 0;
  u32 flags16_distance16 = 0;
  u32 transform_id = 0;
};

// --- Particles ---

OX_CONST u32 PARTICLE_REGISTER_COUNT = 16;
OX_CONST u32 PARTICLE_SIMULATE_GROUP = 64;
OX_CONST u32 PARTICLE_SORT_GROUP = 256;
OX_CONST u32 PARTICLE_CURVE_ATLAS_WIDTH = 64;
OX_CONST u32 PARTICLE_USER_PARAM_COUNT = 4;

enum class ParticleOperandKind : u32 {
  Register = 0,
  Constant = 1,
  Immediate = 2,
};

enum class ParticleOpCode : u32 {
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
  u32 op_dst = 0;
  u32 src0 = 0;
  u32 src1 = 0;
  u32 src2 = 0;
};

OX_CONST u32 PARTICLE_REG_POSITION_LIFE = 0;  // xyz position, w life_remaining
OX_CONST u32 PARTICLE_REG_VELOCITY_TOTAL = 1; // xyz velocity, w life_total
OX_CONST u32 PARTICLE_REG_SIZE_ROT_SEED = 2;  // xy size, z rotation, w seed
OX_CONST u32 PARTICLE_REG_COLOR = 3;          // rgba
OX_CONST u32 PARTICLE_REG_CONTEXT = 4;        // x age_norm, y emitter time, z delta time, w flipbook frame

struct Particle {
  f32x3 position = {};
  f32 life_remaining = 0.0f;
  f32x3 velocity = {};
  f32 life_total = 0.0f;
  f32x2 size = {};
  f32 rotation = 0.0f;
  f32 seed = 0.0f;
  u32 color = 0;
  u32 flipbook_frame = 0;
  u32 emitter_index = 0;
  u32 flags = 0;
};

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
OX_BITMASK(ParticleEmitterFlags)

enum class ParticleEmissionShape : u32 {
  Point = 0,
  Sphere,
  Hemisphere,
  Box,
  Circle,
  Cone,
  Count,
};

struct ParticleEmitter {
  mat4 transform = mat4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  f32x4 rotation = f32x4(0.0f, 0.0f, 0.0f, 1.0f);

  u32 pool_offset = 0;
  u32 capacity = 0;
  u32 spawn_count = 0;
  u32 spawn_offset = 0;

  u32 spawn_program_offset = 0;
  u32 spawn_program_count = 0;
  u32 update_program_offset = 0;
  u32 update_program_count = 0;

  u32 constants_offset = 0;
  u32 curve_atlas_index = ~0u;
  u32 curve_sampler_index = 0;
  u32 curve_row_count = 0;

  u32 atlas_row_count = 0;
  u32 material_index = 0;
  u32 flipbook_x = 1;
  u32 flipbook_y = 1;

  ParticleEmitterFlags flags = ParticleEmitterFlags::None;
  ParticleEmissionShape shape = ParticleEmissionShape::Point;
  u32 seed = 0;
  f32 time = 0.0f;

  f32 delta_time = 0.0f;
  f32 restitution = 0.4f;
  f32 soft_particle_distance = 0.0f;
  f32 velocity_stretch = 1.0f;

  f32x2 lifetime = f32x2(1.0f, 1.0f);
  f32x4 shape_params = {};
  f32x4 velocity_offset = {};
  f32x4 user_params[PARTICLE_USER_PARAM_COUNT] = {};
};

struct ParticleSortKey {
  u32 key = ~0u;
  u32 index = ~0u;
};

struct ParticleCounters {
  u32 alive_count = 0;
  u32 alive_count_next = 0;
  u32 draw_count = 0;
  u32 pad = 0;
};

OX_GPU_END
