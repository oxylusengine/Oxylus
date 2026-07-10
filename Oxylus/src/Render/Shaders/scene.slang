import common;
import gpu;

#include <defines.slang>

public const static f32 CAMERA_SCALE_UNIT     = 0.01;
public const static f32 INV_CAMERA_SCALE_UNIT = 1.0 / CAMERA_SCALE_UNIT;
public const static f32 PLANET_RADIUS_OFFSET  = 0.001;

public enum DebugView : i32 {
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

  public func sample_albedo_color(in UVGradient grad) -> f32x4 {
    if (this.flags & MaterialFlag::HasAlbedoImage) {
        const let color = global_images[NonUniformResourceIndex(this.albedo_image_index)]
            .SampleGrad(global_samplers[NonUniformResourceIndex(this.sampler_index)], grad.uv, grad.ddx, grad.ddy);
        return this.albedo_color * color;
    }

    return this.albedo_color;
  }

  public func sample_normal_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasNormalImage) {
        return global_images[NonUniformResourceIndex(this.normal_image_index)]
            .SampleGrad(global_samplers[NonUniformResourceIndex(this.sampler_index)], grad.uv, grad.ddx, grad.ddy).rgb;
    }

    return { 0.0 };
  }

  public func sample_emissive_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasEmissiveImage) {
        const let color = global_images[NonUniformResourceIndex(this.emissive_image_index)]
            .SampleGrad(global_samplers[NonUniformResourceIndex(this.sampler_index)], grad.uv, grad.ddx, grad.ddy).rgb;
        return this.emissive_color * color;
    }

    return this.emissive_color;
  }

  public func sample_metallic_roughness(in UVGradient grad) -> f32x2 {
    const let metallic_roughness = f32x2(this.metallic_factor, this.roughness_factor);
    if (this.flags & MaterialFlag::HasMetallicRoughnessImage) {
        const let color = global_images[NonUniformResourceIndex(this.metallic_rougness_image_index)]
            .SampleGrad(global_samplers[NonUniformResourceIndex(this.sampler_index)], grad.uv, grad.ddx, grad.ddy).bg;
        return metallic_roughness * color;
    }

    return metallic_roughness;
  }

  public func sample_occlusion_color(in UVGradient grad) -> f32 {
    if (this.flags & MaterialFlag::HasOcclusionImage) {
        return global_images[NonUniformResourceIndex(this.occlusion_image_index)]
            .SampleGrad(global_samplers[NonUniformResourceIndex(this.sampler_index)], grad.uv, grad.ddx, grad.ddy).r;
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
  public f32 acceptable_lod_error;

  public func unproject_uv(f32x2 uv, f32 depth) -> f32x3 {
    let ndc = f32x4(uv * 2.0 - 1.0, depth, 1.0);
    let world_pos_h = mul(this.inv_projection_view, ndc);
    return world_pos_h.xyz / world_pos_h.w;
  }
};

public struct CullCamera {
    public mat4 projection_view = {};
    public f32x3 position = {};
    public f32 acceptable_lod_error = {};
    public f32x2 resolution = {};
    public f32 near_clip = {};
    public u32 mesh_instances_count = {};
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
    public f32   aerial_perspective_exposure;

    public i32x3 transmittance_lut_size;
    public i32x3 sky_view_lut_size;
    public i32x3 multiscattering_lut_size;
    public i32x3 aerial_perspective_lut_size;
};

public struct Sky {
    public f32x4 solid_color;
    public bool has_texture;
};

public enum SceneFlags : u32 {
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
};

public struct DirectionalLightCascade {
  public f32x4x4 projection_view;
  public f32 far_bound;
  public f32 texel_size;
};

public struct DirectionalLight {
  public f32x3 color;
  public f32 intensity;
  public f32x3 direction;
};

public enum LightKind : u32 { Directional = 0, Point = 1, Spot = 2 };

public struct Light {
  public f32x3 position = {};
  public f32 intensity = 1.0;
  public f32x3 color = {1.0, 1.0, 1.0};
  public f32 range = 0.0;
  public f32x3 direction = {};
  public f32 inner_cone_angle = 0.0; // spot only (radians)
  public f32 outer_cone_angle = 0.0; // spot only (radians)
  public LightKind kind = LightKind::Point;
  public u32 _pad_0 = 0;
  public u32 _pad_1 = 0;
};

public struct Transform {
  public mat4 local = {};
  public mat4 world = {};
  public mat4 previous_world = {};
  public mat3 normal = {};

  public func to_world_position(in f32x3 position) -> f32x4 {
    return mul(this.world, f32x4(position, 1.0));
  }

  public func to_world_normal(in f32x3 normal) -> f32x3 {
    return mul(this.normal, normal);
  }

  public func to_world_radius(in f32 radius) -> f32 {
    let sx = length(this.world[0].xyz);
    let sy = length(this.world[1].xyz);
    let sz = length(this.world[2].xyz);
    return radius * max(sx, max(sy, sz));
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

    static func get_micro_index(Ptr<u32> buf, u32 byte_offset) -> u32 {
        let pack_index = byte_offset >> 2;
        let index_pack = buf[pack_index];
        let in_pack_offset = byte_offset & 3;
        let in_pack_shift = in_pack_offset * 8;
        return (index_pack >> in_pack_shift) & 0xFF;
    }

    // Takes a local triange index and returns an index to index buffer.
    public func index(in MeshLOD mesh_lod, u32 i) -> u32 {
        let local_triangle_index = get_micro_index(mesh_lod.local_triangle_indices, this.local_triangle_index_offset + i);
        return mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_index];
    }

    // Returns position of a vertex.
    public func position(in Mesh mesh, u32 index) -> f32x3 {
        return mesh.decode_position(index);
    }

    public func tex_coord(in Mesh mesh, u32 index) -> f32x2 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return mesh.decode_texcoord(index);
    }

    // ----------------------------------------------------------

    public func indices(in MeshLOD mesh_lod, u32 i) -> u32x3 {
        let base = this.local_triangle_index_offset + i * 3;
        let local_triangle_indices = u32x3(
            get_micro_index(mesh_lod.local_triangle_indices, base + 0),
            get_micro_index(mesh_lod.local_triangle_indices, base + 1),
            get_micro_index(mesh_lod.local_triangle_indices, base + 2),
        );

        return { mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.x],
                 mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.y],
                 mesh_lod.indirect_vertex_indices[this.indirect_vertex_index_offset + local_triangle_indices.z] };
    }

    public func positions(in Mesh mesh, in u32x3 indices) -> f32x3x3 {
        return { mesh.decode_position(indices.x),
                 mesh.decode_position(indices.y),
                 mesh.decode_position(indices.z) };
    }

    public func normals(in Mesh mesh, in u32x3 indices) -> f32x3x3 {
        return { mesh.decode_normal(indices.x),
                 mesh.decode_normal(indices.y),
                 mesh.decode_normal(indices.z) };
    }

    public func tex_coords(in Mesh mesh, in u32x3 indices) -> f32x2x3 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return { mesh.decode_texcoord(indices.x),
                 mesh.decode_texcoord(indices.y),
                 mesh.decode_texcoord(indices.z) };
    }
};

