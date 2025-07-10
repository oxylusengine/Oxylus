#pragma once

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

struct Meshlet {
  alignas(4) u32 vertex_offset = 0;
  alignas(4) u32 index_offset = 0;
  alignas(4) u32 triangle_offset = 0;
  alignas(4) u32 triangle_count = 0;
};

struct MeshletBounds {
  alignas(4) glm::vec3 aabb_min = {};
  alignas(4) glm::vec3 aabb_max = {};
};

struct MeshletInstance {
  alignas(4) u32 mesh_index = 0;
  alignas(4) u32 material_index = 0;
  alignas(4) u32 transform_index = 0;
  alignas(4) u32 meshlet_index = 0;
};

struct Mesh {
  alignas(8) u64 indices = 0;
  alignas(8) u64 vertex_positions = 0;
  alignas(8) u64 vertex_normals = 0;
  alignas(8) u64 texture_coords = 0;
  alignas(8) u64 meshlets = 0;
  alignas(8) u64 meshlet_bounds = 0;
  alignas(8) u64 local_triangle_indices = 0;
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
  alignas(4) glm::vec4 albedo_color = glm::vec4(1.0f);
  alignas(4) glm::vec2 uv_size = glm::vec3(1.0f);
  alignas(4) glm::vec2 uv_offset = glm::vec3(0.0f);
  alignas(4) glm::vec3 emissive_color = glm::vec3(1.0f);
  alignas(4) f32 roughness_factor = 0.0f;
  alignas(4) f32 metallic_factor = 0.0f;
  alignas(4) MaterialFlag flags = MaterialFlag::None;
  alignas(4) f32 alpha_cutoff = 0.0f;
  alignas(4) SamplingMode sampling_mode = SamplingMode::LinearRepeated;
  alignas(4) u32 albedo_image_index = ~0_u32;
  alignas(4) u32 normal_image_index = ~0_u32;
  alignas(4) u32 emissive_image_index = ~0_u32;
  alignas(4) u32 metallic_roughness_image_index = ~0_u32;
  alignas(4) u32 occlusion_image_index = ~0_u32;

  static GPU::Material from_material(const ox::Material& material,
                                     option<u32> albedo_id = nullopt,
                                     option<u32> normal_id = nullopt,
                                     option<u32> emissive_id = nullopt,
                                     option<u32> metallic_roughness_id = nullopt,
                                     option<u32> occlusion_id = nullopt) {
    auto mat = GPU::Material{
        .albedo_color = material.albedo_color,
        .uv_size = material.uv_size,
        .uv_offset = material.uv_offset,
        .emissive_color = material.emissive_color,
        .roughness_factor = material.roughness_factor,
        .metallic_factor = material.metallic_factor,
        .alpha_cutoff = material.alpha_cutoff,
        .sampling_mode = material.sampling_mode,
        .albedo_image_index = albedo_id.value_or(~0_u32),
        .normal_image_index = normal_id.value_or(~0_u32),
        .emissive_image_index = emissive_id.value_or(~0_u32),
        .metallic_roughness_image_index = metallic_roughness_id.value_or(~0_u32),
        .occlusion_image_index = occlusion_id.value_or(~0_u32),
    };

    mat.flags |= albedo_id.has_value() ? GPU::MaterialFlag::HasAlbedoImage : GPU::MaterialFlag::None;
    mat.flags |= normal_id.has_value() ? GPU::MaterialFlag::HasNormalImage : GPU::MaterialFlag::None;
    mat.flags |= emissive_id.has_value() ? GPU::MaterialFlag::HasEmissiveImage : GPU::MaterialFlag::None;
    mat.flags |= metallic_roughness_id.has_value() ? GPU::MaterialFlag::HasMetallicRoughnessImage
                                                   : GPU::MaterialFlag::None;
    mat.flags |= occlusion_id.has_value() ? GPU::MaterialFlag::HasOcclusionImage : GPU::MaterialFlag::None;

    switch (material.alpha_mode) {
      case AlphaMode::Opaque: mat.flags |= GPU::MaterialFlag::AlphaOpaque; break;
      case AlphaMode::Mask  : mat.flags |= GPU::MaterialFlag::AlphaMask; break;
      case AlphaMode::Blend : mat.flags |= GPU::MaterialFlag::AlphaBlend; break;
    }

    return mat;
  };
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

} // namespace ox::GPU
