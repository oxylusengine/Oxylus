implementing fsr3;

// blends the freshly upsampled slice into the reprojected history. the tonemap round trip keeps a
// bright outlier from dominating the lerp, and is undone before the result is stored
public func fsr3_accumulate(in AccumulationPassCommonParams params, inout AccumulationPassData data) -> void {
    // guard against a zero accumulation and zero upsample weight producing a nan alpha
    data.history_weight *= f32(data.history_weight > FSR3_FP16_MIN);
    data.history_weight = max(FSR3_EPSILON, data.history_weight + data.upsampled_weight);

    data.upsampled_color = fsr3_rgb_to_ycocg(fsr3_tonemap(fsr3_ycocg_to_rgb(data.upsampled_color)));
    data.history_color = fsr3_rgb_to_ycocg(fsr3_tonemap(fsr3_ycocg_to_rgb(data.history_color)));

    let alpha = saturate(data.upsampled_weight / data.history_weight);
    data.history_color = lerp(data.history_color, data.upsampled_color, alpha);
    data.history_color = fsr3_ycocg_to_rgb(data.history_color);
    data.history_color = fsr3_inverse_tonemap(data.history_color);
}

// clips history against the neighbourhood variance box. the box grows when nothing suggests the
// history is stale and tightens under motion, disocclusion, reactivity or a shading change
public func fsr3_rectify_history(in AccumulationPassCommonParams params, inout AccumulationPassData data) -> void {
    let velocity_4k_factor = saturate(params.velocity_4k / 20.0);
    let distance_factor = saturate(0.75 - params.farthest_depth_in_meters / 20.0);
    let accumulation_factor = 1.0 - params.accumulation;
    let reactive_factor = pow(params.reactive_mask, 1.0 / 2.0);
    let shading_change_factor = params.shading_change;

    let box_scale_t = max(
        velocity_4k_factor,
        max(distance_factor, max(accumulation_factor, max(reactive_factor, shading_change_factor)))
    );

    let box_scale = lerp(3.0, 1.0, box_scale_t);
    let scaled_box_vec = data.clipping_box.box_vec * f32x3(1.7, 1.0, 1.0) * box_scale;

    let clamped_scaled_box_vec = max(scaled_box_vec, f32x3(1.193e-7));
    let transformed_history_color = (data.history_color - data.clipping_box.box_center) / clamped_scaled_box_vec;

    if (length(transformed_history_color) > 1.0) {
        let clamped_history_color = normalize(transformed_history_color);
        let final_clamped_history_color = (clamped_history_color * scaled_box_vec) + data.clipping_box.box_center;

        // the accumulation mask keeps a stale invalid-color protection from sticking around
        let history_contribution = max(params.luma_instability_factor, data.lock_contribution_this_frame) *
                                   params.accumulation * (1.0 - params.disocclusion);

        data.history_color = lerp(final_clamped_history_color, data.history_color, saturate(history_contribution));
    }
}

public func fsr3_update_lock_status(
    in AccumulationPassCommonParams params,
    f32 new_lock_intensity,
    f32 jitter_phase_count,
    inout AccumulationPassData data
) -> void {
    data.lock *= f32(params.is_new_sample == false);

    let lifetime_decrease_factor = max(saturate(params.shading_change), max(params.reactive_mask, params.disocclusion));
    data.lock = max(0.0, data.lock - lifetime_decrease_factor * LOCK_MAX);

    data.lock_contribution_this_frame = saturate(saturate(data.lock - LOCK_THRESHOLD) * (LOCK_MAX - LOCK_THRESHOLD));

    let intensity = new_lock_intensity * (1.0 - max(params.shading_change * 0.0, params.reactive_mask));
    data.lock = max(0.0, min(data.lock + intensity, LOCK_MAX));

    // decay for next frame, scaled so a lock survives roughly one jitter sequence
    let lifetime_decrease = (0.1 / jitter_phase_count) * (1.0 - lifetime_decrease_factor);
    data.lock = max(0.0, data.lock - lifetime_decrease);

    // motion is expected to be similar next frame, so drop a lock that would land offscreen rather
    // than let it pile up against the border
    let estimated_uv_next_frame = params.hr_uv - params.motion_vector;
    data.lock *= f32(fsr3_is_uv_inside(estimated_uv_next_frame));
}

public func fsr3_compute_base_accumulation_weight(
    in AccumulationPassCommonParams params,
    f32 velocity_factor,
    inout AccumulationPassData data
) -> void {
    var base_accumulation = params.accumulation;

    base_accumulation = min(
        base_accumulation,
        lerp(base_accumulation, 0.15, saturate(max(0.0, (params.velocity_4k * velocity_factor) / 0.5)))
    );

    data.history_weight = base_accumulation;
}
