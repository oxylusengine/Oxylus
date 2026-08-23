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
  GeometricNormal,
};

public interface IUVSample {
  func sample(u32 image_index, u32 sampler_index) -> f32x4;
  [[mutating]] func scale_offset(f32x2 size, f32x2 offset);
};

public struct UVGradient : IUVSample {
  public f32x2 uv;
  public f32x2 ddx;
  public f32x2 ddy;

  public func sample(u32 image_index, u32 sampler_index) -> f32x4 {
    return global_images[NonUniformResourceIndex(image_index)]
      .SampleGrad(global_samplers[NonUniformResourceIndex(sampler_index)], this.uv, this.ddx, this.ddy);
  }

  [[mutating]]
  public func scale_offset(f32x2 size, f32x2 offset) {
    this.uv = this.uv * size + offset;
    this.ddx *= size;
    this.ddy *= size;
  }
};

public struct UVLod : IUVSample {
  public f32x2 uv;
  public f32 lod;

  public __init(f32x2 uv, f32 lod) {
    this.uv = uv;
    this.lod = lod;
  }

  public func sample(u32 image_index, u32 sampler_index) -> f32x4 {
    return global_images[NonUniformResourceIndex(image_index)]
      .SampleLevel(global_samplers[NonUniformResourceIndex(sampler_index)], this.uv, this.lod);
  }

  [[mutating]]
  public func scale_offset(f32x2 size, f32x2 offset) {
    this.uv = this.uv * size + offset;
  }
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
  public u16x4 albedo_color = {};
  public u16x3 emissive_color = {};
  public u16 roughness_factor = 0;
  public u16 metallic_factor = 0;
  public u16 alpha_cutoff = 0;
  public MaterialFlag flags = MaterialFlag::None;
  public u32 sampler_index = 0;
  public u32 albedo_image_index = 0;
  public u32 normal_image_index = 0;
  public u32 emissive_image_index = 0;
  public u32 metallic_rougness_image_index = 0;
  public u32 occlusion_image_index = 0;
  public u16x2 uv_size = {};
  public u16x2 uv_offset = {};

  public func get_albedo_color() -> f32x4 {
    return f32x4(
      com::dequantize_half(albedo_color.x),
      com::dequantize_half(albedo_color.y),
      com::dequantize_half(albedo_color.z),
      com::dequantize_half(albedo_color.w)
    );
  }

  public func get_emissive_color() -> f32x3 {
    return f32x3(
      com::dequantize_half(emissive_color.x),
      com::dequantize_half(emissive_color.y),
      com::dequantize_half(emissive_color.z)
    );
  }

  public func get_roughness_factor() -> f32 {
    return com::dequantize_half(roughness_factor);
  }

  public func get_metallic_factor() -> f32 {
    return com::dequantize_half(metallic_factor);
  }

  public func get_alpha_cutoff() -> f32 {
    return com::dequantize_half(alpha_cutoff);
  }

  public func get_uv_size() -> f32x2 {
    return f32x2(
      com::dequantize_half(uv_size.x),
      com::dequantize_half(uv_size.y)
    );
  }

  public func get_uv_offset() -> f32x2 {
    return f32x2(
      com::dequantize_half(uv_offset.x),
      com::dequantize_half(uv_offset.y)
    );
  }

  public func transform_uv<S : IUVSample>(inout S s) {
    s.scale_offset(this.get_uv_size(), this.get_uv_offset());
  }

  public func sample_albedo_color<S : IUVSample>(in S s) -> f32x4 {
    let albedo = this.get_albedo_color();
    if (this.flags & MaterialFlag::HasAlbedoImage) {
        return albedo * s.sample(this.albedo_image_index, this.sampler_index);
    }

    return albedo;
  }

  public func sample_normal_color<S : IUVSample>(in S s) -> f32x3 {
    if (this.flags & MaterialFlag::HasNormalImage) {
        return s.sample(this.normal_image_index, this.sampler_index).rgb;
    }

    return { 0.0 };
  }

  public func sample_emissive_color<S : IUVSample>(in S s) -> f32x3 {
    let emissive = this.get_emissive_color();
    if (this.flags & MaterialFlag::HasEmissiveImage) {
        return emissive * s.sample(this.emissive_image_index, this.sampler_index).rgb;
    }

    return emissive;
  }

