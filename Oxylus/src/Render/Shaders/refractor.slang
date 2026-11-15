module refractor;

import common;
import gpu;
import scene;
import visbuffer;
import hit;
import pbr;

#include <defines.slang>

// stands in for the distance to a background that is only sky
public static const f32 REFRACTOR_SKY_DISTANCE = 100.0;

public struct RefractorDecode {
    public bool is_valid;
    public TriangleHit tri;
    public Material material;
    public DecodedSurface decoded;
    // the water normal and filtered roughness already applied
    public SurfaceMaterial surface;
    public f32x3 world_position;
    public bool is_water;
    public f32 column;
    public f32 shore;
}

public func refractor_opaque_depth_at(in Camera camera, Texture2D<f32> opaque_depth, f32x2 uv) -> f32 {
    let resolution = i32x2(camera.resolution);
    let pixel = clamp(i32x2(uv * camera.resolution), i32x2(0), resolution - 1);
    return opaque_depth.Load(i32x3(pixel, 0));
}

// distance from a surface position to the opaque point seen at uv
public func refractor_background_distance(
    in Camera camera, Texture2D<f32> opaque_depth, f32x3 world_position, f32x2 uv
) -> f32 {
    let depth = refractor_opaque_depth_at(camera, opaque_depth, uv);
    let is_sky = depth <= 0.0;
    // the reversed infinite projection puts the sky at w = 0, which unproject would divide by
    let background_position = camera.unproject_uv(uv, max(depth, 1e-7));
    let distance_behind = min(distance(background_position, world_position), REFRACTOR_SKY_DISTANCE);

    return select(is_sky, REFRACTOR_SKY_DISTANCE, distance_behind);
}

func water_tangent_normal(in Material material, in UVGradient uv_grad, f32 time) -> f32x3 {
    let flow = material.get_water_flow();
    var base_layer = uv_grad;
    base_layer.uv += flow * time;
    // a second layer at another scale drifting across the first, so the pattern never visibly repeats
    var detail_layer = uv_grad;
    detail_layer.scale_offset(f32x2(max(material.get_water_detail_scale(), 1e-3)), f32x2(-flow.y, flow.x) * 0.7 * time);

    let base_normal = material.sample_tangent_normal(base_layer);
    let detail_normal = material.sample_tangent_normal(detail_layer);
    // whiteout blend keeps the slopes of both layers
    return normalize(f32x3(base_normal.xy + detail_normal.xy, base_normal.z * detail_normal.z));
}

func water_world_normal(in Material material, in TangentFrame frame, in UVGradient uv_grad, f32 time, f32 shore) -> f32x3 {
    let tangent_normal = water_tangent_normal(material, uv_grad, time);
    return frame.to_world(lerp(f32x3(0.0, 0.0, 1.0), tangent_normal, shore));
}

func shade_water_surface(in Camera camera, f32 time, in Material material, in DecodedSurface decoded, f32 shore)
    -> SurfaceMaterial {
    var surface = decoded.surface;
    surface.metallic = 0.0;
    surface.normal = water_world_normal(material, decoded.frame, decoded.uv_grad, time, shore);

    // the moving normal needs its own footprint, measured explicitly as the visbuffer decode does
    let unbiased_scale = exp2(-camera.texture_mip_bias);
    var sample_dx = decoded.uv_grad;
    sample_dx.uv += decoded.uv_grad.ddx * unbiased_scale;
    var sample_dy = decoded.uv_grad;
    sample_dy.uv += decoded.uv_grad.ddy * unbiased_scale;
    let normal_dx = water_world_normal(material, decoded.frame, sample_dx, time, shore) - surface.normal;
    let normal_dy = water_world_normal(material, decoded.frame, sample_dy, time, shore) - surface.normal;
    let roughness = material.sample_metallic_roughness(decoded.uv_grad).y;
    surface.roughness = filter_specular_roughness(roughness, normal_dx, normal_dy);

    return surface;
}

// pixel_uv is the centre of the pixel, the visbuffer was rasterized jittered so the lookup undoes that
public func decode_refractor(
    in SceneGeometry geometry,
    StructuredBuffer<MeshletInstance> meshlet_instances,
    in Camera camera,
    Texture2D<f32> opaque_depth,
    f32 time,
    u32 texel,
    f32x2 pixel_uv
) -> RefractorDecode {
    RefractorDecode result = {};
    result.shore = 1.0;

    result.tri = resolve_visbuffer_triangle(geometry, meshlet_instances, VisBufferData(texel));
    result.is_valid = result.tri.valid;
    if (!result.is_valid) {
        return result;
    }

    result.material = geometry.materials[result.tri.material_index];
    let sample_uv = pixel_uv - camera.temporalaa_jitter / camera.resolution;
    result.decoded = decode_triangle_surface(result.tri, result.material, sample_uv * 2.0 - 1.0, camera);
    result.world_position = result.tri.interpolate_position(result.decoded.triangle.deriv.lambda);
    result.is_water = HAS_FLAG(result.material.flags, MaterialFlag::Water);
    result.surface = result.decoded.surface;

    if (result.is_water) {
        result.column = refractor_background_distance(camera, opaque_depth, result.world_position, pixel_uv);
        // ripples and refraction calm down where the water gets shallow
        result.shore = saturate(result.column / max(result.material.get_water_shore_fade(), 1e-3));
        result.surface = shade_water_surface(camera, time, result.material, result.decoded, result.shore);
    }

    return result;
}
