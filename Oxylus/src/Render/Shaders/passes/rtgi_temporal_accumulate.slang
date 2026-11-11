import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] [[format("r16ui")]] RWTexture2D<u32> sample_count_image;
[[vk::binding(1, 0)]] [[format("r16ui")]] Texture2D<u32> previous_sample_count_image;
[[vk::binding(2, 0)]] [[format("r8ui")]] Texture2D<u32> ray_count_image;
[[vk::binding(3, 0)]] [[format("rg16ui")]] Texture2D<u32x2> reproject_corner_image;
[[vk::binding(4, 0)]] Texture2D<f32x4> reproject_weights_image;
[[vk::binding(5, 0)]] Texture2D<f32x4> pre_filtered_diffuse_image;
[[vk::binding(6, 0)]] Texture2D<f32x2> pre_filtered_diffuse2_image;
[[vk::binding(7, 0)]] Texture2D<f32> ao_guide_image;
[[vk::binding(8, 0)]] Texture2D<f32> perceptual_radiance_image;
[[vk::binding(9, 0)]] Texture2D<f32x4> previous_diffuse_image;
[[vk::binding(10, 0)]] Texture2D<f32x2> previous_diffuse2_image;
[[vk::binding(11, 0)]] Texture2D<f32x2> previous_fast_history_image;
[[vk::binding(12, 0)]] Texture2D<f32> previous_ao_guide_image;
[[vk::binding(13, 0)]] Texture2D<f32> previous_perceptual_radiance_image;
[[vk::binding(14, 0)]] RWTexture2D<f32x4> diffuse_image;
[[vk::binding(15, 0)]] RWTexture2D<f32x2> diffuse2_image;
[[vk::binding(16, 0)]] RWTexture2D<f32x2> fast_history_image;
[[vk::binding(17, 0)]] RWTexture2D<f32> ao_guide_history_image;
[[vk::binding(18, 0)]] RWTexture2D<f32> perceptual_radiance_history_image;

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(u32x2 half_pixel : SV_DispatchThreadID, uniform RTGISettings settings) {
    if (any(half_pixel >= settings.half_size)) {
        return;
    }

    let load = u32x3(half_pixel, 0u);
    let packed_carry = sample_count_image[half_pixel];
    let carry_sample_count = rtgi_unpack_normal_count(packed_carry);
    if (carry_sample_count < 0.0) {
        diffuse_image[half_pixel] = f32x4(0.0);
        diffuse2_image[half_pixel] = f32x2(0.0);
        fast_history_image[half_pixel] = f32x2(0.0);
        ao_guide_history_image[half_pixel] = 0.0;
        perceptual_radiance_history_image[half_pixel] = 0.0;
        return;
    }

    // the reproject pass stashed the parallax penalty in the fast field
    let parallax_penalty = rtgi_unpack_fast_count(packed_carry) * (1.0 / 15.0);
    let rays_shot = f32(ray_count_image.Load(load));
    let accumulated_sample_count = rtgi_accumulate_sample_count(carry_sample_count, rays_shot, settings.max_temporal_samples);

    let origin = i32x2(reproject_corner_image.Load(load)) - 1;
    let sample_weights = reproject_weights_image.Load(load);
    let max_texel = i32x2(settings.half_size) - 1;
    let is_geometry_disocclusion = dot(sample_weights, f32x4(1.0)) < 0.025;
    let is_disocclusion = is_geometry_disocclusion;

    u32x3 taps[4];
    for (u32 t = 0u; t < 4u; t++) {
        let offset = i32x2(i32(t & 1u), i32(t >> 1u));
        taps[t] = u32x3(u32x2(clamp(origin + offset, i32x2(0), max_texel)), 0u);
    }

    let bilinear_diffuse = com::apply_bilinear_weights(
        previous_diffuse_image.Load(taps[0]),
        previous_diffuse_image.Load(taps[1]),
        previous_diffuse_image.Load(taps[2]),
        previous_diffuse_image.Load(taps[3]),
        sample_weights
    );
    let reprojected_diffuse = select(any(isnan(bilinear_diffuse)), f32x4(0.0), bilinear_diffuse);

    let bilinear_diffuse2 = com::apply_bilinear_weights(
        previous_diffuse2_image.Load(taps[0]),
        previous_diffuse2_image.Load(taps[1]),
        previous_diffuse2_image.Load(taps[2]),
        previous_diffuse2_image.Load(taps[3]),
        sample_weights
    );
    let reprojected_diffuse2 = select(any(isnan(bilinear_diffuse2)), f32x2(0.0), bilinear_diffuse2);

    let bilinear_fast = com::apply_bilinear_weights(
        previous_fast_history_image.Load(taps[0]),
        previous_fast_history_image.Load(taps[1]),
        previous_fast_history_image.Load(taps[2]),
        previous_fast_history_image.Load(taps[3]),
        sample_weights
    );
    let reprojected_fast = select(any(isnan(bilinear_fast)), f32x2(0.0), bilinear_fast);

    let bilinear_ao_guide = com::apply_bilinear_weights(
        previous_ao_guide_image.Load(taps[0]),
        previous_ao_guide_image.Load(taps[1]),
        previous_ao_guide_image.Load(taps[2]),
        previous_ao_guide_image.Load(taps[3]),
        sample_weights
    );
    let reprojected_ao_guide = select(isnan(bilinear_ao_guide), 0.0, bilinear_ao_guide);

    let bilinear_perceptual_radiance = com::apply_bilinear_weights(
        previous_perceptual_radiance_image.Load(taps[0]),
        previous_perceptual_radiance_image.Load(taps[1]),
        previous_perceptual_radiance_image.Load(taps[2]),
        previous_perceptual_radiance_image.Load(taps[3]),
        sample_weights
    );
    let reprojected_perceptual_radiance = select(isnan(bilinear_perceptual_radiance), 0.0, bilinear_perceptual_radiance);

    let bilinear_fast_frames = com::apply_bilinear_weights(
        rtgi_unpack_fast_count(previous_sample_count_image.Load(taps[0])),
        rtgi_unpack_fast_count(previous_sample_count_image.Load(taps[1])),
        rtgi_unpack_fast_count(previous_sample_count_image.Load(taps[2])),
        rtgi_unpack_fast_count(previous_sample_count_image.Load(taps[3])),
        sample_weights
    );
    let reprojected_fast_frames = select(isnan(bilinear_fast_frames), 0.0, bilinear_fast_frames);

    // fast history age counts frames, not ray samples, a stretched pixel resets it with the same penalty
    let accumulated_fast_frames = select(is_disocclusion, 0.0, (reprojected_fast_frames + 1.0) * (1.0 - parallax_penalty));

    let new_diffuse = pre_filtered_diffuse_image.Load(load);
    let new_diffuse2 = pre_filtered_diffuse2_image.Load(load);
    let new_ao_guide = ao_guide_image.Load(load);

    let fast_history_frames = clamp(f32(settings.temporal_fast_history_frames), 1.0, 15.0);
    let fast_blend_factor = 1.0 / (1.0 + min(accumulated_fast_frames, fast_history_frames));
    let reprojected_fast_mean = reprojected_fast.x;
    let reprojected_fast_variance = reprojected_fast.y;

    // temporal firefly clamp on the fast brightness only, skipped on disocclusion where there is no valid mean
    let is_fast_clamping = and(!is_disocclusion, accumulated_fast_frames > fast_history_frames);
    let brightness_ratio = reprojected_fast_mean * (1.0 + sqrt(reprojected_fast_variance) * settings.temporal_firefly_std_dev_clamp) / max(new_diffuse.w, 1.0e-8);
    let new_fast_brightness = new_diffuse.w * select(is_fast_clamping, min(1.0, brightness_ratio), 1.0);

    let accumulated_fast_mean = select(is_disocclusion, new_fast_brightness, lerp(reprojected_fast_mean, new_fast_brightness, fast_blend_factor));
    let old_mean_safe = max(reprojected_fast_mean, 1.0e-6);
    let new_relative_variance_point = min(com::square((new_fast_brightness - reprojected_fast_mean) / old_mean_safe), 4.0);
    let accumulated_fast_relative_variance = select(is_disocclusion, 0.0, lerp(reprojected_fast_variance, new_relative_variance_point, fast_blend_factor));

    let wrong_point_estimate = min(com::square(abs(accumulated_fast_mean - new_fast_brightness) / max(accumulated_fast_mean, 1.0e-6)), 4.0);
    let adaptation_relative_variance = lerp(reprojected_fast_variance, wrong_point_estimate, fast_blend_factor);
    let fast_variance_scaling = com::square(1.0 + sqrt(adaptation_relative_variance) * settings.temporal_variance_fast_history_blend);

    let slow_history_mean = reprojected_diffuse.w;
    let slow_to_fast_mean_ratio = max(slow_history_mean, accumulated_fast_mean) / (min(slow_history_mean, accumulated_fast_mean) + 0.00000001);
    let fast_mean_diff_scaling = com::square(1.0 / max(1.0, slow_to_fast_mean_ratio));

    // no ray this frame and valid history, the reprojected history passes straight through
    if (and(rays_shot == 0.0, !is_geometry_disocclusion)) {
        sample_count_image[half_pixel] = rtgi_pack_sample_counts(accumulated_sample_count, reprojected_fast_frames * (1.0 - parallax_penalty));
        diffuse_image[half_pixel] = reprojected_diffuse;
        diffuse2_image[half_pixel] = reprojected_diffuse2;
        fast_history_image[half_pixel] = reprojected_fast;
        ao_guide_history_image[half_pixel] = reprojected_ao_guide;
        perceptual_radiance_history_image[half_pixel] = reprojected_perceptual_radiance;
        return;
    }

    let fast_history_based_confidence = accumulated_sample_count * fast_variance_scaling * fast_mean_diff_scaling;
    let history_confidence = select(
        accumulated_sample_count > fast_history_frames,
        min(accumulated_sample_count, fast_history_based_confidence),
        accumulated_sample_count
    );

    // a batch of rays_shot samples joins the history, so the new mean weighs rays_shot / (1 + history)
    let blend = min(1.0, rays_shot / (1.0 + history_confidence));

    let accumulated_diffuse = select(is_disocclusion, new_diffuse, lerp(reprojected_diffuse, new_diffuse, blend));
    let accumulated_diffuse2 = select(is_disocclusion, new_diffuse2, lerp(reprojected_diffuse2, new_diffuse2, blend));

    // guides ramp on the fast history first, then follow the slow color blend once the fast window is full
    let guide_blend = select(accumulated_fast_frames < fast_history_frames, fast_blend_factor, blend);
    let accumulated_ao_guide = select(is_geometry_disocclusion, new_ao_guide, lerp(reprojected_ao_guide, new_ao_guide, guide_blend));

    let raw_perceptual_radiance = perceptual_radiance_image.Load(load);
    let is_new_perceptual_radiance_invalid = isinf(raw_perceptual_radiance);
    let new_perceptual_radiance = select(is_new_perceptual_radiance_invalid, 0.0, raw_perceptual_radiance);

    let accumulated_perceptual_radiance = select(
        is_geometry_disocclusion,
        new_perceptual_radiance,
        lerp(reprojected_perceptual_radiance, new_perceptual_radiance, select(is_new_perceptual_radiance_invalid, 0.0, guide_blend))
    );

    sample_count_image[half_pixel] = rtgi_pack_sample_counts(accumulated_sample_count, accumulated_fast_frames);
    diffuse_image[half_pixel] = accumulated_diffuse;
    diffuse2_image[half_pixel] = accumulated_diffuse2;
    fast_history_image[half_pixel] = f32x2(accumulated_fast_mean, accumulated_fast_relative_variance);
    ao_guide_history_image[half_pixel] = accumulated_ao_guide;
    perceptual_radiance_history_image[half_pixel] = accumulated_perceptual_radiance;
}
