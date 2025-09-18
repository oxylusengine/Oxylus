module shadows;

import common;
import gpu;
import scene;

// SYBAU lsp 💔
#ifndef MAX_DIRECTIONAL_SHADOW_CASCADES
#define MAX_DIRECTIONAL_SHADOW_CASCADES 4
#endif

static const f32x2 POISSON_DISK[16] = {
    f32x2(-0.94201624, -0.39906216),
    f32x2(0.94558609, -0.76890725),
    f32x2(-0.094184101, -0.92938870),
    f32x2(0.34495938, 0.29387760),
    f32x2(-0.91588581, 0.45771432),
    f32x2(-0.81544232, -0.87912464),
    f32x2(-0.38277543, 0.27676845),
    f32x2(0.97484398, 0.75648379),
    f32x2(0.44323325, -0.97511554),
    f32x2(0.53742981, -0.47373420),
    f32x2(-0.26496911, -0.41893023),
    f32x2(0.79197514, 0.19090188),
    f32x2(-0.24188840, 0.99706507),
    f32x2(-0.81409955, 0.91437590),
    f32x2(0.19984126, 0.78641367),
    f32x2(0.14383161, -0.14100790)
};

f32 poisson_disk_shadow(Texture2DArray<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, u32 cascade_index, f32 bias, f32 filter_radius) {
    f32 shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        f32x2 offset = POISSON_DISK[i] * filter_radius;
        f32x3 sample_coord = f32x3(shadow_coord.xy + offset, f32(cascade_index));
        f32 shadow_depth = shadow_map.SampleLevel(shadow_sampler, sample_coord, 0).r;
        shadow += (shadow_coord.z + bias) < shadow_depth ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}

f32 pcf_shadow(Texture2DArray<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, u32 cascade_index, f32 bias, f32 texel_size) {
    f32 shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            f32x2 offset = f32x2(x, y) * texel_size;
            f32x3 sample_coord = f32x3(shadow_coord.xy + offset, f32(cascade_index));
            f32 shadow_depth = shadow_map.SampleLevel(shadow_sampler, sample_coord, 0).r;
            shadow += (shadow_coord.z + bias) < shadow_depth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

f32 find_blocker_distance(Texture2DArray<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, u32 cascade_index, f32 light_size, f32 bias) {
    f32 search_radius = light_size * (shadow_coord.z - 0.1) / shadow_coord.z;
    f32 blocker_sum = 0.0;
    f32 num_blockers = 0.0;

    for (int i = 0; i < 16; ++i) {
        f32x2 offset = POISSON_DISK[i] * search_radius;
        f32x3 sample_coord = f32x3(shadow_coord.xy + offset, f32(cascade_index));
        f32 shadow_depth = shadow_map.SampleLevel(shadow_sampler, sample_coord, 0).r;

        if ((shadow_coord.z + bias) > shadow_depth) {
            blocker_sum += shadow_depth;
            num_blockers += 1.0;
        }
    }

    if (num_blockers == 0.0) return -1.0;
    return blocker_sum / num_blockers;
}

f32 contact_hardening_shadow(Texture2DArray<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, u32 cascade_index,
                           f32 light_size, f32 bias, f32x3 world_pos, f32x3 light_pos) {
    f32 light_distance = length(light_pos - world_pos);

    f32 avg_blocker_depth = find_blocker_distance(shadow_map, shadow_sampler, shadow_coord, cascade_index, light_size * 0.1, bias);

    if (avg_blocker_depth < 0.0) return 0.0;

    f32 blocker_world_distance = avg_blocker_depth * light_distance;
    f32 receiver_world_distance = shadow_coord.z * light_distance;

    f32 penumbra_size = light_size * (receiver_world_distance - blocker_world_distance) / blocker_world_distance;
    f32 filter_radius = penumbra_size / light_distance;

    filter_radius = clamp(filter_radius, 0.001, 0.05);

    return poisson_disk_shadow(shadow_map, shadow_sampler, shadow_coord, cascade_index, bias, filter_radius);
}

func get_shadow_sample_position(
    in DirectionalLight light,
    u32 cascade_index,
    f32x4 position_offset
) -> f32x4 {
    let cascade = light.cascades[cascade_index];
    var shadow_offset = mul(cascade.projection_view, position_offset);
    if (shadow_offset.w <= 0.0) {
        return 0.0;
    }

    let shadow_offset_ndc = shadow_offset.xyz / shadow_offset.w;
    if (any(shadow_offset_ndc.xy < -1.0) || any(shadow_offset_ndc > 1.0) || shadow_offset_ndc.z < 0.0) {
        return 0.0;
    }

    return f32x4(shadow_offset_ndc.xy * 0.5 + 0.5, shadow_offset_ndc.z, 1.0);
}

func sample_directional_cascade(
    in DirectionalLight light,
    Texture2DArray<f32> shadow_map,
    SamplerComparisonState shadow_sampler,
    u32 cascade_index,
    f32x4 pixel_position,
    f32x3 normal
) -> f32 {
    let normal_offset = light.normal_bias * light.texel_size * normal;
    let depth_offset = light.depth_bias * light.direction;
    let position_offset = f32x4(pixel_position.xyz + normal_offset + depth_offset, pixel_position.w);
    let light_sample_position = get_shadow_sample_position(light, cascade_index, position_offset);
    if (light_sample_position.w == 0.0) {
        return 1.0;
    }

    return shadow_map.SampleCmpLevelZero(shadow_sampler, f32x3(light_sample_position.xy, f32(cascade_index)), light_sample_position.z);
}

public func get_cascade_index(in DirectionalLight light, f32 view_z) -> u32 {
    for (u32 i = 0; i < light.cascade_count; i++) {
        if (-view_z < light.cascades[i].far_bound) {
            return i;
        }
    }

    return light.cascade_count;
}

public func sample_directional_light(
    in DirectionalLight light,
    Texture2DArray<f32> shadow_map,
    SamplerComparisonState shadow_sampler,
    f32x4 pixel_position,
    f32x3 normal,
    f32 view_z
) -> f32 {
    let cascade_index = get_cascade_index(light, view_z);
    if (cascade_index >= light.cascade_count) {
        return 1.0;
    }

    var shadow = sample_directional_cascade(light, shadow_map, shadow_sampler, cascade_index, pixel_position, normal);
    // Cascade blending
    let next_cascade_index = cascade_index + 1u;
    if (next_cascade_index < light.cascade_count) {
        let this_far_bound = light.cascades[cascade_index].far_bound;
        let next_near_bound = (1.0 - light.cascades_overlap_proportion) * this_far_bound;
        if (-view_z >= next_near_bound) {
            let next_shadow = sample_directional_cascade(light, shadow_map, shadow_sampler, next_cascade_index, pixel_position, normal);
            shadow = lerp(shadow, next_shadow, (-view_z - next_near_bound) / (this_far_bound - next_near_bound));
        }
    }

    return shadow;
}
