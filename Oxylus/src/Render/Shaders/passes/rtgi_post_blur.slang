import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<u32> half_res_face_normal_image;
[[vk::binding(3, 0)]] [[format("r16ui")]] Texture2D<u32> sample_count_image;
[[vk::binding(4, 0)]] Texture2D<f32> ao_guide_image;
[[vk::binding(5, 0)]] Texture2D<f32> perceptual_radiance_image;
[[vk::binding(6, 0)]] Texture2D<f32x4> diffuse_before_image;
[[vk::binding(7, 0)]] Texture2D<f32x2> diffuse2_before_image;
[[vk::binding(8, 0)]] RWTexture2D<f32x4> diffuse_blurred_image;
[[vk::binding(9, 0)]] RWTexture2D<f32x2> diffuse2_blurred_image;

// world position in xyz and linear depth in w for the group's span along the blur axis
groupshared f32x4 gs_position_depth[RTGI_POST_BLUR_LDS_SPAN][RTGI_POST_BLUR_Y];

func post_blur(u32x2 group_thread_id, u32x2 group_id, bool is_vertical, in RTGISettings settings) {
    let half_size = f32x2(settings.half_size);
    let max_index = i32x2(settings.half_size) - 1;

    let along = i32(group_thread_id.x);
    let across = i32(group_thread_id.y);

    // the vertical pass swaps the axes so the 16 wide thread dimension runs along y
    let group_blur_start = i32(select(is_vertical, group_id.y, group_id.x)) * RTGI_POST_BLUR_X;
    let perpendicular = i32(select(is_vertical, group_id.x, group_id.y)) * RTGI_POST_BLUR_Y + across;
    let pixel = select(
        is_vertical,
        i32x2(perpendicular, group_blur_start + along),
        i32x2(group_blur_start + along, perpendicular)
    );

    for (i32 s = along; s < RTGI_POST_BLUR_LDS_SPAN; s += RTGI_POST_BLUR_X) {
        let blur_global = group_blur_start - RTGI_POST_BLUR_LDS_RADIUS + s;
        let texel = clamp(
            select(is_vertical, i32x2(perpendicular, blur_global), i32x2(blur_global, perpendicular)),
            i32x2(0),
            max_index
        );
        let tap_depth = half_res_depth_image.Load(i32x3(texel, 0));
        let tap_uv = (f32x2(texel) + 0.5) / half_size;
        gs_position_depth[s][across] = f32x4(camera.world_position_from_linear_depth(tap_uv, tap_depth), tap_depth);
    }

    GroupMemoryBarrierWithGroupSync();

    if (any(pixel >= i32x2(settings.half_size))) {
        return;
    }

    let center_span = RTGI_POST_BLUR_LDS_RADIUS + along;
    let center = gs_position_depth[center_span][across];
    if (center.w == 0.0) {
        diffuse_blurred_image[pixel] = f32x4(0.0);
        diffuse2_blurred_image[pixel] = f32x2(0.0);
        return;
    }

    let center_load = i32x3(pixel, 0);
    let center_position = center.xyz;
    let center_normal = com::decompress_normal_32(half_res_face_normal_image.Load(center_load));
    let center_uv = (f32x2(pixel) + 0.5) / half_size;
    let pixel_width_ws = rtgi_pixel_width_ws(camera, center_uv, center.w);
    let rcp_pixel_width_ws = 1.0 / pixel_width_ws;

    let pixel_sample_count = rtgi_unpack_normal_count(sample_count_image.Load(center_load));
    let ao_guide = ao_guide_image.Load(center_load);
    let ao_guide_radius_scale = lerp(settings.post_blur_ao_guide_floor, 1.0, ao_guide);
    let filter_width = min(max(1, i32(f32(settings.post_blur_max_width) * ao_guide_radius_scale)), RTGI_POST_BLUR_LDS_RADIUS);

    let pixel_perceptual_radiance = perceptual_radiance_image.Load(center_load);
    let sample_count_ramp = settings.max_temporal_samples * 0.1;
    let guide_ramp = saturate((pixel_sample_count - sample_count_ramp) / max(sample_count_ramp, 1.0));
    let stride = i32(settings.post_blur_stride);

    var weight_accum = 0.0;
    var blurred_accum = f32x4(0.0);
    var blurred_accum2 = f32x2(0.0);
    for (i32 i = -filter_width; i <= filter_width;) {
        let sample_position_depth = gs_position_depth[center_span + i][across];
        let raw_index = pixel + select(is_vertical, i32x2(0, i), i32x2(i, 0));

        // out of bounds taps get no weight rather than over weighting the clamped edge pixel
        let is_out_of_bounds = or(any(raw_index < i32x2(0)), any(raw_index > max_index));
        let sample_index = clamp(raw_index, i32x2(0), max_index);
        let sample_load = i32x3(sample_index, 0);
        let sample_normal = com::decompress_normal_32(half_res_face_normal_image.Load(sample_load));
        let sample_sh_y = diffuse_before_image.Load(sample_load);
        let sample_co_cg = diffuse2_before_image.Load(sample_load);

        let geometric_weight = rtgi_calc_similar_surface_weight(
            rcp_pixel_width_ws, center_position, center_normal, sample_position_depth.xyz, sample_normal
        );
        let normal_weight = rtgi_calc_similar_normal_weight(center_normal, sample_normal);
        let gauss_weight = com::gaussian_weight(f32(abs(i)) / f32(filter_width));

        // the taps within 2 pixels always run at stride 1 to hide the quad structure of the pre-filter
        let is_near_center = abs(i) <= 2;
        i = select(i < -2, min(i + stride, -2), select(i < 2, i + 1, i + stride));

        let sample_perceptual_radiance = perceptual_radiance_image.Load(sample_load);
        let raw_weight = rtgi_calc_perceptual_difference_weight(
            pixel_perceptual_radiance, sample_perceptual_radiance, settings.post_blur_perceptual_radiance_guide_tolerance
        );
        let extra_weight = select(is_near_center, 1.0, lerp(1.0, raw_weight, guide_ramp));

        let is_valid = and(!is_out_of_bounds, sample_position_depth.w != 0.0);
        let weight = select(is_valid, geometric_weight * normal_weight * gauss_weight * extra_weight, 0.0);
        weight_accum += weight;
        blurred_accum += weight * sample_sh_y;
        blurred_accum2 += weight * sample_co_cg;
    }

    // pixels that find almost no usable taps fall back to their own value
    let low_weight_fallback_blend = max(0.0, 1.0 - weight_accum / (f32(filter_width) / 4.0));
    let blurry_sh_y = blurred_accum / (weight_accum + 0.0001);
    let blurry_co_cg = blurred_accum2 / (weight_accum + 0.0001);
    let pixel_sh_y = diffuse_before_image.Load(center_load);
    let pixel_co_cg = diffuse2_before_image.Load(center_load);

    diffuse_blurred_image[pixel] = lerp(blurry_sh_y, pixel_sh_y, low_weight_fallback_blend);
    diffuse2_blurred_image[pixel] = lerp(blurry_co_cg, pixel_co_cg, low_weight_fallback_blend);
}

[[shader("compute")]]
[numthreads(16, 4, 1)]
func cs_horizontal(u32x2 group_thread_id : SV_GroupThreadID, u32x2 group_id : SV_GroupID, uniform RTGISettings settings) {
    post_blur(group_thread_id, group_id, false, settings);
}

[[shader("compute")]]
[numthreads(16, 4, 1)]
func cs_vertical(u32x2 group_thread_id : SV_GroupThreadID, u32x2 group_id : SV_GroupID, uniform RTGISettings settings) {
    post_blur(group_thread_id, group_id, true, settings);
}
