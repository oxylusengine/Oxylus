// The "pioneer" idea is from Timberdoodle (https://github.com/Sunset-Flock/Timberdoodle) (Apache-2.0)
import common;
import gpu;
import scene;

// 24 bit octahedral normal in the low bits, 8 bit roughness on top
public func rtgi_pack_normal_roughness(f32x3 normal, f32 roughness) -> u32 {
    return (com::compress_normal_24(normal) & 0xffffffu) | (u32(round(saturate(roughness) * 255.0)) << 24u);
}

public func rtgi_unpack_normal(u32 packed) -> f32x3 {
    return com::decompress_normal_24(packed & 0xffffffu);
}

public func rtgi_unpack_roughness(u32 packed) -> f32 {
    return f32(packed >> 24u) * (1.0 / 255.0);
}

// picks the most central non sky depth of a 2x2 quad, index is x | (y << 1), depth 0 is sky
public func rtgi_quad_downsample_representative(f32x4 depths) -> i32 {
    let surface_depths = select(depths != 0.0, depths, f32x4(1e30));
    let min_depth = min(min(surface_depths.x, surface_depths.y), min(surface_depths.z, surface_depths.w));

    // keeps near outliers from winning only when the quad straddles a large depth gap
    let foreground_bias = 0.25;

    f32 depth_dist[4];
    for (i32 i = 0; i < 4; i++) {
        var dist = foreground_bias * (depths[i] - min_depth);
        for (i32 j = 0; j < 4; j++) {
            dist += select(depths[j] != 0.0, abs(depths[i] - depths[j]), 0.0);
        }

        depth_dist[i] = select(depths[i] != 0.0, dist, 1e30);
    }

    // distances inside this band count as equal so flat surfaces fall through to the top then left tie break
    let repr_tol = 0.02 * min_depth;

    i32 rep = 0;
    for (i32 i = 1; i < 4; i++) {
        let is_better = or(depths[rep] == 0.0, depth_dist[i] < depth_dist[rep] - repr_tol);
        rep = select(and(depths[i] != 0.0, is_better), i, rep);
    }

    return rep;
}

// reverse-z with a finite far plane, view z does not depend on xy so the uv is not needed
// world space size of one half res pixel at the given linear depth
public func rtgi_pixel_width_ws(in Camera camera, f32x2 uv, f32 linear_depth) -> f32 {
    let center = camera.view_position_from_linear_depth(uv, linear_depth);
    let below = camera.view_position_from_linear_depth(uv + f32x2(0.0, 2.0) / camera.resolution, linear_depth);
    return abs(below.y - center.y);
}

static const f32 RTGI_RAY_START_OFFSET = 0.001;

public static const f32 RTGI_PIONEER_MAX_DISTANCE = 256.0;

// guide window in half res pixels, the tap count in pioneer cells is window / grid div
public static const u32 RTGI_GUIDE_RESAMPLE_WINDOW = 64u;
public static const i32 RTGI_GUIDE_RESAMPLE_STRIDE = 2;
public static const f32 RTGI_GUIDE_RESAMPLE_PX_DIST_THRESHOLD = 32.0;
public static const f32 RTGI_GUIDE_IMPORTANCE_FLOOR = 1.0e-6;

public func rtgi_pioneer_grid_size(u32x2 half_size) -> u32x2 {
    return (half_size + (RTGI_PIONEER_GRID_DIV - 1u)) / RTGI_PIONEER_GRID_DIV;
}

public func rtgi_pioneer_rotation(u32 frame_index) -> u32x2 {
    let rotation_frame = frame_index & 0xfffu;
    return u32x2(
        rotation_frame % RTGI_PIONEER_GRID_DIV,
        (rotation_frame / RTGI_PIONEER_GRID_DIV) % RTGI_PIONEER_GRID_DIV
    );
}

// the pioneer trace, both resample passes and the guide fetch must agree on which half res pixel a cell stands for
public func rtgi_pioneer_pixel(u32x2 cell, u32 frame_index) -> u32x2 {
    return cell * RTGI_PIONEER_GRID_DIV + rtgi_pioneer_rotation(frame_index);
}

