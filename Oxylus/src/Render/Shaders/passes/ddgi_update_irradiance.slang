import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> irradiance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;

// this constant must fit into atlas's format (its currentlyy 10 bit)
static const f32 MIN_DARKENING_STEP = 1.0 / 1024.0;

static const f32 FIREFLY_FLOOR = 1e-4;

static const f32 HYSTERESIS_GAIN = 0.1;
// 1/GAMMA that is 2.5^0.2 - 1.
static const f32 HYSTERESIS_BALANCE_CHANGE = 0.2;
static const f32 HYSTERESIS_MAX_CHANGE = 4.0;

[[shader("compute")]]
[[numthreads(6, 6, 1)]]
func cs_main(
    u32x3 group_id : SV_GroupID,
    u32x3 thread_id : SV_GroupThreadID,
    uniform u32 volume_index,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform f32 hysteresis,
    uniform f32 max_brightness_step,
    uniform f32 firefly_ratio,
    uniform f32 hysteresis_dark_bias
) -> void {
    let volume = probe_volumes[volume_index];
    let probe_index = group_id.x;
    if (probe_index >= volume.probe_count) {
        return;
    }

    let probe_row = volume.probe_offset + probe_index;
    let flags = probe_states[probe_row].flags;
    if (!probe_updates_this_frame(flags)) {
        return;
    }

    let coords = probe_tile_origin(probe_row, IRRADIANCE_TEXELS) + 1 + thread_id.xy;
    let previous = irradiance[coords];
    let history = com::sanitize(previous.rgb);
    let previous_confidence = previous.a > 0.0 ? min(previous.a, 1.0) : 0.0;
    let history_luminance = com::luminance(decode_probe_irradiance(history));
    let reset = probe_is_reset(flags);
    let firefly_limit = reset ? com::HALF_MAX : (history_luminance + FIREFLY_FLOOR) * firefly_ratio;

    let texel_direction = probe_texel_direction(thread_id.xy, IRRADIANCE_TEXELS);

    var sum = f32x3(0.0);
    var weight_sum = 0.0;
    var backface_count = 0u;
    for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
        let ray = ray_data.Load(u32x3(ray_index, probe_row, 0));
        if (ray.a < 0.0) {
            backface_count += 1;
            continue;
        }

        let weight = max(dot(texel_direction, probe_ray_direction(ray_index, rays_per_probe, frame_index)), 0.0);
        if (weight <= 0.0) {
            continue;
        }

        var radiance = ray.rgb;
        let radiance_luminance = com::luminance(radiance);
        if (radiance_luminance > firefly_limit) {
            radiance *= firefly_limit / radiance_luminance;
        }

        sum += radiance * weight;
        weight_sum += weight;
    }

    if (f32(backface_count) / f32(rays_per_probe) > PROBE_INSIDE_BACKFACE_RATIO) {
        return;
    }

    let estimate = weight_sum > 0.0 ? sum / weight_sum : f32x3(0.0);
    let result = encode_probe_irradiance(estimate);

    var confidence = 0.0;
    if (!reset) {
        let scale = min(history, result) + hysteresis_dark_bias;
        let relative = abs(history - result) / scale;
        let change = min(max(max(relative.r, relative.g), relative.b), HYSTERESIS_MAX_CHANGE);

        confidence = previous_confidence + HYSTERESIS_GAIN -
                     change * (HYSTERESIS_GAIN / HYSTERESIS_BALANCE_CHANGE);
        confidence = clamp(confidence, 0.0, hysteresis);
    }

    let rate = confidence;

    // darkening runs at full rate so a light going out registers at once
    var delta = result - history;
    if (!reset && com::luminance(delta) > max_brightness_step) {
        delta *= 0.25;
    }

    var lerp_delta = (1.0 - rate) * delta;

    let result_peak = max(max(result.r, result.g), result.b);
    let history_peak = max(max(history.r, history.g), history.b);
    if (result_peak < history_peak) {
        let min_step = max(history_peak, result_peak) * MIN_DARKENING_STEP;
        lerp_delta = min(max(abs(lerp_delta), min_step), abs(delta)) * sign(lerp_delta);
    }

    irradiance[coords] = f32x4(history + lerp_delta, confidence);
}
