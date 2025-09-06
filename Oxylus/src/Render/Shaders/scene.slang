module material;

import common;
import gpu;

public const static f32 CAMERA_SCALE_UNIT     = 0.01;
public const static f32 INV_CAMERA_SCALE_UNIT = 1.0 / CAMERA_SCALE_UNIT;
public const static f32 PLANET_RADIUS_OFFSET  = 0.001;

public enum DebugView : i32 {
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
};

[[Flags]]
public enum CullFlags : u32 {
    MeshletFrustum,
    TriangleBackFace,
    MicroTriangles,
    OcclusionCulling,
    TriangleCulling,
};

public struct UVGradient {
  public f32x2 uv;
  public f32x2 ddx;
  public f32x2 ddy;
};

public enum MaterialFlag : u32 {
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

public struct Material {
  public f32x4 albedo_color = {};
  public f32x3 emissive_color = {};
  public f32 roughness_factor = 0.0;
  public f32 metallic_factor = 0.0;
  public f32 alpha_cutoff = 0.0;
  public MaterialFlag flags = MaterialFlag::None;
  public u32 sampler_index = 0;
  public u32 albedo_image_index = 0;
  public u32 normal_image_index = 0;
  public u32 emissive_image_index = 0;
  public u32 metallic_rougness_image_index = 0;
  public u32 occlusion_image_index = 0;
  public f32x2 uv_size = {};
  public f32x2 uv_offset = {};

  public func sample_albedo_color(f32x2 uv) -> f32x4 { 
    if (this.flags & MaterialFlag::HasAlbedoImage) {
      const let color = global_images[this.albedo_image_index]
        .sample(global_samplers[this.sampler_index], uv);
      return this.albedo_color * color;
    }

    return this.albedo_color;
  }

  public func sample_albedo_color(in UVGradient grad) -> f32x4 {
    if (this.flags & MaterialFlag::HasAlbedoImage) {
        const let color = global_images[this.albedo_image_index]
            .sample_grad(global_samplers[this.sampler_index], grad.uv, grad.ddx, grad.ddy);
        return this.albedo_color * color;
    }

    return this.albedo_color;
  }

  public func sample_normal_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasNormalImage) {
        return global_images[this.normal_image_index]
            .sample_grad(global_samplers[this.sampler_index], grad.uv, grad.ddx, grad.ddy).rgb;
    }

    return { 0.0 };
  }

  public func sample_emissive_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasEmissiveImage) {
        const let color = global_images[this.emissive_image_index]
            .sample_grad(global_samplers[this.sampler_index], grad.uv, grad.ddx, grad.ddy).rgb;
        return this.emissive_color * color;
    }

    return this.emissive_color;
  }

  public func sample_metallic_roughness(in UVGradient grad) -> f32x2 {
    const let metallic_roughness = f32x2(this.metallic_factor, this.roughness_factor);
    if (this.flags & MaterialFlag::HasMetallicRoughnessImage) {
        const let color = global_images[this.metallic_rougness_image_index]
            .sample_grad(global_samplers[this.sampler_index], grad.uv, grad.ddx, grad.ddy).bg;
        return metallic_roughness * color;
    }

    return metallic_roughness;
  }

  public func sample_occlusion_color(in UVGradient grad) -> f32 {
    if (this.flags & MaterialFlag::HasOcclusionImage) {
        return global_images[this.occlusion_image_index]
            .sample_grad(global_samplers[this.sampler_index], grad.uv, grad.ddx, grad.ddy).r;
    }

    return 1.0;
  }
};