  public func sample_metallic_roughness<S : IUVSample>(in S s) -> f32x2 {
    let metallic_roughness = f32x2(this.get_metallic_factor(), this.get_roughness_factor());
    if (this.flags & MaterialFlag::HasMetallicRoughnessImage) {
        return metallic_roughness * s.sample(this.metallic_rougness_image_index, this.sampler_index).bg;
    }

    return metallic_roughness;
  }

  public func sample_occlusion_color<S : IUVSample>(in S s) -> f32 {
    if (this.flags & MaterialFlag::HasOcclusionImage) {
        return s.sample(this.occlusion_image_index, this.sampler_index).r;
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

    public func get_eye_pos(f32x3 position) -> f32x3 {
        let min_altitude = this.planet_radius + PLANET_RADIUS_OFFSET;
        let altitude = max(position.y * CAMERA_SCALE_UNIT + min_altitude, min_altitude);
        return f32x3(0.0, altitude, 0.0);
    }
};

public struct Sky {
    public f32x4 solid_color;
    public f32x3 ambient_color;
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
  TransparentBackground = 1 << 11,
  HasDDGI = 1 << 12,
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
  public i32 shadow_map_index = -1;
  public u32 _pad_0 = 0;
};

public constexpr static u32 MAX_SHADOW_POINT_LIGHTS = 64;
public constexpr static u32 MAX_SHADOW_SPOT_LIGHTS = 64;

// per-cell shadow mask, point 0-63, then spot 0-63, packed into u32x4
public constexpr static i32x3 LIGHT_GRID_RESOLUTION = i32x3(64, 32, 64);
public constexpr static f32 LIGHT_GRID_CELL_SIZE = 8.0;
public constexpr static u32 LIGHT_GRID_CELL_COUNT = 64u * 32u * 64u;

public func light_grid_cell_coords(f32x3 world_pos, f32x3 grid_origin) -> i32x3 {
  return i32x3(floor((world_pos - grid_origin) / LIGHT_GRID_CELL_SIZE));
}

public func light_grid_cell_in_bounds(i32x3 cell) -> bool {
  return all(cell >= 0) && all(cell < LIGHT_GRID_RESOLUTION);
}

public func light_grid_cell_index(i32x3 cell) -> u32 {
  return u32(cell.x) + u32(cell.y) * u32(LIGHT_GRID_RESOLUTION.x) +
         u32(cell.z) * u32(LIGHT_GRID_RESOLUTION.x * LIGHT_GRID_RESOLUTION.y);
}

public func light_grid_mask_word(LightKind kind, i32 shadow_map_index) -> u32 {
  return (kind == LightKind::Spot ? 2u : 0u) + u32(shadow_map_index >> 5);
}

public func light_grid_mask_bit(i32 shadow_map_index) -> u32 {
  return 1u << (u32(shadow_map_index) & 31u);
}

public struct TransformWorld {
  public mat4 world = {};

  public func to_world_position(in f32x3 position) -> f32x4 {
    return mul(this.world, f32x4(position, 1.0));
  }

  public func normal_matrix() -> mat3 {
    let basis = transpose(mat3(this.world));
    return transpose(mat3(
      cross(basis[1], basis[2]),
      cross(basis[2], basis[0]),
      cross(basis[0], basis[1])
    ));
  }

  public func to_world_normal(in f32x3 normal) -> f32x3 {
    return mul(this.normal_matrix(), normal);
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
    let n = this.normal_matrix();
    return { mul(n, normals[0]), mul(n, normals[1]), mul(n, normals[2]) };
  }
};

public struct TransformPrevious {
  public mat4 previous_world = {};
};

public struct Meshlet {
    public u32 indirect_vertex_index_offset = 0;
    public u32 local_triangle_index_offset = 0;
    public u32 vertex_count = 0;
    public u32 triangle_count = 0;

    public static func get_micro_index(Ptr<u32> buf, u32 byte_offset) -> u32 {
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
        return mesh.positions(indices);
    }

    public func normals(in Mesh mesh, in u32x3 indices) -> f32x3x3 {
        return mesh.normals(indices);
    }

    public func tex_coords(in Mesh mesh, in u32x3 indices) -> f32x2x3 {
        return mesh.tex_coords(indices);
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
    public MeshLOD *lods = nullptr;
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

    public func positions(in u32x3 indices) -> f32x3x3 {
        return { this.decode_position(indices.x),
                 this.decode_position(indices.y),
                 this.decode_position(indices.z) };
    }

    public func normals(in u32x3 indices) -> f32x3x3 {
        return { this.decode_normal(indices.x),
                 this.decode_normal(indices.y),
                 this.decode_normal(indices.z) };
    }

    public func tex_coords(in u32x3 indices) -> f32x2x3 {
        if (this.texture_coords == nullptr) {
            return {};
        }

        return { this.decode_texcoord(indices.x),
                 this.decode_texcoord(indices.y),
                 this.decode_texcoord(indices.z) };
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
    public f32 exposure;
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

// --- Terrain ---

public struct TerrainErosion {
    public f32 scale;
    public f32 strength;
    public f32 gully_weight;
    public f32 detail;
    // x: ridge rounding, y: crease rounding, z: input height rounding, w: per-octave multiplier
    public f32x4 rounding;
    // x: input slope -> mask, y: octave slope -> mask, z: input slope -> ridge mask, w: octave -> ridge mask
    public f32x4 onset;
    // x: the pretended input slope magnitude, y: blend between real and assumed slope
    public f32x2 assumed_slope;
    public f32 cell_scale;
    public f32 gain;
    public f32 lacunarity;
    public f32 normalization;
    public u32 octaves;
    public u32 seed;
};

public struct TerrainGenerate {
    public TerrainErosion erosion;
    public u32x2 resolution;
    public f32x2 height_offset;
    public f32 domain_size;
    public f32 height_frequency;
    public f32 height_amplitude;
    public f32 height_lacunarity;
    public f32 height_gain;
    public u32 height_octaves;
};

public struct TerrainDerive {
    public u32x2 resolution;
    public f32x2 texel_world_size;
    public f32 height_range;
    public f32 slope_rock_begin;
    public f32 slope_rock_end;
    public f32 altitude_snow_begin;
    public f32 altitude_snow_end;
    public f32 ridge_drainage_scale;
};

public struct TerrainMinMax {
    public u32x2 resolution;
    public u32x2 patch_count;
};

public struct TerrainRegion {
    public u32x2 texel_origin;
    public u32x2 patch_origin;
};

public enum TerrainBrushMode : u32 {
    Raise = 0,
    Smooth,
    Flatten,
    Noise,
    PaintLayer,
};

public struct TerrainBrushHit {
    public f32x3 world_position;
    public u32 valid;
};

public struct TerrainBrushParams {
    public f32x3 ray_origin;
    public f32 radius_texels;
    public f32x3 ray_direction;
    public f32 strength;
    public u32x2 resolution;
    public f32x2 world_min;
    public f32x2 world_size;
    public f32x2 inv_world_size;
    public u32x2 patch_count;
    public f32 base_height;
    public f32 height_scale;
    public f32 falloff;
    public f32 flatten_height;
    public u32 mode;
    public u32 layer;

    public func world_to_uv(f32x2 world_xz) -> f32x2 {
        return (world_xz - this.world_min) * this.inv_world_size;
    }

    public func decode_height(f32 normalized) -> f32 {
        return this.base_height + normalized * this.height_scale;
    }
};

public constexpr static u32 TERRAIN_INVALID_LAYER_MATERIAL = ~0u;

public struct TerrainData {
    public f32x2 world_min;
    public f32x2 world_size;
    public f32x2 inv_world_size;
    public u32x2 patch_count;
    public f32 base_height;
    public f32 height_scale;
    public f32 target_edge_pixels;
    public f32 max_tessellation;
    public f32 layer_tiling;
    public f32 triplanar_begin;
    public u32x4 layer_material_indices;
    public f32 brush_radius;

    // XZ world position of a patch corner, given the patch's grid coordinate.
    public func patch_corner(u32x2 patch, u32x2 corner) -> f32x2 {
        let grid = f32x2(patch + corner) / f32x2(this.patch_count);
        return this.world_min + grid * this.world_size;
    }

    public func world_to_uv(f32x2 world_xz) -> f32x2 {
        return (world_xz - this.world_min) * this.inv_world_size;
    }

    public func decode_height(f32 normalized) -> f32 {
        return this.base_height + normalized * this.height_scale;
    }
};

// --- Bindings ---

[[vk::binding(0, 1)]]
public SamplerState global_samplers[];
[[vk::binding(1, 1)]]
public Image2D<f32x4> global_images[];
[[vk::binding(2, 1)]]
public StorageImage2D<f32x4> global_storage_images[];
