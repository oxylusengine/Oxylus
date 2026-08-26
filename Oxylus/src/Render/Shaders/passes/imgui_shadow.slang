module imgui_shadow;

import common;

struct VertexInput {
    f32x2 position;
    f32x2 tex_coord;
    f32x4 color;
};

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 local;
    f32x4 color;
};

struct PushConstants {
    f32x2 translate;
    f32x2 scale;
    f32x2 half_size;
    f32 corner_radius;
    f32 sigma;
    f32x2 cutout_center;
    f32x2 cutout_half_size;
    f32 cutout_radius;
    f32 cutout_enabled;
};
[[vk::push_constant]] PushConstants c;

static const u32 SAMPLE_COUNT = 12;
static const f32 INV_SQRT_TWO_PI = 0.3989422804;
static const f32 INV_SQRT_TWO = 0.7071067812;

// Abramowitz & Stegun 7.1.28, |err| < 5e-4.
func erf_approx(f32 x) -> f32 {
    let s = sign(x);
    let a = abs(x);
    var v = 1.0 + (0.278393 + (0.230389 + 0.078108 * a * a) * a) * a;
    v *= v;
    v *= v;
    return s - s / v;
}

func gaussian(f32 x, f32 sigma) -> f32 {
    return exp(-(x * x) / (2.0 * sigma * sigma)) * INV_SQRT_TWO_PI / sigma;
}

// Coverage of one horizontal slice of the rounded box, convolved with a 1D gaussian along x.
func rounded_box_shadow_x(f32 x, f32 y, f32 sigma, f32 corner, f32x2 half_size) -> f32 {
    let delta = min(half_size.y - corner - abs(y), 0.0);
    let curved = half_size.x - corner + sqrt(max(corner * corner - delta * delta, 0.0));
    let lo = erf_approx((x - curved) * INV_SQRT_TWO / sigma);
    let hi = erf_approx((x + curved) * INV_SQRT_TWO / sigma);
    return 0.5 * (hi - lo);
}

func rounded_box_shadow(f32x2 half_size, f32x2 point, f32 sigma, f32 corner) -> f32 {
    let low = point.y - half_size.y;
    let high = point.y + half_size.y;
    let start = clamp(-3.0 * sigma, low, high);
    let end = clamp(3.0 * sigma, low, high);

    let step = (end - start) / f32(SAMPLE_COUNT);
    var y = start + step * 0.5;
    var value = 0.0;
    for (u32 i = 0; i < SAMPLE_COUNT; i += 1) {
        value += rounded_box_shadow_x(point.x, point.y - y, sigma, corner, half_size) * gaussian(y, sigma) * step;
        y += step;
    }

    return value;
}

func sd_rounded_box(f32x2 point, f32x2 half_size, f32 radius) -> f32 {
    let q = abs(point) - half_size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput {
    VertexOutput output;

    output.position = f32x4(input.position * c.scale + c.translate, 0.0, 1.0);
    output.local = input.tex_coord;
    output.color.rgb = com::srgb_to_linear(input.color.rgb);
    output.color.a = input.color.a;

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_Target0 {
    let corner = min(c.corner_radius, min(c.half_size.x, c.half_size.y));
    var alpha = rounded_box_shadow(c.half_size, input.local, max(c.sigma, 0.01), corner);

    if (c.cutout_enabled > 0.5) {
        let cutout_corner = min(c.cutout_radius, min(c.cutout_half_size.x, c.cutout_half_size.y));
        let distance = sd_rounded_box(input.local - c.cutout_center, c.cutout_half_size, cutout_corner);
        alpha *= saturate(distance + 0.5);
    }

    return f32x4(input.color.rgb, input.color.a * saturate(alpha));
}