public struct Camera {
  public f32x4 position = {};
  public mat4 projection = {};
  public mat4 inv_projection = {};
  public mat4 view = {};
  public mat4 inv_view = {};
  public mat4 projection_view = {};
  public mat4 inv_projection_view = {};
  public mat4 previous_projection = {};
  public mat4 previous_inv_projection = {};
  public mat4 previous_view = {};
  public mat4 previous_inv_view = {};
  public mat4 previous_projection_view = {};
  public mat4 previous_inv_projection_view = {};
  public f32x2 temporalaa_jitter = {};
  public f32x2 temporalaa_jitter_prev = {};
  public f32x4 frustum_planes[6] = {};
  public f32x3 up = {};
  public f32 near_clip = 0;
  public f32x3 forward = {};
  public f32 far_clip = 0;
  public f32x3 right = {};
  public f32 fov = 0;
  public u32 output_index = 0;
  public f32x2 resolution;
};

public struct Sun {
    public f32x3 direction;
    public f32   intensity;
};

public struct Atmosphere {
    public f32x3 eye_pos;

    public f32x3 rayleigh_scatter;
    public f32   rayleigh_density;

    public f32x3 mie_scatter;
    public f32   mie_density;
    public f32   mie_extinction;
    public f32   mie_asymmetry;

    public f32x3 ozone_absorption;
    public f32   ozone_height;
    public f32   ozone_thickness;

    public f32x3 terrain_albedo;
    public f32   planet_radius;
    public f32   atmos_radius;
    public f32   aerial_perspective_start_km;

    public i32x3 transmittance_lut_size;
    public i32x3 sky_view_lut_size;
    public i32x3 multiscattering_lut_size;
    public i32x3 aerial_perspective_lut_size;
};

public struct PointLight {
  public f32x3 position;
  public f32x3 color;
  public f32 intensity;
  public f32 cutoff;
};

public struct SpotLight {
  public f32x3 position;
  public f32x3 direction;
  public f32x3 color;
  public f32 intensity;
  public f32 cutoff;
  public f32 inner_cone_angle;
  public f32 outer_cone_angle;
};

public enum SceneFlags : u32 {
  None = 0,
  HasSun = 1 << 0,
  HasAtmosphere = 1 << 1,
  HasEyeAdaptation = 1 << 2,
  HasBloom = 1 << 3,
  HasFXAA = 1 << 4,
  HasGTAO = 1 << 5,
  HasFilmGrain = 1 << 6,
  HasChromaticAberration = 1 << 7,
  HasVignette = 1 << 8,
};

public struct LightSettings {
  public u32 point_light_count = 0;
  public u32 spot_light_count = 0;
};

public struct Scene {
  public SceneFlags scene_flags;
  public LightSettings light_settings;

  public Atmosphere atmosphere;
  public Sun sun;
  public PointLight* point_lights;
  public SpotLight* spot_lights;
};

public struct Transform {
  public mat4 local = {};
  public mat4 world = {};
  public mat3 normal = {};

  public func to_world_position(in f32x3 position) -> f32x4 {
    return mul(this.world, f32x4(position, 1.0));
  }

  public func to_world_normal(in f32x3 normal) -> f32x3 {
    return mul(this.normal, normal);
  }

  public func to_world_positions(in f32x3x3 positions) -> f32x4x3 {
    return {
      mul(this.world, f32x4(positions[0], 1.0)),
      mul(this.world, f32x4(positions[1], 1.0)),
      mul(this.world, f32x4(positions[2], 1.0)),
    };
  }

  public func to_world_normals(in f32x3x3 normals) -> f32x3x3 {
    return { mul(this.normal, normals[0]), mul(this.normal, normals[1]), mul(this.normal, normals[2]) };
  }
};

public struct Meshlet {
    public u32 indirect_vertex_index_offset = 0;
    public u32 local_triangle_index_offset = 0;
    public u32 vertex_count = 0;
    public u32 triangle_count = 0;

