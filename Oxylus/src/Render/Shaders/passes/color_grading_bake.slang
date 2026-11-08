import common;
import gpu;
import scene;

#include <passes/color_grading_common.slang>
#include <passes/tonemap_operators.slang>

[[vk::constant_id(0)]] const u32 tonemap_type = TonemapType::None;
[[vk::binding(0, 0)]] StorageImage3D<f32x4, ImageFormat::RGBA16F> color_grading_lut;
[[vk::binding(1, 0)]] Sampler sampler;

[[vk::push_constant]] ColorGradingSettings C;

// Kim et al. 2002, valid from 1667 K to 25000 K
f32x2 planckian_xy(f32 kelvin) {
    let t = clamp(kelvin, 1667.0, 25000.0);
    let t2 = t * t;
    let t3 = t2 * t;
    var x = 0.0;
    if (t < 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }

    let x2 = x * x;
    let x3 = x2 * x;
    var y = 0.0;
    if (t < 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t < 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }

    return f32x2(x, y);
}

f32x2 xy_to_uv(f32x2 xy) {
    let d = -2.0 * xy.x + 12.0 * xy.y + 3.0;
    return f32x2(4.0 * xy.x, 6.0 * xy.y) / d;
}

f32x2 uv_to_xy(f32x2 uv) {
    let d = 2.0 * uv.x - 8.0 * uv.y + 4.0;
    return f32x2(3.0 * uv.x, 2.0 * uv.y) / d;
}

f32x3 white_point_xyz(f32 kelvin, f32 tint) {
    let uv = xy_to_uv(planckian_xy(kelvin));
    let uv_a = xy_to_uv(planckian_xy(kelvin * 0.99));
    let uv_b = xy_to_uv(planckian_xy(kelvin * 1.01));
    let tangent = normalize(uv_b - uv_a);
    var normal = f32x2(-tangent.y, tangent.x);
    if (normal.y < 0.0) {
        normal = -normal;
    }

    let xy = uv_to_xy(uv + normal * (tint * 0.05));
    return f32x3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y);
}

static const f32x3x3 BRADFORD = f32x3x3(
    0.8951, 0.2664, -0.1614,
    -0.7502, 1.7135, 0.0367,
    0.0389, -0.0685, 1.0296
);

static const f32x3x3 BRADFORD_INVERSE = f32x3x3(
    0.9869929, -0.1470543, 0.1599627,
    0.4323053, 0.5183603, 0.0492912,
    -0.0085287, 0.0400428, 0.9684867
);

f32x3 apply_white_balance(f32x3 color, f32 kelvin, f32 tint) {
    if (kelvin == 6500.0 && tint == 0.0) {
        return color;
    }

    let source = mul(BRADFORD, white_point_xyz(kelvin, tint));
    let destination = mul(BRADFORD, white_point_xyz(6500.0, 0.0));
    var cone = mul(BRADFORD, com::rec709_to_xyz(color));
    cone *= destination / source;

    return com::xyz_to_rec709(mul(BRADFORD_INVERSE, cone));
}

f32x3 sample_user_lut(f32x3 encoded) {
    let lut_index = NonUniformResourceIndex(C.user_lut_view_index);
    u32 width;
    u32 height;
    global_images[lut_index].GetDimensions(width, height);

    let n = f32(height);
    let blue = encoded.b * (n - 1.0);
    let slice_a = floor(blue);
    let slice_b = min(slice_a + 1.0, n - 1.0);
    let blue_fraction = blue - slice_a;

    let y = (encoded.g * (n - 1.0) + 0.5) / n;
    let x = encoded.r * (n - 1.0) + 0.5;
    let color_a = global_images[lut_index].SampleLevel(sampler, f32x2((slice_a * n + x) / (n * n), y), 0.0).rgb;
    let color_b = global_images[lut_index].SampleLevel(sampler, f32x2((slice_b * n + x) / (n * n), y), 0.0).rgb;

    return lerp(color_a, color_b, blue_fraction);
}

[[shader("compute")]]
[[numthreads(8, 8, 8)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    if (any(thread_id >= u32x3(COLOR_GRADING_LUT_SIZE))) {
        return;
    }

    var color = f32x3(
        color_grading_log_decode(thread_id.x),
        color_grading_log_decode(thread_id.y),
        color_grading_log_decode(thread_id.z)
    );

    color = apply_white_balance(color, C.temperature, C.tint);

    let mid_grey = COLOR_GRADING_MID_GREY;
    color = select(color > 0.0, mid_grey * pow(max(color, 1e-10) / mid_grey, C.contrast), color);

    let luma = dot(color, f32x3(0.2126729, 0.7151522, 0.0721750));
    color = lerp(f32x3(luma), color, C.saturation);

    color = pow(max(color * C.gain + C.lift, 0.0), 1.0 / max(C.gamma, 1e-4));

    color = apply_tonemap_operator(color, tonemap_type, C.filmic);

    if (C.user_lut_view_index != ~0u && C.lut_contribution > 0.0) {
        let encoded = com::linear_to_srgb(saturate(color));
        color = com::srgb_to_linear(lerp(encoded, sample_user_lut(encoded), C.lut_contribution));
    }

    color_grading_lut[thread_id] = f32x4(max(color, 0.0), 1.0);
}
