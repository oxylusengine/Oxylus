implementing fsr3;

public static const f32 FSR3_FP16_MIN = 6.10e-05;
public static const f32 FSR3_FP16_MAX = 65504.0;
public static const f32 FSR3_EPSILON = FSR3_FP16_MIN;
public static const f32 FSR3_TONEMAP_EPSILON = FSR3_FP16_MIN;
public static const f32 FSR3_FP32_MAX = 3.402823466e+38;
public static const f32 FSR3_FP32_MIN = 1.175494351e-38;

// reconstructed depth usage
public static const f32 RECONSTRUCTED_DEPTH_BILINEAR_WEIGHT_THRESHOLD = FSR3_EPSILON * 10.0;

// accumulation
public static const f32 UPSAMPLE_LANCZOS_WEIGHT_SCALE = 1.0 / 16.0;
public static const f32 AVERAGE_LANCZOS_WEIGHT_PER_FRAME = 0.74 * UPSAMPLE_LANCZOS_WEIGHT_SCALE;
public static const f32 ACCUMULATION_MAX_ON_MOTION = 3.0 * UPSAMPLE_LANCZOS_WEIGHT_SCALE;

public static const i32 SHADING_CHANGE_SET_SIZE = 5;
public static const i32 SHADING_CHANGE_MIP_START = 0;

public static const f32 LOCK_THRESHOLD = 1.0;
public static const f32 LOCK_MAX = 2.0;

// channel indices into the dilated reactive mask
public static const i32 REACTIVE = 0;
public static const i32 DISOCCLUSION = 1;
public static const i32 SHADING_CHANGE = 2;
public static const i32 ACCUMULATION = 3;

// channel indices into the frame info buffer
public static const i32 FRAME_INFO_EXPOSURE = 0;
public static const i32 FRAME_INFO_LOG_LUMA = 1;
public static const i32 FRAME_INFO_SCENE_AVERAGE_LUMA = 2;

public static const f32 RESET_AUTO_EXPOSURE_AVERAGE_SMOOTHING = 1e4;

public func fsr3_reconstructed_depth_mv_px_threshold(f32 nearest_depth_in_meters) -> f32 {
    return lerp(0.25, 0.75, saturate(nearest_depth_in_meters / 100.0));
}

public func fsr3_get_4k_velocity(f32x2 motion_vector) -> f32 {
    return length(motion_vector * f32x2(3840.0, 2160.0));
}

public func fsr3_ycocg_to_rgb(f32x3 ycocg) -> f32x3 {
    return f32x3(
        ycocg.x + ycocg.y - ycocg.z,
        ycocg.x + ycocg.z,
        ycocg.x - ycocg.y - ycocg.z
    );
}

public func fsr3_rgb_to_ycocg(f32x3 rgb) -> f32x3 {
    return f32x3(
        0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
        0.5 * rgb.r - 0.5 * rgb.b,
        -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b
    );
}

public func fsr3_rgb_to_luma(f32x3 linear_rgb) -> f32 {
    return dot(linear_rgb, f32x3(0.2126, 0.7152, 0.0722));
}

public func fsr3_rgb_to_perceived_luma(f32x3 linear_rgb) -> f32 {
    let luminance = fsr3_rgb_to_luma(linear_rgb);

    var perceived = 0.0;
    if (luminance <= 216.0 / 24389.0) {
        perceived = luminance * (24389.0 / 27.0);
    } else {
        perceived = pow(luminance, 1.0 / 3.0) * 116.0 - 16.0;
    }

    return perceived * 0.01;
}

public func fsr3_tonemap(f32x3 rgb) -> f32x3 {
    return rgb / (max(max(0.0, rgb.r), max(rgb.g, rgb.b)) + 1.0).xxx;
}

public func fsr3_inverse_tonemap(f32x3 rgb) -> f32x3 {
    return rgb / max(FSR3_TONEMAP_EPSILON, 1.0 - max(rgb.r, max(rgb.g, rgb.b))).xxx;
}

public func fsr3_is_uv_inside(f32x2 uv) -> bool {
    return (uv.x >= 0.0 && uv.x <= 1.0) && (uv.y >= 0.0 && uv.y <= 1.0);
}

public func fsr3_clamp_load(i32x2 px_sample, i32x2 px_offset, i32x2 texture_size) -> i32x2 {
    var result = px_sample + px_offset;
    result.x = max(0, min(result.x, texture_size.x - 1));
    result.y = max(0, min(result.y, texture_size.y - 1));
    return result;
}