    // Takes a local triange index and returns an index to index buffer.
    public func index(in MeshLOD mesh_lod, u32 i) -> u32 {
        let local_triangle_index = u32(mesh_lod.local_triangle_indices[this.local_triangle_index_offset + i]);
        return mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_index];
    }

    // Returns position of a vertex.
    public func position(in Mesh mesh, u32 index) -> f32x3 {
        return mesh.vertex_positions[index];
    }

    public func tex_coord(in Mesh mesh, u32 index) -> f32x2 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return mesh.texture_coords[index];
    }

    // ----------------------------------------------------------

    public func indices(in MeshLOD mesh_lod, u32 i) -> u32x3 {
        let local_triangle_indices = u32x3(
            u32(mesh_lod.local_triangle_indices[this.local_triangle_index_offset + i * 3 + 0]),
            u32(mesh_lod.local_triangle_indices[this.local_triangle_index_offset + i * 3 + 1]),
            u32(mesh_lod.local_triangle_indices[this.local_triangle_index_offset + i * 3 + 2]),
        );

        return { mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.x],
                 mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.y],
                 mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.z] };
    }

    public func positions(in Mesh mesh, in u32x3 indices) -> f32x3x3 {
        return { mesh.vertex_positions[indices.x],
                 mesh.vertex_positions[indices.y],
                 mesh.vertex_positions[indices.z] };
    }

    public func normals(in Mesh mesh, in u32x3 indices) -> f32x3x3 {
        return { mesh.vertex_normals[indices.x],
                 mesh.vertex_normals[indices.y],
                 mesh.vertex_normals[indices.z] };
    }

    public func tex_coords(in Mesh mesh, in u32x3 indices) -> f32x2x3 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return { mesh.texture_coords[indices.x],
                 mesh.texture_coords[indices.y],
                 mesh.texture_coords[indices.z] };
    }
};

public struct Bounds {
    public f32x3 aabb_center = {};
    public f32x3 aabb_extent = {};
    public f32x3 sphere_center = {};
    public f32 sphere_radius = 0.0f;
};

public struct MeshletInstance {
    public u32 mesh_instance_index = 0;
    public u32 meshlet_index = 0;
};

public struct MeshInstance {
    public u32 mesh_index = 0;
    public u32 lod_index = 0;
    public u32 material_index = 0;
    public u32 transform_index = 0;
};

public struct MeshLOD {
    public u32 *indices = nullptr;
    public Meshlet *meshlets = nullptr;
    public Bounds *meshlet_bounds = nullptr;
    public u8 *local_triangle_indices = nullptr;
    public u32 *indirect_vertex_indices = nullptr;
    public u32 indices_count = 0;
    public u32 meshlet_count = 0;
    public u32 meshlet_bounds_count = 0;
    public u32 local_triangle_indices_count = 0;
    public u32 indirect_vertex_indices_count = 0;
    public f32 error = 0.0;
};

#ifndef MESH_MAX_LODS
#define MESH_MAX_LODS 8
#endif

public struct Mesh {
    public f32x3 *vertex_positions = nullptr;
    public f32x3 *vertex_normals = nullptr;
    public f32x2 *texture_coords = nullptr;
    public u32 vertex_count = 0;
    public u32 lod_count = 0;
    public MeshLOD lods[MESH_MAX_LODS] = {};
    public Bounds bounds = {};
};

public struct VBGTAOSettings {
    public f32 thickness = 0.25;
    public u32 slice_count = 3;
    public u32 samples_per_slice_side = 3;
    public f32 effect_radius = 0.5;
    public u32 noise_index = 0;
    public f32 final_power = 2.2;
};

public struct PostProcessSettings {
    public f32 chromatic_aberration_amount;
    public f32 vignette_amount;
    public f32 film_grain_scale;
    public f32 film_grain_amount;
    public u32 film_grain_seed;
};

// --- Bindings ---

[[vk::binding(0, 1)]]
public SamplerState global_samplers[];
[[vk::binding(1, 1)]]
public Image2D<f32x4> global_images[];
[[vk::binding(2, 1)]]
public StorageImage2D<f32x4> global_storage_images[];