public func rtgi_calc_ray_start(f32x3 position, f32x3 face_normal, f32x3 view_ray) -> f32x3 {
    return position + (face_normal - view_ray * 2.0) * RTGI_RAY_START_OFFSET;
}

// the post blur tile, 16 along the blur axis and 4 across, the numthreads of the post blur entries must match
public static const i32 RTGI_POST_BLUR_X = 16;
public static const i32 RTGI_POST_BLUR_Y = 4;
public static const i32 RTGI_POST_BLUR_LDS_RADIUS = 16;
public static const i32 RTGI_POST_BLUR_LDS_SPAN = RTGI_POST_BLUR_X + RTGI_POST_BLUR_LDS_RADIUS * 2;

// packed history counters in one r16 texel, normal count in the low 10 bits and fast history frames in the high 6
public static const u32 RTGI_COUNT_NORMAL_MASK = 0x3ffu;
public static const u32 RTGI_COUNT_FAST_MASK = 0x3fu;
public static const u32 RTGI_COUNT_SKY = 0x3ffu;
public static const u32 RTGI_COUNT_COARSE_CODE = 512u;
public static const f32 RTGI_COUNT_COARSE_BASE = 128.0;
public static const f32 RTGI_COUNT_NORMAL_MAX = 1148.0;

public func rtgi_pack_sample_counts(f32 normal_count, f32 fast_count) -> u32 {
    let clamped = clamp(normal_count, 0.0, RTGI_COUNT_NORMAL_MAX);
    let normal_code = select(
        clamped < RTGI_COUNT_COARSE_BASE - 0.125,
        u32(round(clamped * 4.0)),
        RTGI_COUNT_COARSE_CODE + u32(round(max(clamped - RTGI_COUNT_COARSE_BASE, 0.0) * 0.5))
    );
    let fast_code = u32(round(clamp(fast_count, 0.0, 15.0) * 4.0));
    return (normal_code & RTGI_COUNT_NORMAL_MASK) | ((fast_code & RTGI_COUNT_FAST_MASK) << 10u);
}

public func rtgi_pack_sample_counts_sky() -> u32 {
    return RTGI_COUNT_SKY;
}

public func rtgi_unpack_normal_count(u32 packed) -> f32 {
    let code = packed & RTGI_COUNT_NORMAL_MASK;
    let count = select(
        code < RTGI_COUNT_COARSE_CODE,
        f32(code) * 0.25,
        RTGI_COUNT_COARSE_BASE + (f32(code) - f32(RTGI_COUNT_COARSE_CODE)) * 2.0
    );
    return select(code == RTGI_COUNT_SKY, -1.0, count);
}

public func rtgi_unpack_fast_count(u32 packed) -> f32 {
    return f32((packed >> 10u) & RTGI_COUNT_FAST_MASK) * 0.25;
}

// history is only capped in time, above max_temporal_samples it decays by 1 / max_temporal_samples per frame
public func rtgi_accumulate_sample_count(f32 carry, f32 rays, f32 max_temporal_samples) -> f32 {
    let grown = carry + rays;
    let leaked = carry * (1.0 - 1.0 / max(max_temporal_samples, 1.0)) + rays;
    return min(min(grown, max(max_temporal_samples, leaked)), RTGI_COUNT_NORMAL_MAX);
}

// floor of 1e-3 in display units, radiance is not pre exposed so it is scale * 1e-3 / exposure
public func rtgi_perceptual_radiance_floor(f32 exposure) -> f32 {
    return RTGI_RADIANCE_SCALE * 1.0e-3 / exposure;
}

public func rtgi_current_exposure(in RTGISettings settings, f32 buffered_exposure) -> f32 {
    let exposure = select(settings.is_eye_adapted != 0, buffered_exposure, settings.fixed_exposure);
    return select(and(isfinite(exposure), exposure > 0.0), exposure, 1.0);
}

