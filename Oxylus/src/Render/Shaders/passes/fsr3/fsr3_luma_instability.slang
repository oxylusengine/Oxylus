import common;
import gpu;
import fsr3;

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x2> dilated_motion_vectors;
    Texture2D<f32x4> dilated_reactive_masks;
    Texture2D<f32> current_luma;
    Texture2D<f32x4> luma_history_prev;
    Texture2D<f32> farthest_depth_mip1;
    ConstantBuffer<FSR3Exposure> exposure;
    Sampler sampler_linear_clamp;

    RWTexture2D<f32x4> luma_history;
    RWTexture2D<f32> luma_instability;
};
ParameterBlock<ShaderParameters> params;

static const i32 N_MINUS_1 = 0;
static const i32 N_MINUS_2 = 1;
static const i32 N_MINUS_3 = 2;
static const i32 N_MINUS_4 = 3;

struct LumaInstabilityFactorData {
    f32x4 luma_history;
    f32 luma_instability_factor;
};

// a pixel whose luma keeps returning to an earlier value, rather than trending, is flickering
// rather than genuinely changing, and its history is worth protecting harder
func compute_luma_instability_factor(
    LumaInstabilityFactorData data,
    f32 current_frame_luma,
    f32 exposure
) -> LumaInstabilityFactorData {
    var result = data;

    var luma_instability = 0.0;
    let diffs0 = current_frame_luma - result.luma_history[N_MINUS_1];
    let similarity0 = fsr3_min_divided_by_max(current_frame_luma, result.luma_history[N_MINUS_1], 1.0);

    var max_similarity = similarity0;

    if (similarity0 < 1.0) {
        for (i32 i = N_MINUS_2; i <= N_MINUS_4; i++) {
            let diffs1 = current_frame_luma - result.luma_history[i];
            let similarity1 = fsr3_min_divided_by_max(current_frame_luma, result.luma_history[i]);

            if (sign(diffs0) == sign(diffs1)) {
                max_similarity = max(max_similarity, similarity1);
            }
        }

        luma_instability = f32(max_similarity > similarity0);
    }

    result.luma_history[N_MINUS_4] = result.luma_history[N_MINUS_3];
    result.luma_history[N_MINUS_3] = result.luma_history[N_MINUS_2];
    result.luma_history[N_MINUS_2] = result.luma_history[N_MINUS_1];
    result.luma_history[N_MINUS_1] = current_frame_luma;

    result.luma_history /= exposure;

    // a history slot that never filled means there is not enough evidence to call it unstable
    result.luma_instability_factor = luma_instability * f32(result.luma_history[N_MINUS_4] != 0.0);

    return result;
}

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let px_pos = i32x2(thread_id.xy);
    let exposure = params.exposure.value();

    LumaInstabilityFactorData data;
    data.luma_instability_factor = 0.0;
    data.luma_history = f32x4(0.0);

    let dilated_motion_vector = params.dilated_motion_vectors.Load(i32x3(px_pos, 0));
    let uv = (f32x2(px_pos) + 0.5) / f32x2(params.constants.render_size);
    let uv_curr_jittered = uv + params.constants.jitter_offset / f32x2(params.constants.render_size);
    let uv_prev_jittered = uv + params.constants.previous_frame_jitter_offset /
                                f32x2(params.constants.previous_frame_render_size);
    let reprojected_uv = uv_prev_jittered + dilated_motion_vector;

    if (fsr3_is_uv_inside(reprojected_uv)) {
        let uv_reactive_hw = fsr3_clamp_uv(
            uv_curr_jittered,
            params.constants.render_size,
            params.constants.max_render_size
        );

        let masks = params.dilated_reactive_masks.SampleLevel(params.sampler_linear_clamp, uv_reactive_hw, 0.0);
        let reactive_mask = saturate(masks[REACTIVE]);
        let disocclusion = saturate(masks[DISOCCLUSION]);
        let shading_change = saturate(masks[SHADING_CHANGE]);
        let accumulation = saturate(masks[ACCUMULATION]);

        if (accumulation > 0.9) {
            let uv_hw = fsr3_clamp_uv(uv_curr_jittered, params.constants.render_size, params.constants.max_render_size);
            let current_frame_luma = params.current_luma.SampleLevel(params.sampler_linear_clamp, uv_hw, 0.0) * exposure;

            let reprojected_uv_hw = fsr3_clamp_uv(
                reprojected_uv,
                params.constants.previous_frame_render_size,
                params.constants.max_render_size
            );
            data.luma_history = params.luma_history_prev.SampleLevel(params.sampler_linear_clamp, reprojected_uv_hw, 0.0) *
                                params.constants.delta_pre_exposure * exposure;

            data = compute_luma_instability_factor(data, current_frame_luma, exposure);

            let velocity_weight = 1.0 - saturate(fsr3_get_4k_velocity(dilated_motion_vector) / 20.0);
            data.luma_instability_factor *= velocity_weight * (1.0 - disocclusion) * (1.0 - reactive_mask) *
                                            (1.0 - shading_change);
        }
    }

    params.luma_history[thread_id.xy] = data.luma_history;
    params.luma_instability[thread_id.xy] = data.luma_instability_factor;
}
