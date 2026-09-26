import common;
import gpu;
import fsr3;

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x4> input_color;
    Texture2D<f32x2> dilated_motion_vectors;
    Texture2D<f32x4> dilated_reactive_masks;
    Texture2D<f32> luma_instability;
    Texture2D<f32> farthest_depth_mip1;
    Texture2D<f32x4> internal_upscaled_color_prev;
    ConstantBuffer<FSR3Exposure> exposure;
    Sampler sampler_linear_clamp;

    RWTexture2D<f32> new_locks;
    RWTexture2D<f32x4> internal_upscaled_color;
    RWTexture2D<f32x4> upscaled_output;
};
ParameterBlock<ShaderParameters> params;

struct PushConstants {
    // when RCAS runs it owns the final write, so the accumulate pass skips it
    u32 apply_sharpening;
};

struct HistoryLoader : ITexelLoader {
    Texture2D<f32x4> tex;

    func load(i32x2 pos) -> f32x4 {
        return this.tex.Load(i32x3(pos, 0));
    }
};

struct ColorLoader : IColorLoader {
    Texture2D<f32x4> tex;

    func load_color(i32x2 pos) -> f32x3 {
        return this.tex.Load(i32x3(pos, 0)).rgb;
    }
};

func init_pass_data(
    i32x2 px_hr_pos,
    f32 exposure,
    inout AccumulationPassCommonParams p,
    inout AccumulationPassData data
) -> void {
    p.px_hr_pos = px_hr_pos;

    let hr_uv = (f32x2(px_hr_pos) + 0.5) / f32x2(params.constants.upscale_size);
    p.hr_uv = hr_uv;
    p.lr_uv_jittered = hr_uv + params.constants.jitter_offset / f32x2(params.constants.render_size);
    p.lr_uv_hw_sampler = fsr3_clamp_uv(
        p.lr_uv_jittered,
        params.constants.render_size,
        params.constants.max_render_size
    );

    // render resolution motion vectors, so the dilated field is indexed at low res
    p.motion_vector = params.dilated_motion_vectors.Load(
        i32x3(i32x2(hr_uv * f32x2(params.constants.render_size)), 0)
    );
    p.velocity_4k = fsr3_get_4k_velocity(p.motion_vector);

    fsr3_compute_reprojected_uvs(p);

    let luma_instability_uv = fsr3_clamp_uv(
        hr_uv,
        params.constants.render_size,
        params.constants.max_render_size
    );
    p.luma_instability_factor = params.luma_instability.SampleLevel(
        params.sampler_linear_clamp,
        luma_instability_uv,
        0.0
    );

    u32 fd_width, fd_height, fd_levels;
    params.farthest_depth_mip1.GetDimensions(0, fd_width, fd_height, fd_levels);
    let farthest_depth_uv = fsr3_clamp_uv(
        p.lr_uv_jittered,
        params.constants.render_size / 2,
        i32x2(fd_width, fd_height)
    );
    p.farthest_depth_in_meters = params.farthest_depth_mip1.SampleLevel(
        params.sampler_linear_clamp,
        farthest_depth_uv,
        0.0
    );

    p.is_new_sample = (p.is_existing_sample == false) || (params.constants.frame_index == 0.0);

    let masks = params.dilated_reactive_masks.SampleLevel(params.sampler_linear_clamp, p.lr_uv_hw_sampler, 0.0);
    p.reactive_mask = saturate(masks[REACTIVE]);
    p.disocclusion = saturate(masks[DISOCCLUSION]);
    p.shading_change = saturate(masks[SHADING_CHANGE]);
    p.accumulation = saturate(masks[ACCUMULATION]);
    // the unorm round trip leaves a floor of noise, drop anything under one stored step
    p.accumulation *= f32(round(p.accumulation * 100.0) > 1.0);

    data.upsampled_color = f32x3(0.0);
    data.history_color = f32x3(0.0);
    data.history_weight = 1.0;
    data.upsampled_weight = 0.0;
    data.lock = 0.0;
    data.lock_contribution_this_frame = 0.0;
}

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform PushConstants pc) -> void {
    let px_hr_pos = i32x2(thread_id.xy);
    if (!fsr3_is_on_screen(px_hr_pos, params.constants.upscale_size)) {
        return;
    }

    let exposure = params.exposure.value();

    AccumulationPassCommonParams p;
    AccumulationPassData data;
    init_pass_data(px_hr_pos, exposure, p, data);

    if (p.is_existing_sample && !p.is_new_sample) {
        HistoryLoader history_loader = { params.internal_upscaled_color_prev };
        fsr3_reproject_history_color(
            history_loader,
            params.constants.previous_frame_upscale_size,
            params.constants.delta_pre_exposure,
            exposure,
            p,
            data
        );
    }

    let new_lock_intensity = params.new_locks[thread_id.xy];
    fsr3_update_lock_status(p, new_lock_intensity, params.constants.jitter_phase_count, data);

    fsr3_compute_base_accumulation_weight(p, params.constants.velocity_factor, data);

    ColorLoader color_loader = { params.input_color };
    fsr3_compute_upsampled_color_and_weight(color_loader, params.constants, exposure, p, data);

    fsr3_rectify_history(p, data);

    fsr3_accumulate(p, data);

    data.history_color /= exposure;
    data.history_color = max(data.history_color, f32x3(0.0));

    params.internal_upscaled_color[thread_id.xy] = f32x4(data.history_color, data.lock);

    if (pc.apply_sharpening == 0u) {
        params.upscaled_output[thread_id.xy] = f32x4(data.history_color, 1.0);
    }

    params.new_locks[thread_id.xy] = 0.0;
}