public func rtgi_linear_to_perceptual(f32 value, f32 exposure) -> f32 {
    return log(max(value, rtgi_perceptual_radiance_floor(exposure)));
}

public func rtgi_linear_to_perceptual_rgb(f32x3 value, f32 exposure) -> f32x3 {
    return log(max(value, rtgi_perceptual_radiance_floor(exposure)));
}

public func rtgi_perceptual_radiance_from_rgb(f32x3 perceptual_rgb) -> f32 {
    return dot(perceptual_rgb, f32x3(0.25, 0.5, 0.25));
}

public func rtgi_calc_perceptual_difference_weight(f32 a_perceptual, f32 b_perceptual, f32 tolerance) -> f32 {
    let difference = a_perceptual - b_perceptual;
    return exp(-com::square(2.0 * difference / tolerance));
}

// 1 for a coincident hit, 0 at max_visibility_pixel_range pixel widths
public func rtgi_calc_ray_shortness(f32 ray_length, f32 pixel_width_ws, f32 max_visibility_pixel_range) -> f32 {
    let max_visibility_ray_length = pixel_width_ws * max_visibility_pixel_range;
    return 1.0 - min(1.0, com::square(ray_length / max(max_visibility_ray_length, 1.0e-8)));
}

public func rtgi_calc_similar_surface_weight(
    f32 rcp_pixel_width_ws,
    f32x3 a_position,
    f32x3 a_normal,
    f32x3 b_position,
    f32x3 b_normal,
    f32 px_threshold = 1.2
) -> f32 {
    let within_plane_distance_a = 1.0 - saturate(
        com::plane_distance(a_position, a_normal, b_position) / px_threshold * rcp_pixel_width_ws
    );
    let within_plane_distance_b = 1.0 - saturate(
        com::plane_distance(b_position, b_normal, a_position) / px_threshold * rcp_pixel_width_ws
    );
    return within_plane_distance_a * within_plane_distance_b;
}

public func rtgi_calc_similar_surface_weight_dist_limited(
    f32 rcp_pixel_width_ws,
    f32x3 a_position,
    f32x3 a_normal,
    f32x3 b_position,
    f32x3 b_normal,
    f32 px_threshold = 1.2
) -> f32 {
    let surface_weight = rtgi_calc_similar_surface_weight(
        rcp_pixel_width_ws, a_position, a_normal, b_position, b_normal, px_threshold
    );
    let distance_in_pixel_widths = abs(distance(a_position, b_position)) * rcp_pixel_width_ws;
    return step(distance_in_pixel_widths, 32.0 * px_threshold) * surface_weight;
}

// at half resolution normals are only a strong suggestion, not a cutoff
public func rtgi_calc_similar_normal_weight(f32x3 normal, f32x3 other_normal) -> f32 {
    let validity = max(0.1, dot(normal, other_normal) + 0.85) * (1.0 / 1.85);
    return com::square(validity);
}

public func rtgi_y_co_cg_to_linear_corrected(f32 y, f32 sh_y_0, f32x2 co_cg) -> f32x3 {
    let clamped_y = max(y, 0.0);
    let scaled_co_cg = co_cg * ((clamped_y + 1.0e-6) / (sh_y_0 + 1.0e-6));
    return max(com::ycocg_to_rgb(f32x3(clamped_y, scaled_co_cg)), 0.0);
}

public func rtgi_sh_resolve_diffuse(f32x4 sh_y, f32x2 co_cg, f32x3 normal) -> f32x3 {
    let y = max(dot(normal, sh_y.xyz) + 0.5 * sh_y.w, sh_y.w * 0.1);
    return rtgi_y_co_cg_to_linear_corrected(y, sh_y.w, co_cg);
}

public func rtgi_radiance_to_y_co_cg_sh(f32x3 radiance, f32x3 direction, out f32x4 sh_y, out f32x2 co_cg) {
    let y_co_cg = com::rgb_to_ycocg(radiance);
    co_cg = y_co_cg.yz;
    sh_y = f32x4(direction * y_co_cg.x, y_co_cg.x);
}

