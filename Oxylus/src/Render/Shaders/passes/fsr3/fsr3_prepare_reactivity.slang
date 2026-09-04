import common;
import gpu;
import fsr3;

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x2> dilated_motion_vectors;
    Texture2D<f32> dilated_depth;
    Texture2D<u32> reconstructed_prev_nearest_depth;
    Texture2D<f32> current_luma;
    Texture2D<f32> shading_change;
    Texture2D<f32> accumulation_prev;
    Texture2D<f32> input_reactive_mask;
    Texture2D<f32> input_transparency_and_composition_mask;
    ConstantBuffer<FSR3Exposure> exposure;
    Sampler sampler_linear_clamp;

    RWTexture2D<f32x4> dilated_reactive_masks;
    RWTexture2D<f32> accumulation;
    RWTexture2D<f32> new_locks;
};
ParameterBlock<ShaderParameters> params;

// a surface is disoccluded when nothing in last frame's reconstructed depth grid is close enough
// in front of it to be the same surface
func compute_disocclusions(f32x2 uv, f32x2 motion_vector, f32 current_depth_view_space) -> f32 {
    let nearest_depth_in_meters = min(
        current_depth_view_space * params.constants.view_space_to_meters_factor,
        FSR3_FP16_MAX
    );
    let mv_threshold = fsr3_reconstructed_depth_mv_px_threshold(nearest_depth_in_meters);

    let mv = motion_vector * f32(fsr3_get_4k_velocity(motion_vector) > mv_threshold);

    let reprojected_uv = uv + mv;
    let bilinear_info = fsr3_get_bilinear_sampling_data(reprojected_uv, params.constants.render_size);

    var disocclusion = 0.0;
    var weight_sum = 0.0;
    var potential_disocclusion = true;

    for (i32 i = 0; i < 4 && potential_disocclusion; i++) {
        let sample_pos = fsr3_clamp_load(bilinear_info.base_pos, bilinear_info.offsets[i], params.constants.render_size);

        if (fsr3_is_on_screen(sample_pos, params.constants.render_size)) {
            let weight = bilinear_info.weights[i];
            if (weight > RECONSTRUCTED_DEPTH_BILINEAR_WEIGHT_THRESHOLD) {
                let prev_depth_raw = asfloat(params.reconstructed_prev_nearest_depth.Load(i32x3(sample_pos, 0)));
                let prev_nearest_depth_view_space = params.constants.get_view_space_depth(prev_depth_raw);
                let depth_difference = current_depth_view_space - prev_nearest_depth_view_space;

                potential_disocclusion = potential_disocclusion && (depth_difference > FSR3_FP32_MIN);

                if (potential_disocclusion) {
                    let half_viewport_width = length(f32x2(params.constants.render_size) * 0.5);
                    let depth_threshold = max(current_depth_view_space, prev_nearest_depth_view_space);

                    const f32 ksep = 1.37e-05;
                    let required_depth_separation = ksep * half_viewport_width * depth_threshold;

                    disocclusion += saturate(required_depth_separation / depth_difference) * weight;
                    weight_sum += weight;
                }
            }
        }
    }

    return (potential_disocclusion && weight_sum > 0.0) ? saturate(1.0 - disocclusion / weight_sum) : 0.0;
}

// where this pixel's motion disagrees with the motion at the place it reprojects to, the vector
// field is unreliable and the pixel should lean on the current frame instead of history
func compute_motion_divergence(f32x2 uv, f32x2 motion_vector, f32 current_depth_sample) -> f32 {
    let reprojected_pos = i32x2((uv + motion_vector) * f32x2(params.constants.render_size));
    let reprojected_depth = params.dilated_depth.Load(i32x3(reprojected_pos, 0));
    let reprojected_motion_vector = params.dilated_motion_vectors.Load(i32x3(reprojected_pos, 0));

    let reprojected_velocity = fsr3_get_4k_velocity(reprojected_motion_vector);
    let velocity_4k = fsr3_get_4k_velocity(motion_vector);

    let nucleus_depth_in_meters = params.constants.get_view_space_depth_in_meters(reprojected_depth);
    let current_depth_in_meters = params.constants.get_view_space_depth_in_meters(current_depth_sample);

    let distance_factor = fsr3_min_divided_by_max(nucleus_depth_in_meters, current_depth_in_meters);
    let velocity_factor = saturate(velocity_4k / 10.0);

    return (1.0 - saturate(reprojected_velocity / velocity_4k)) * distance_factor * velocity_factor;
}

func dilate_reactive_masks(i32x2 px_pos) -> f32 {
    var dilated = 0.0;

    [ForceUnroll]
    for (i32 y = -1; y <= 1; y++) {
        [ForceUnroll]
        for (i32 x = -1; x <= 1; x++) {
            let coord = fsr3_clamp_load(px_pos, i32x2(x, y), params.constants.render_size);
            dilated = max(dilated, params.input_reactive_mask.Load(i32x3(coord, 0)));
        }
    }

    return dilated;
}