public func fsr3_clamp_uv(f32x2 uv, i32x2 texture_size, i32x2 resource_size) -> f32x2 {
    let sample_location = uv * texture_size;
    let clamped_location = max(f32x2(0.5), min(sample_location, f32x2(texture_size) - f32x2(0.5)));
    return clamped_location / f32x2(resource_size);
}

public func fsr3_is_on_screen(i32x2 pos, i32x2 size) -> bool {
    return all(u32x2(pos) < u32x2(size));
}

public func fsr3_compute_auto_exposure_from_lavg(f32 log_average) -> f32 {
    let lavg = exp(log_average);

    const f32 S = 100.0; // ISO arithmetic speed
    const f32 K = 12.5;
    let exposure_iso100 = log2((lavg * S) / K);

    const f32 q = 0.65;
    let lmax = (78.0 / (q * S)) * pow(2.0, exposure_iso100);

    return 1.0 / lmax;
}

public func fsr3_compute_ndc(f32x2 px_pos, i32x2 size) -> f32x2 {
    return px_pos / f32x2(size) * f32x2(2.0, -2.0) + f32x2(-1.0, 1.0);
}

public func fsr3_safe_rcp3(f32x3 v) -> f32x3 {
    return all(v != f32x3(0.0)) ? (f32x3(1.0) / v) : f32x3(0.0);
}

public func fsr3_min_divided_by_max(f32 v0, f32 v1, f32 on_zero_return_value) -> f32 {
    let m = max(v0, v1);
    return m != 0.0 ? min(v0, v1) / m : on_zero_return_value;
}

public func fsr3_min_divided_by_max(f32 v0, f32 v1) -> f32 {
    return fsr3_min_divided_by_max(v0, v1, 0.0);
}

public struct BilinearSamplingData {
    public i32x2 offsets[4];
    public f32 weights[4];
    public i32x2 base_pos;
};

public func fsr3_get_bilinear_sampling_data(f32x2 uv, i32x2 size) -> BilinearSamplingData {
    BilinearSamplingData data;

    let px_sample = (uv * size) - f32x2(0.5);
    data.base_pos = i32x2(floor(px_sample));
    let px_frac = frac(px_sample);

    data.offsets[0] = i32x2(0, 0);
    data.offsets[1] = i32x2(1, 0);
    data.offsets[2] = i32x2(0, 1);
    data.offsets[3] = i32x2(1, 1);

    data.weights[0] = (1.0 - px_frac.x) * (1.0 - px_frac.y);
    data.weights[1] = (px_frac.x) * (1.0 - px_frac.y);
    data.weights[2] = (1.0 - px_frac.x) * (px_frac.y);
    data.weights[3] = (px_frac.x) * (px_frac.y);

    return data;
}

public struct PlaneData {
    public f32x3 normal;
    public f32 distance_from_origin;
};

public func fsr3_get_plane_from_points(f32x3 p0, f32x3 p1, f32x3 p2) -> PlaneData {
    PlaneData plane;

    let v0 = p0 - p1;
    let v1 = p0 - p2;
    plane.normal = normalize(cross(v0, v1));
    plane.distance_from_origin = -dot(p0, plane.normal);

    return plane;
}

public func fsr3_point_to_plane_distance(PlaneData plane, f32x3 point) -> f32 {
    return abs(dot(plane.normal, point) + plane.distance_from_origin);
}

public struct RectificationBox {
    public f32x3 box_center;
    public f32x3 box_vec;
    public f32x3 aabb_min;
    public f32x3 aabb_max;
    public f32 box_center_weight;

    [mutating]
    public func add_initial_sample(f32x3 color_sample, f32 sample_weight) -> void {
        this.aabb_min = color_sample;
        this.aabb_max = color_sample;

        let weighted_sample = color_sample * sample_weight;
        this.box_center = weighted_sample;
        this.box_vec = color_sample * weighted_sample;
        this.box_center_weight = sample_weight;
    }

    [mutating]
    public func add_sample(bool initial_sample, f32x3 color_sample, f32 sample_weight) -> void {
        if (initial_sample) {
            this.add_initial_sample(color_sample, sample_weight);
            return;
        }

        this.aabb_min = min(this.aabb_min, color_sample);
        this.aabb_max = max(this.aabb_max, color_sample);

        let weighted_sample = color_sample * sample_weight;
        this.box_center += weighted_sample;
        this.box_vec += color_sample * weighted_sample;
        this.box_center_weight += sample_weight;
    }

