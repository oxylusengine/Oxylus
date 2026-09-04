implementing fsr3;

public interface IColorLoader {
    func load_color(i32x2 pos) -> f32x3;
};

public func fsr3_deringing(RectificationBox clipping_box, inout f32x3 color) -> void {
    color = clamp(color, clipping_box.aabb_min, clipping_box.aabb_max);
}

// the SDK defaults UPSAMPLE_USE_LANCZOS_TYPE to 2 (approximate), which avoids both the transcendental
// reference form and shipping a weight LUT texture
public func fsr3_get_upsample_lanczos_weight(f32x2 src_sample_offset, f32 kernel_weight) -> f32 {
    let biased = src_sample_offset * kernel_weight.xx;
    return fsr3_lanczos2_approx_sq(dot(biased, biased));
}

public func fsr3_compute_max_kernel_weight(f32x2 downscale_factor) -> f32 {
    let kernel_size_bias = 1.0 + (1.0 / downscale_factor - 1.0).x;
    return min(1.99, kernel_size_bias);
}

public func fsr3_load_prepared_color<L : IColorLoader>(L loader, i32x2 sample_pos, f32 exposure) -> f32x3 {
    let rgb = max(f32x3(0.0), loader.load_color(sample_pos)) * exposure;
    return fsr3_rgb_to_ycocg(rgb);
}

// a sliced 2-lobe lanczos: only the slice covered by this frame's jitter is resolved here, the rest
// of the kernel is what the temporal accumulation fills in over the jitter sequence
public func fsr3_compute_upsampled_color_and_weight<L : IColorLoader>(
    L loader,
    in FSR3Constants constants,
    f32 exposure,
    in AccumulationPassCommonParams params,
    inout AccumulationPassData data
) -> void {
    let dst_output_pos = f32x2(params.px_hr_pos) + f32x2(0.5);
    let src_output_pos = dst_output_pos * constants.downscale_factor;
    let src_input_pos = i32x2(floor(src_output_pos));
    // un-jittered position of the sample at offset 0,0
    let src_unjittered_pos = (f32x2(src_input_pos) + f32x2(0.5)) - constants.jitter_offset;
    let base_sample_offset = src_unjittered_pos - src_output_pos;

    i32x2 offset_tl;
    offset_tl.x = (src_unjittered_pos.x > src_output_pos.x) ? -2 : -1;
    offset_tl.y = (src_unjittered_pos.y > src_output_pos.y) ? -2 : -1;

    // when offset_tl is -2 the sampled rows run [-2, 1] and the clip box wants rows [1, 3]. flipping
    // the row/col index keeps the first 3x3 of the sampled array usable for the box either way, which
    // moves the branch off the color loads and onto the cheaper offset math
    let flip_row = src_unjittered_pos.y > src_output_pos.y;
    let flip_col = src_unjittered_pos.x > src_output_pos.x;
    let offset_tl_f = f32x2(offset_tl);

    let is_initial_sample = params.accumulation == 0.0;

    f32x3 samples[9];
    var sample_index = 0;

    [ForceUnroll]
    for (i32 row = 0; row < 3; row++) {
        [ForceUnroll]
        for (i32 col = 0; col < 3; col++) {
            let sample_col_row = i32x2(flip_col ? (3 - col) : col, flip_row ? (3 - row) : row);
            let src_sample_pos = src_input_pos + offset_tl + sample_col_row;
            let sample_coord = fsr3_clamp_load(src_sample_pos, i32x2(0, 0), constants.render_size);

            samples[sample_index] = fsr3_load_prepared_color(loader, sample_coord, exposure);
            sample_index += 1;
        }
    }

    // hdr input: go through the rgb tonemapper for the first sample so the box is built in a range
    // where a bright outlier cannot desaturate the result
    if (is_initial_sample) {
        [ForceUnroll]
        for (i32 i = 0; i < 9; i++) {
            samples[i] = fsr3_rgb_to_ycocg(fsr3_tonemap(fsr3_ycocg_to_rgb(samples[i])));
        }
    }

    let kernel_bias_max = fsr3_compute_max_kernel_weight(constants.downscale_factor);
    let kernel_bias_min = max(1.0, ((1.0 + kernel_bias_max) * 0.3));

    let kernel_bias_weight = min(
        1.0 - params.disocclusion * 0.5,
        min(1.0 - params.shading_change, saturate(data.history_weight * 5.0))
    );

    let kernel_bias = lerp(kernel_bias_min, kernel_bias_max, kernel_bias_weight);

    sample_index = 0;

    [ForceUnroll]
    for (i32 row = 0; row < 3; row++) {
        [ForceUnroll]
        for (i32 col = 0; col < 3; col++) {
            let sample_col_row = i32x2(flip_col ? (3 - col) : col, flip_row ? (3 - row) : row);
            let offset = offset_tl_f + f32x2(sample_col_row);
            let src_sample_offset = base_sample_offset + offset;

            let src_sample_pos = src_input_pos + offset_tl + sample_col_row;
            let on_screen_factor = f32(fsr3_is_on_screen(src_sample_pos, constants.render_size));

            if (!is_initial_sample) {
                let sample_weight = on_screen_factor *
                                    fsr3_get_upsample_lanczos_weight(src_sample_offset, kernel_bias);

                data.upsampled_color += samples[sample_index] * sample_weight;
                data.upsampled_weight += sample_weight;
            }

            {
                const f32 rectification_curve_bias = -2.3;
                let src_sample_offset_sq = dot(src_sample_offset, src_sample_offset);
                let box_sample_weight = exp(rectification_curve_bias * src_sample_offset_sq) * on_screen_factor;

                data.clipping_box.add_sample((row == 0) && (col == 0), samples[sample_index], box_sample_weight);
            }

            sample_index += 1;
        }
    }

    data.clipping_box.compute_variance_box_data();

    data.upsampled_weight *= f32(data.upsampled_weight > FSR3_EPSILON);

    if (data.upsampled_weight > FSR3_EPSILON) {
        // normalize before deringing, the clamp compares colors not weighted sums
        data.upsampled_color = data.upsampled_color / data.upsampled_weight;
        data.upsampled_weight *= AVERAGE_LANCZOS_WEIGHT_PER_FRAME;

        fsr3_deringing(data.clipping_box, data.upsampled_color);
    }

    if (is_initial_sample) {
        data.upsampled_color = fsr3_rgb_to_ycocg(fsr3_inverse_tonemap(fsr3_ycocg_to_rgb(data.clipping_box.box_center)));
        data.upsampled_weight = 1.0;
        data.history_weight = 0.0;
    }
}