// a thin bright feature sitting on a different background is exactly what a temporal upscaler
// loses first, so it earns a lock. the rejection masks throw out anything that is really a corner
// or an edge of a larger region rather than a genuine ridge
func compute_thin_feature_confidence(i32x2 px_pos, f32 exposure) -> f32 {
    /*
     1 2 3
     4 0 5
     6 7 8
    */
    const i32x2 sample_offsets[9] = {
        i32x2(+0, +0),
        i32x2(-1, -1), i32x2(+0, -1), i32x2(+1, -1),
        i32x2(-1, +0), i32x2(+1, +0),
        i32x2(-1, +1), i32x2(+0, +1), i32x2(+1, +1),
    };

    f32 samples[9];
    var luma_min = FSR3_FP32_MAX;
    var luma_max = FSR3_FP32_MIN;

    [ForceUnroll]
    for (i32 i = 0; i < 9; i++) {
        let sample_pos = fsr3_clamp_load(px_pos, sample_offsets[i], params.constants.render_size);
        samples[i] = params.current_luma.Load(i32x3(sample_pos, 0)) * exposure;

        luma_min = min(luma_min, samples[i]);
        luma_max = max(luma_max, samples[i]);
    }

    const f32 threshold = 0.9;
    var dissimilar_luma_min = FSR3_FP32_MAX;
    var dissimilar_luma_max = 0.0;

    var pattern_mask = 1u << 0; // nucleus counts as similar to itself

    const u32 rejection_masks[4] = {
        (1u << 1) | (1u << 2) | (1u << 4) | (1u << 0), // upper left
        (1u << 2) | (1u << 3) | (1u << 5) | (1u << 0), // upper right
        (1u << 4) | (1u << 6) | (1u << 7) | (1u << 0), // lower left
        (1u << 5) | (1u << 7) | (1u << 8) | (1u << 0), // lower right
    };

    [ForceUnroll]
    for (i32 i = 1; i < 9; i++) {
        let difference = abs(samples[i] - samples[0]) / (luma_max - luma_min);

        if (difference < threshold) {
            pattern_mask |= (1u << u32(i));
        } else {
            dissimilar_luma_min = min(dissimilar_luma_min, samples[i]);
            dissimilar_luma_max = max(dissimilar_luma_max, samples[i]);
        }
    }

    let is_ridge = samples[0] > dissimilar_luma_max || samples[0] < dissimilar_luma_min;
    if (!is_ridge) {
        return 0.0;
    }

    [ForceUnroll]
    for (i32 i = 0; i < 4; i++) {
        if ((pattern_mask & rejection_masks[i]) == rejection_masks[i]) {
            return 0.0;
        }
    }

    return 1.0 - luma_min / luma_max;
}

// accumulation counts how many frames of history this pixel is entitled to trust. a shading change
// resets it to zero and a disocclusion drives it negative, so the pixel has to earn its way back up
// at accumulation_added_per_frame before history starts contributing again
func update_accumulation(i32x2 px_pos, f32x2 uv, f32x2 motion_vector, f32 disocclusion, f32 shading_change) -> f32 {
    let reprojected_uv = uv + motion_vector;
    var accumulation = 0.0;

    if (fsr3_is_uv_inside(reprojected_uv)) {
        let reprojected_uv_hw = fsr3_clamp_uv(
            reprojected_uv,
            params.constants.previous_frame_render_size,
            params.constants.max_render_size
        );
        accumulation = saturate(
            params.accumulation_prev.SampleLevel(params.sampler_linear_clamp, reprojected_uv_hw, 0.0)
        );
    }

    accumulation = lerp(accumulation, 0.0, shading_change);
    accumulation = lerp(
        accumulation,
        min(params.constants.min_disocclusion_accumulation, accumulation),
        disocclusion
    );

    accumulation *= f32(round(accumulation * 100.0) > 1.0);

    // stored normalized so it fits a unorm target
    let accumulated_frames_to_store = saturate(accumulation + params.constants.accumulation_added_per_frame);
    params.accumulation[u32x2(px_pos)] = accumulated_frames_to_store;

    return accumulation;
}

// jitter is re-applied here and reverted again when the accumulate pass samples this
func compute_shading_change(f32x2 uv) -> f32 {
    let shading_change_uv = fsr3_clamp_uv(
        uv - params.constants.jitter_offset / f32x2(params.constants.render_size),
        params.constants.shading_change_render_size(),
        params.constants.shading_change_max_render_size()
    );

    return saturate(params.shading_change.SampleLevel(params.sampler_linear_clamp, shading_change_uv, 0.0));
}

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let px_pos = i32x2(thread_id.xy);
    if (!fsr3_is_on_screen(px_pos, params.constants.render_size)) {
        return;
    }

    let exposure = params.exposure.value();
    let uv = (f32x2(px_pos) + 0.5) / f32x2(params.constants.render_size);
    let motion_vector = params.dilated_motion_vectors.Load(i32x3(px_pos, 0));
    let dilated_depth = params.dilated_depth.Load(i32x3(px_pos, 0));

    let disocclusion = compute_disocclusions(uv, motion_vector, params.constants.get_view_space_depth(dilated_depth));
    let shading_change = max(dilate_reactive_masks(px_pos), compute_shading_change(uv));

    let motion_divergence = compute_motion_divergence(uv, motion_vector, dilated_depth);
    let transparency_uv = fsr3_clamp_uv(uv, params.constants.render_size, params.constants.max_render_size);
    let dilated_transparency_and_composition = params.input_transparency_and_composition_mask.SampleLevel(
        params.sampler_linear_clamp,
        transparency_uv,
        0.0
    );
    let final_reactiveness = max(motion_divergence, dilated_transparency_and_composition);

    let accumulation = update_accumulation(px_pos, uv, motion_vector, disocclusion, shading_change);

    f32x4 output;
    output[REACTIVE] = final_reactiveness;
    output[DISOCCLUSION] = disocclusion;
    output[SHADING_CHANGE] = shading_change;
    output[ACCUMULATION] = accumulation;

    params.dilated_reactive_masks[thread_id.xy] = output;

    let lock_strength = compute_thin_feature_confidence(px_pos, exposure);
    if (lock_strength > (1.0 / 100.0)) {
        params.new_locks[u32x2(params.constants.compute_hr_pos_from_lr_pos(px_pos))] = lock_strength;
    }
}