// a grazing surface that turns face on stretches its thin history strip over many new pixels, ramps in past 2x
public func rtgi_calc_parallax_penalty(
    f32x3 world_position,
    f32x3 normal,
    f32x3 camera_position,
    f32x3 previous_camera_position,
    f32 strength
) -> f32 {
    let view_current = normalize(world_position - camera_position);
    let view_previous = normalize(world_position - previous_camera_position);
    let graze_current = max(abs(dot(view_current, normal)), 1.0e-3);
    let graze_previous = max(abs(dot(view_previous, normal)), 1.0e-3);
    let stretch = graze_current / graze_previous;
    return saturate((log2(max(stretch, 1.0)) - 1.0) * strength);
}

// mean of the valid taps, reduced only by the coverage of failed taps lying in front of the expected point
public func rtgi_reproject_count_occluder_aware(f32x4 counts, f32x4 w, f32 occluder_coverage) -> f32 {
    let valid = select(w > 0.1, counts, f32x4(0.0));
    let max_valid = max(max(valid.x, valid.y), max(valid.z, valid.w));

    let weight_sum = dot(w, f32x4(1.0));
    let normalized = select(weight_sum > 1.0e-6, dot(counts, w) / max(weight_sum, 1.0e-6), 0.0);
    let penalty = pow(saturate(1.0 - occluder_coverage), 1.0 - 0.66);
    return max(min(max_valid, 4.0), normalized * penalty);
}

// rays per frame, never exceeded, the distribute pass drains demand to it and the trace never reads past it
public func rtgi_hard_ray_budget(u32x2 half_size, in RTGISettings settings) -> u32 {
    let ray_percentage = clamp(settings.ray_percentage, 0.0, f32(RTGI_RAY_LIST_CAPACITY_MUL));
    let min_budget = clamp(settings.min_ray_budget, 0.0, 1.0);
    return u32(f32(half_size.x * half_size.y) * max(ray_percentage, min_budget));
}

public func rtgi_ray_list_capacity(u32x2 half_size) -> u32 {
    return half_size.x * half_size.y * RTGI_RAY_LIST_CAPACITY_MUL;
}

public func rtgi_ray_list_limit(u32x2 half_size, in RTGISettings settings) -> u32 {
    return min(rtgi_ray_list_capacity(half_size), rtgi_hard_ray_budget(half_size, settings));
}

// extra rays above the base ray a pixel wants, exp(-4x) over the convergence target, amplitude extras at x = 0
public func rtgi_calc_desired_extra_rays(f32 sample_count, f32 target, f32 amplitude) -> f32 {
    let x = max(sample_count, 0.0) / max(target, 1.0e-6);
    return select(or(sample_count >= target, target <= 0.0), 0.0, amplitude * exp(-4.0 * x));
}

// factors live in [0, 2], stored linearly as factor / 2 in an unorm texel
public func rtgi_pack_ray_impact_factor(f32 factor) -> f32 {
    return saturate(factor * 0.5);
}

public func rtgi_unpack_ray_impact_factor(f32 packed) -> f32 {
    return packed * 2.0;
}

// rays one pixel requests this frame, the base ray plus extras from the history deficit, capped by max_rays_per_pixel
public func rtgi_calc_ray_demand(f32 sample_count, f32 impact_diffuse, in RTGISettings settings) -> u32 {
    let max_rays = max(settings.max_rays_per_pixel, 1u);
    let extra_cap = f32(max_rays - 1u);
    let diffuse_extra = rtgi_calc_desired_extra_rays(sample_count, settings.fast_convergence_samples, extra_cap) * impact_diffuse;
    let extra_scale = min(1.0, extra_cap / max(diffuse_extra, 1.0e-6));
    let demand = 1u + u32(diffuse_extra * extra_scale + 0.5);
    return select(demand > max_rays, demand - 1u, demand);
}