public struct MeshletBounds {
    u16x3 aabb_center;
    i8x2 cone_axis_xy;
    u16x3 aabb_extent;
    i8 cone_axis_z;
    i8 cone_cutoff;

    public func get_cone_axis() -> f32x3 {
        return f32x3(
            i32(cone_axis_xy.x) / 127.0,
            i32(cone_axis_xy.y) / 127.0,
            i32(cone_axis_z) / 127.0
        );
    }

    public func get_cone_cutoff() -> f32 {
        return cone_cutoff / 127.0;
    }

    public func get_aabb_center() -> f32x3 {
        return f32x3(
            com::dequantize_half(aabb_center.x),
            com::dequantize_half(aabb_center.y),
            com::dequantize_half(aabb_center.z)
        );
    }

    public func get_aabb_extent() -> f32x3 {
        return f32x3(
            com::dequantize_half(aabb_extent.x),
            com::dequantize_half(aabb_extent.y),
            com::dequantize_half(aabb_extent.z)
        );
    }
};

public struct MeshBounds {
    public f32x3 aabb_center = {};
    public f32x3 aabb_extent = {};
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
    public u32 meshlet_instance_visibility_offset = 0;
};

public struct MeshLOD {
    public u32 *indices = nullptr;
    public Meshlet *meshlets = nullptr;
    public MeshletBounds *meshlet_bounds = nullptr;
    public u32 *local_triangle_indices = nullptr;
    public u32 *indirect_vertex_indices = nullptr;
    public u32 indices_count = 0;
    public u32 meshlet_count = 0;
    public u32 meshlet_bounds_count = 0;
    public u32 local_triangle_indices_count = 0;
    public u32 indirect_vertex_indices_count = 0;
    public f32 error = 0.0;
};

public struct Mesh {
    public u16x4 *vertex_positions = nullptr;
    public u32 *vertex_normals = nullptr;
    public u16x2 *texture_coords = nullptr;
    public u32 vertex_count = 0;
    public u32 lod_count = 0;
    public MeshLOD lods[MESH_MAX_LODS] = {};
    public MeshBounds bounds = {};

    public func decode_position(u32 index) -> f32x3 {
        return f32x3(
            com::dequantize_half(vertex_positions[index].x),
            com::dequantize_half(vertex_positions[index].y),
            com::dequantize_half(vertex_positions[index].z)
        );
    }

    public func decode_normal(u32 index) -> f32x3 {
        let packed = vertex_normals[index];
        return ((i32x3(packed) >> i32x3(20, 10, 0)) & i32x3(1023)) / 511.0 - 1.0;
    }

    public func decode_texcoord(u32 index) -> f32x2 {
        let packed = texture_coords[index];
        return f32x2(
            com::dequantize_half(packed.x),
            com::dequantize_half(packed.y)
        );
    }
};

public struct MeshletInstanceVisibility {
    public u32 total_visible_meshlet_instances;
    public u32 early_visible_meshlet_instances;
    public u32 late_visible_meshlet_instances;
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

public enum TonemapType : u32 {
    None = 0,
    ACES,
    AgX,
    GT7,
};

[[Flags]]
public enum CullFlag : u32 {
    TestFrustum,
    SelectLOD,
    TestOcclusion,
    LatePass,
};

// --- Bindings ---

[[vk::binding(0, 1)]]
public SamplerState global_samplers[];
[[vk::binding(1, 1)]]
public Image2D<f32x4> global_images[];
[[vk::binding(2, 1)]]
public StorageImage2D<f32x4> global_storage_images[];
