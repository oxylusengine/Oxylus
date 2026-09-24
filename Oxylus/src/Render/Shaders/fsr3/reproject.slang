implementing fsr3;

public func fsr3_compute_reprojected_uvs(inout AccumulationPassCommonParams params) -> void {
    params.reprojected_hr_uv = params.hr_uv + params.motion_vector;
    params.is_existing_sample = fsr3_is_uv_inside(params.reprojected_hr_uv);
}

// history is stored premultiplied by the exposure of the frame that produced it, so undoing that
// and reapplying the current exposure is what keeps an exposure change from popping
public func fsr3_reproject_history_color<L : ITexelLoader>(
    L history_loader,
    i32x2 previous_frame_upscale_size,
    f32 delta_pre_exposure,
    f32 exposure,
    in AccumulationPassCommonParams params,
    inout AccumulationPassData data
) -> void {
    let reprojected_history = fsr3_sample_bicubic(
        history_loader,
        params.reprojected_hr_uv,
        previous_frame_upscale_size
    );

    data.history_color = reprojected_history.rgb;
    data.history_color *= delta_pre_exposure;
    data.history_color *= exposure;
    data.history_color = fsr3_rgb_to_ycocg(data.history_color);

    data.lock = reprojected_history.w;
}
