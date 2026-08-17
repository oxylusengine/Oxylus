// Advanced Terrain Erosion Filter by Rune Skovbo Johansen
// Links:
// - https://www.shadertoy.com/view/wXcfWn
// - https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html

import common;
import gpu;
import scene;

public static constexpr f32 TERRAIN_EPSILON = 1e-10;

func clamp01(f32 v) -> f32 {
    return clamp(v, 0.0, 1.0);
}

func ease_out(f32 t) -> f32 {
    let v = 1.0 - clamp01(t);
    return 1.0 - v * v;
}

func pow_inv(f32 t, f32 power) -> f32 {
    return 1.0 - pow(1.0 - clamp01(t), power);
}

func smooth_start(f32 t, f32 smoothing) -> f32 {
    if (t >= smoothing) {
        return t - 0.5 * smoothing;
    }
    return 0.5 * t * t / smoothing;
}

func hash2(f32x2 x, u32 seed) -> f32x2 {
    let k = f32x2(0.3183099, 0.3678794); // 1/PI, exp(-1)
    let seed_offset = f32(seed) * f32x2(0.06711056, 0.00583715);
    let p = (x + seed_offset) * k + f32x2(k.y, k.x);
    let t = frac(p.x * p.y * (p.x + p.y));
    return -1.0 + 2.0 * frac(16.0 * k * t);
}

public func noised(f32x2 p, u32 seed) -> f32x3 {
    let i = floor(p);
    let f = frac(p);

    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    let du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    let ga = hash2(i + f32x2(0.0, 0.0), seed);
    let gb = hash2(i + f32x2(1.0, 0.0), seed);
    let gc = hash2(i + f32x2(0.0, 1.0), seed);
    let gd = hash2(i + f32x2(1.0, 1.0), seed);

    let va = dot(ga, f - f32x2(0.0, 0.0));
    let vb = dot(gb, f - f32x2(1.0, 0.0));
    let vc = dot(gc, f - f32x2(0.0, 1.0));
    let vd = dot(gd, f - f32x2(1.0, 1.0));

    let value = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
    let derivative = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                           du * (u.yx * (va - vb - vc + vd) + f32x2(vb, vc) - va);

    return f32x3(value, derivative);
}

public func fractal_noise(f32x2 p, f32 frequency, u32 octaves, f32 lacunarity, f32 gain, u32 seed) -> f32x3 {
    var result = f32x3(0.0);
    var amplitude = 1.0;
    var freq = frequency;

    for (var i = 0u; i < octaves; i++) {
        let octave = noised(p * freq, seed + i);
        result.x += octave.x * amplitude;
        result.yz += octave.yz * (amplitude * freq);
        amplitude *= gain;
        freq *= lacunarity;
    }

    return result;
}

func normalize_safe(f32x2 v) -> f32x2 {
    let len = length(v);
    return len > TERRAIN_EPSILON ? v / len : f32x2(0.0);
}

public func phacelle_noise(
    f32x2 p, f32x2 norm_dir, f32 frequency, f32 offset, f32 normalization, u32 seed
) -> f32x4 {
    let side_dir = norm_dir.yx * f32x2(-1.0, 1.0) * frequency * TAU;
    let phase_offset = offset * TAU;

    let p_int = floor(p);
    let p_frac = frac(p);

    var phase_dir = f32x2(0.0);
    var weight_sum = 0.0;

    for (var i = -1; i <= 2; i++) {
        for (var j = -1; j <= 2; j++) {
            let grid_offset = f32x2(f32(i), f32(j));
            let random_offset = hash2(p_int + grid_offset, seed) * 0.5;
            let from_cell_point = p_frac - grid_offset - random_offset;

            let sqr_dist = dot(from_cell_point, from_cell_point);
            let weight = max(0.0, exp(-sqr_dist * 2.0) - 0.01111);
            weight_sum += weight;

            let wave_input = dot(from_cell_point, side_dir) + phase_offset;
            phase_dir += f32x2(cos(wave_input), sin(wave_input)) * weight;
        }
    }

    let interpolated = phase_dir / max(weight_sum, TERRAIN_EPSILON);

    let magnitude = max(1.0 - normalization, length(interpolated));

    return f32x4(interpolated / magnitude, side_dir);
}

public struct ErosionResult {
    // x: height delta, yz: slope delta, w: accumulated magnitude
    public f32x4 height_slope_delta;
    // -1 at creases, +1 at ridges
    public f32 ridge_map;
};

public func erosion_filter(f32x2 p, f32x3 height_and_slope, f32 fade_target, in TerrainErosion params)
    -> ErosionResult {
    var strength = params.strength * params.scale;
    var target = clamp(fade_target, -1.0, 1.0);

    let input_height_and_slope = height_and_slope;
    var current = height_and_slope;
    var freq = 1.0 / (params.scale * params.cell_scale);
    let slope_length = max(length(current.yz), TERRAIN_EPSILON);
    var magnitude = 0.0;
    var rounding_mult = 1.0;

    let rounding_for_input = lerp(params.rounding.y, params.rounding.x, clamp01(target + 0.5)) *
                                   params.rounding.z;
    var combi_mask = ease_out(smooth_start(slope_length * params.onset.x, rounding_for_input * params.onset.x));

    var ridge_mask = ease_out(slope_length * params.onset.z);
    var ridge_target = target;

    var gully_slope = lerp(current.yz, current.yz / slope_length * params.assumed_slope.x, params.assumed_slope.y);

    for (var i = 0u; i < params.octaves; i++) {
        var phacelle = phacelle_noise(
            p * freq, normalize_safe(gully_slope), params.cell_scale, 0.25, params.normalization, params.seed + i
        );

        phacelle.zw *= -freq;

        let sloping = abs(phacelle.y);

        gully_slope += sign(phacelle.y) * phacelle.zw * strength * params.gully_weight;

        let gullies = f32x3(phacelle.x, phacelle.y * phacelle.zw);
        let faded_gullies = lerp(f32x3(target, 0.0, 0.0), gullies * params.gully_weight, combi_mask);

        current += faded_gullies * strength;
        magnitude += strength;

        target = faded_gullies.x;

        let rounding_for_octave = lerp(params.rounding.y, params.rounding.x, clamp01(phacelle.x + 0.5)) *
                                        rounding_mult;
        let new_mask = ease_out(smooth_start(sloping * params.onset.y, rounding_for_octave * params.onset.y));
        combi_mask = pow_inv(combi_mask, params.detail) * new_mask;

        ridge_target = lerp(ridge_target, gullies.x, ridge_mask);
        ridge_mask *= ease_out(sloping * params.onset.w);

        strength *= params.gain;
        freq *= params.lacunarity;
        rounding_mult *= params.rounding.w;
    }

    ErosionResult result;
    result.height_slope_delta = f32x4(current - input_height_and_slope, magnitude);
    result.ridge_map = ridge_target * (1.0 - ridge_mask);

    return result;
}
