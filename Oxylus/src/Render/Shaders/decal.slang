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

func apply_decal(
    inout DecalTarget target,
    in Decal decal,
    StructuredBuffer<Material> materials,
    f32x3 world_position,
    f32x3 position_dx,
    f32x3 position_dy,
    f32x3 geometric_normal,
    f32 mip_bias
) {
    let local = mul(decal.world_to_decal, f32x4(world_position, 1.0)).xyz;
    if (any(abs(local) > 0.5)) {
        return;
    }

    let facing = dot(geometric_normal, decal.world_axis_z);
    let angle_range = max(decal.angle_fade_start_cos - decal.angle_fade_end_cos, 1e-4);
    let angle_fade = saturate((facing - decal.angle_fade_end_cos) / angle_range);
    if (angle_fade <= 0.0) {
        return;
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
        return;
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

// position_dx and position_dy are the world space footprint of one pixel. decal_tiles holds DECAL_TILE_WORDS
// words per DECAL_TILE_SIZE tile, a bit per decal that may touch it
public func apply_decals(
    inout DecalTarget target,
    Decal *decals,
    u32 *decal_tiles,
    u32 decal_count,
    u32x2 pixel,
    f32x2 resolution,
    StructuredBuffer<Material> materials,
    f32x3 world_position,
    f32x3 position_dx,
    f32x3 position_dy,
    f32x3 geometric_normal,
    u32 receiver_layer_mask,
    f32 mip_bias
) {
    if (decal_count == 0) {
        return;
    }

    let tile = pixel / DECAL_TILE_SIZE;
    let tile_count_x = (u32(resolution.x) + DECAL_TILE_SIZE - 1u) / DECAL_TILE_SIZE;
    let tile_words = decal_tiles + (tile.y * tile_count_x + tile.x) * DECAL_TILE_WORDS;
    let word_count = (min(decal_count, MAX_DECALS) + 31u) / 32u;
    [loop]
    for (u32 word = 0; word < word_count; word++) {
        let lane_word = tile_words[word];
        // walked as a wave so every lane loads the same decal, lanes skip the bits their own tile lacks
        var wave_bits = WaveActiveBitOr(lane_word);
        while (wave_bits != 0u) {
            let bit = firstbitlow(wave_bits);
            wave_bits &= wave_bits - 1u;
            if (((lane_word >> bit) & 1u) == 0u) {
                continue;
            }

            let decal = decals[word * 32u + bit];
            if ((decal.layer_mask & receiver_layer_mask) == 0) {
                continue;
            }

            apply_decal(target, decal, materials, world_position, position_dx, position_dy, geometric_normal, mip_bias);
        }
    }
}
