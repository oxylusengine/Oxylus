module decal;

import common;
import gpu;
import scene;
import hit;

public struct DecalTarget {
    public f32x3 albedo;
    public f32x3 normal;
    public f32x3 emissive;
    public f32 metallic;
    public f32 roughness;
};

// position_dx and position_dy are the world space footprint of one pixel
public func apply_decals(
    inout DecalTarget target,
    Decal *decals,
    u32 decal_count,
    StructuredBuffer<Material> materials,
    f32x3 world_position,
    f32x3 position_dx,
    f32x3 position_dy,
    f32x3 geometric_normal,
    u32 receiver_layer_mask,
    f32 mip_bias
) {
    for (u32 i = 0; i < decal_count; i++) {
        let decal = decals[i];
        if ((decal.layer_mask & receiver_layer_mask) == 0) {
            continue;
        }

        let local = mul(decal.world_to_decal, f32x4(world_position, 1.0)).xyz;
        if (any(abs(local) > 0.5)) {
            continue;
        }

        let facing = dot(geometric_normal, decal.world_axis_z);
        let angle_range = max(decal.angle_fade_start_cos - decal.angle_fade_end_cos, 1e-4);
        let angle_fade = saturate((facing - decal.angle_fade_end_cos) / angle_range);
        if (angle_fade <= 0.0) {
            continue;
        }

        let depth_fade = decal.depth_fade > 0.0 ? saturate((0.5 - abs(local.z)) / decal.depth_fade) : 1.0;

        let local_dx = mul(decal.world_to_decal, f32x4(position_dx, 0.0)).xyz;
        let local_dy = mul(decal.world_to_decal, f32x4(position_dy, 0.0)).xyz;

        let material = materials[decal.material_index];
        UVGradient grad;
        grad.uv = f32x2(local.x + 0.5, 0.5 - local.y);
        grad.ddx = f32x2(local_dx.x, -local_dx.y);
        grad.ddy = f32x2(local_dy.x, -local_dy.y);
        material.transform_uv(grad);

        // the tangent frame wants the unbiased derivatives
        let uv_dx = grad.ddx;
        let uv_dy = grad.ddy;
        grad.apply_mip_bias(mip_bias);

        let albedo = material.sample_albedo_color(grad);
        let weight = albedo.a * decal.opacity * angle_fade * depth_fade;
        if (weight <= 0.0) {
            continue;
        }

        if (decal.flags & DecalFlag::AffectAlbedo) {
            target.albedo = lerp(target.albedo, albedo.rgb, weight);
        }

        let is_normal_mapped = (material.flags & MaterialFlag::HasNormalImage) != 0;
        if ((decal.flags & DecalFlag::AffectNormal) != 0 && is_normal_mapped) {
            let frame = build_tangent_frame(target.normal, position_dx, position_dy, uv_dx, uv_dy);
            let normal = frame.to_world(material.sample_tangent_normal(grad));
            target.normal = normalize(lerp(target.normal, normal, weight));
        }

        if (decal.flags & DecalFlag::AffectMetallicRoughness) {
            let metallic_roughness = material.sample_metallic_roughness(grad);
            target.metallic = lerp(target.metallic, metallic_roughness.x, weight);
            target.roughness = lerp(target.roughness, metallic_roughness.y, weight);
        }

        if (decal.flags & DecalFlag::AffectEmissive) {
            target.emissive = lerp(target.emissive, material.sample_emissive_color(grad), weight);
        }
    }
}