    [mutating]
    public func compute_variance_box_data() -> void {
        this.box_center_weight = abs(this.box_center_weight) > FSR3_FP32_MIN ? this.box_center_weight : 1.0;
        this.box_center /= this.box_center_weight;
        this.box_vec /= this.box_center_weight;
        this.box_vec = sqrt(abs(this.box_vec - this.box_center * this.box_center));
    }
};

// constant-dependent helpers. the SDK reaches these through cbFSR3Upscaler accessors; as methods
// they keep the same names and stay comparable against the reference
public extension FSR3Constants {
    public func shading_change_render_size() -> i32x2 {
        return i32x2(f32x2(this.render_size) * 0.5);
    }

    public func shading_change_max_render_size() -> i32x2 {
        return i32x2(f32x2(this.max_render_size) * 0.5);
    }

    public func previous_frame_shading_change_render_size() -> i32x2 {
        return i32x2(f32x2(this.previous_frame_render_size) * 0.5);
    }

    public func tonemap_first_frame() -> bool {
        return this.frame_index == 0.0;
    }

    // maps a low resolution pixel to the display resolution pixel its jittered sample landed on
    public func compute_hr_pos_from_lr_pos(i32x2 px_lr_pos) -> i32x2 {
        let src_jittered_pos = f32x2(px_lr_pos) + 0.5 - this.jitter_offset;
        let lr_pos_in_hr = (src_jittered_pos / f32x2(this.render_size)) * f32x2(this.upscale_size);
        return i32x2(floor(lr_pos_in_hr));
    }

    public func get_view_space_depth(f32 device_depth) -> f32 {
        // see the fDeviceToViewDepth derivation in ffx_fsr3upscaler.cpp
        return this.device_to_view_depth[1] / (device_depth - this.device_to_view_depth[0]);
    }

    public func get_view_space_depth_in_meters(f32 device_depth) -> f32 {
        return this.get_view_space_depth(device_depth) * this.view_space_to_meters_factor;
    }

    public func get_view_space_position(i32x2 viewport_pos, i32x2 viewport_size, f32 device_depth) -> f32x3 {
        let z = this.get_view_space_depth(device_depth);
        let ndc_pos = fsr3_compute_ndc(f32x2(viewport_pos), viewport_size);

        return f32x3(
            this.device_to_view_depth[2] * ndc_pos.x * z,
            this.device_to_view_depth[3] * ndc_pos.y * z,
            z
        );
    }

    public func get_view_space_position_in_meters(i32x2 viewport_pos, i32x2 viewport_size, f32 device_depth) -> f32x3 {
        return this.get_view_space_position(viewport_pos, viewport_size, device_depth) *
               this.view_space_to_meters_factor;
    }

    // reversed-z, so the far plane sits at 0
    public func get_max_distance_in_meters() -> f32 {
        return this.get_view_space_depth(0.0) * this.view_space_to_meters_factor;
    }
};

// per-pixel state threaded through the accumulate pass, mirrors AccumulationPassCommonParams
public struct AccumulationPassCommonParams {
    public i32x2 px_hr_pos;
    public f32x2 hr_uv;
    public f32x2 lr_uv_jittered;
    public f32x2 lr_uv_hw_sampler;
    public f32x2 motion_vector;
    public f32x2 reprojected_hr_uv;
    public f32 velocity_4k;
    public f32 disocclusion;
    public f32 reactive_mask;
    public f32 shading_change;
    public f32 accumulation;
    public f32 luma_instability_factor;
    public f32 farthest_depth_in_meters;

    public bool is_existing_sample;
    public bool is_new_sample;
};

public struct AccumulationPassData {
    public RectificationBox clipping_box;
    public f32x3 upsampled_color;
    public f32 upsampled_weight;
    public f32x3 history_color;
    public f32 history_weight;
    public f32 lock;
    public f32 lock_contribution_this_frame;
};

// Mirrors GPU::HistogramLuminance. The exposure multiplier is the *second* field; the first is the
// adapted scene luminance, which is a different quantity entirely and drifts every frame while eye
// adaptation converges.
public struct FSR3Exposure {
    public f32 adapted_luminance;
    public f32 exposure;

    // the SDK applies this same guard: a zero exposure would divide the stored history to infinity
    public func value() -> f32 {
        return this.exposure == 0.0 ? 1.0 : this.exposure;
    }
};
