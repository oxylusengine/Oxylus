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
};

[[Flags]]
public enum CullFlags : u32 {
    MeshletFrustum,
    TriangleBackFace,
    MicroTriangles,
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
  public f32x2 uv_size = {};
  public f32x2 uv_offset = {};
  public f32x3 emissive_color = {};
  public f32 roughness_factor = 0.0;
  public f32 metallic_factor = 0.0;
  public MaterialFlag flags = MaterialFlag::None;
  public f32 alpha_cutoff = 0.0;
  public u32 sampling_mode = 0;
  public u32 albedo_image_index = ~0u;
  public u32 normal_image_index = ~0u;
  public u32 emissive_image_index = ~0u;
  public u32 metallic_rougness_image_index = ~0u;
  public u32 occlusion_image_index = ~0u;

  public func sample_albedo_color(f32x2 uv) -> f32x4 { 
    if (this.albedo_image_index != ~0u) {
      const let color = material_images[this.albedo_image_index]
        .sample(material_samplers[this.sampling_mode], uv);
      return color * this.albedo_color;
    }

    return this.albedo_color;
  }

  public func sample_albedo_color(in UVGradient grad) -> f32x4 {
    if (this.flags & MaterialFlag::HasAlbedoImage) {
        const let color = material_images[this.albedo_image_index]
            .sample_grad(material_samplers[this.sampling_mode], grad.uv, grad.ddx, grad.ddy);
        return this.albedo_color * color;
    }

    return this.albedo_color;
  }

  public func sample_normal_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasNormalImage) {
        return material_images[this.normal_image_index]
            .sample_grad(material_samplers[this.sampling_mode], grad.uv, grad.ddx, grad.ddy).rgb;
    }

    return { 0.0 };
  }

  public func sample_emissive_color(in UVGradient grad) -> f32x3 {
    if (this.flags & MaterialFlag::HasEmissiveImage) {
        const let color = material_images[this.emissive_image_index]
            .sample_grad(material_samplers[this.sampling_mode], grad.uv, grad.ddx, grad.ddy).rgb;
        return this.emissive_color * color;
    }

    return this.emissive_color;
  }

  public func sample_metallic_roughness(in UVGradient grad) -> f32x2 {
    const let metallic_roughness = f32x2(this.metallic_factor, this.roughness_factor);
    if (this.flags & MaterialFlag::HasMetallicRoughnessImage) {
        const let color = material_images[this.metallic_rougness_image_index]
            .sample_grad(material_samplers[this.sampling_mode], grad.uv, grad.ddx, grad.ddy).bg;
        return metallic_roughness * color;
    }

    return metallic_roughness;
  }

  public func sample_occlusion_color(in UVGradient grad) -> f32 {
    if (this.flags & MaterialFlag::HasOcclusionImage) {
        return material_images[this.occlusion_image_index]
            .sample_grad(material_samplers[this.sampling_mode], grad.uv, grad.ddx, grad.ddy).r;
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

public typealias Triangle = u32x3;
public struct Meshlet {
    public u32 vertex_offset = 0;
    public u32 index_offset = 0;
    public u32 triangle_offset = 0;
    public u32 triangle_count = 0;

    // Takes a local triange index and returns an index to index buffer.
    public func index(in Mesh mesh, u32 i) -> u32 {
        return u32(mesh.local_triangle_indices[this.triangle_offset + i]);
    }

    // Returns index to an actual vertex.
    public func vertex(in Mesh mesh, u32 index) -> u32 {
        return mesh.indices[this.index_offset + index];
    }

    // Returns position of a vertex.
    public func position(in Mesh mesh, u32 vertex) -> f32x3 {
        return mesh.vertex_positions[this.vertex_offset + vertex];
    }

    public func tex_coord(in Mesh mesh, u32 vertex) -> f32x2 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return mesh.texture_coords[this.vertex_offset + vertex];
    }

    // ----------------------------------------------------------

    public func indices(in Mesh mesh, u32 i) -> Triangle {
        return {
            u32(mesh.local_triangle_indices[this.triangle_offset + i * 3 + 0]),
            u32(mesh.local_triangle_indices[this.triangle_offset + i * 3 + 1]),
            u32(mesh.local_triangle_indices[this.triangle_offset + i * 3 + 2]),
        };
    }

    public func vertices(in Mesh mesh, in Triangle indices) -> u32x3 {
        return { mesh.indices[this.index_offset + indices.x],
                 mesh.indices[this.index_offset + indices.y],
                 mesh.indices[this.index_offset + indices.z] };
    }

    public func positions(in Mesh mesh, in u32x3 vertices) -> f32x3x3 {
        return { mesh.vertex_positions[this.vertex_offset + vertices.x],
                 mesh.vertex_positions[this.vertex_offset + vertices.y],
                 mesh.vertex_positions[this.vertex_offset + vertices.z] };
    }

    public func normals(in Mesh mesh, in u32x3 vertices) -> f32x3x3 {
        return { mesh.vertex_normals[this.vertex_offset + vertices.x],
                 mesh.vertex_normals[this.vertex_offset + vertices.y],
                 mesh.vertex_normals[this.vertex_offset + vertices.z] };
    }

    public func tex_coords(in Mesh mesh, in u32x3 vertices) -> f32x2x3 {
        if (mesh.texture_coords == nullptr) {
            return {};
        }

        return { mesh.texture_coords[this.vertex_offset + vertices.x],
                 mesh.texture_coords[this.vertex_offset + vertices.y],
                 mesh.texture_coords[this.vertex_offset + vertices.z] };
    }
};

public struct MeshletBounds {
  public f32x3 aabb_min = {};
  public f32x3 aabb_max = {};
};

public struct MeshletInstance {
  public u32 mesh_index = 0;
  public u32 material_index = 0;
  public u32 transform_index = 0;
  public u32 meshlet_index = 0;
};

public struct Mesh {
  public u32 *indices = nullptr;
  public f32x3 *vertex_positions = nullptr;
  public f32x3 *vertex_normals = nullptr;
  public f32x2 *texture_coords = nullptr;
  public Meshlet *meshlets = nullptr;
  public MeshletBounds *meshlet_bounds = nullptr;
  public u8 *local_triangle_indices = nullptr;
};

public struct UVGradient {
  public f32x2 uv;
  public f32x2 ddx;
  public f32x2 ddy;
};

// --- Bindings ---

[[vk::binding(0, 1)]]
public SamplerState material_samplers[];
[[vk::binding(1, 1)]]
public Image2D<f32x4> material_images[];
