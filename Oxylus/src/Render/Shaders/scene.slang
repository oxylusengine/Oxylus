import common;
import gpu;
__exported import shared;

#include <defines.slang>

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

  // scaling both gradients by 2^bias shifts the selected lod by bias, same as a sampler lod bias
  [[mutating]]
  public func apply_mip_bias(f32 bias) {
    let scale = exp2(bias);
    this.ddx *= scale;
    this.ddy *= scale;
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

public extension Material {

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

  public func get_normal_scale() -> f32 {
    return com::dequantize_half(normal_scale);
  }

  public func get_occlusion_strength() -> f32 {
    return com::dequantize_half(occlusion_strength);
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

  public func sample_albedo_color_grad(in UVGradient grad) -> f32x4 {
    let albedo = this.get_albedo_color();
    if (this.flags & MaterialFlag::HasAlbedoImage) {
      let color = global_images[NonUniformResourceIndex(this.albedo_image_index)]
        .SampleGrad(
          global_samplers[NonUniformResourceIndex(this.sampler_index)],
          grad.uv,
          grad.ddx,
          grad.ddy
        );
      return albedo * color;
    }

    return albedo;
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

  // Decoded tangent-space normal, or a flat one when the material has no normal map. Two-component
  // maps rebuild z from the unscaled xy, since that reconstruction assumes a unit vector, and only
  // then does glTF's normalTexture.scale widen the xy.
  public func sample_tangent_normal<S : IUVSample>(in S s) -> f32x3 {
    if (!HAS_FLAG(this.flags, MaterialFlag::HasNormalImage)) {
        return f32x3(0.0, 0.0, 1.0);
    }

    var normal = this.sample_normal_color(s) * 2.0 - 1.0;

    if (this.flags & MaterialFlag::NormalFlipY) {
        normal.y = -normal.y;
    }

    if (this.flags & MaterialFlag::NormalTwoComponent) {
        normal.z = sqrt(saturate(1.0 - dot(normal.xy, normal.xy)));
    }

    normal.xy *= this.get_normal_scale();

    return normalize(normal);
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
        return metallic_roughness * s.sample(this.metallic_roughness_image_index, this.sampler_index).bg;
    }

    return metallic_roughness;
  }

  public func sample_occlusion_color<S : IUVSample>(in S s) -> f32 {
    if (this.flags & MaterialFlag::HasOcclusionImage) {
        let occlusion = s.sample(this.occlusion_image_index, this.sampler_index).r;
        return 1.0 + this.get_occlusion_strength() * (occlusion - 1.0);
    }

    return 1.0;
  }
};

public extension Camera {

  // jitter stays out of `projection` so cpu side culling and the motion vector reprojection keep
  // working off unjittered matrices. oxylus ndc has +y down, matching the texel space the upscaler
  // reports the offset in, so neither axis is flipped here
  public func jitter_clip(f32x4 clip_pos) -> f32x4 {
    clip_pos.xy += (2.0 * this.temporalaa_jitter / this.resolution) * clip_pos.w;
    return clip_pos;
  }

  public func unproject_uv(f32x2 uv, f32 depth) -> f32x3 {
    let ndc = f32x4(uv * 2.0 - 1.0, depth, 1.0);
    let world_pos_h = mul(this.inv_projection_view, ndc);
    return world_pos_h.xyz / world_pos_h.w;
  }
};

public extension CullCamera {

    // see Camera::jitter_clip, same convention
    public func jitter_clip(f32x4 clip_pos) -> f32x4 {
        clip_pos.xy += (2.0 * this.jitter / this.resolution) * clip_pos.w;
        return clip_pos;
    }
};

public extension Atmosphere {

    public func get_eye_pos(f32x3 position) -> f32x3 {
        let min_altitude = this.planet_radius + PLANET_RADIUS_OFFSET;
        let altitude = max(position.y * CAMERA_SCALE_UNIT + min_altitude, min_altitude);
        return f32x3(0.0, altitude, 0.0);
    }
};

public struct DirectionalLightCascade {
  public f32x4x4 projection_view;
  public f32 far_bound;
  public f32 texel_size;
};

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

public extension TransformWorld {

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

public extension Meshlet {

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

public extension MeshletBounds {

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

public extension SkinningTransform {
    public func rotate(in f32x3 v) -> f32x3 {
        let axis = rotation.xyz;
        return v + 2.0 * cross(axis, cross(axis, v) + rotation.w * v);
    }

    public func transform_point(in f32x3 p) -> f32x3 {
        return rotate(p * translation_scale.w) + translation_scale.xyz;
    }

    public func transform_normal(in f32x3 n) -> f32x3 {
        return rotate(n);
    }
};

public extension MeshInstance {
    public func is_skinned() -> bool {
        return skinned_vertex_positions != nullptr;
    }
};

public extension Mesh {

    // redirects vertex fetch at the instance's skinned copy, which the skinning pass wrote in the
    // same quantized format, so meshlet decode, LOD selection and culling bounds work unchanged
    [[mutating]]
    public func apply_skin(in MeshInstance instance) {
        if (instance.skinned_vertex_positions == nullptr) {
            return;
        }

        this.vertex_positions = instance.skinned_vertex_positions;
        this.vertex_normals = instance.skinned_vertex_normals;
        this.bounds = instance.skinned_bounds;
    }

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

public extension TerrainBrushParams {

    public func world_to_uv(f32x2 world_xz) -> f32x2 {
        return (world_xz - this.world_min) * this.inv_world_size;
    }

    public func decode_height(f32 normalized) -> f32 {
        return this.base_height + normalized * this.height_scale;
    }
};

public extension TerrainData {

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
