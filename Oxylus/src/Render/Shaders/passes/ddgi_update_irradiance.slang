import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> irradiance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(4, 0)]] StructuredBuffer<u32> probe_update_list;
[[vk::binding(5, 0)]] StructuredBuffer<ProbeUpdateArgs> probe_update_args;

// this constant must fit into atlas's format (its currentlyy 10 bit)
static const f32 MIN_DARKENING_STEP = 1.0 / 1024.0;

static const f32 FIREFLY_FLOOR = 1e-4;

static const f32 HYSTERESIS_GAIN = 0.1;
// 1/GAMMA that is 2.5^0.2 - 1.
static const f32 HYSTERESIS_BALANCE_CHANGE = 0.2;
static const f32 HYSTERESIS_MAX_CHANGE = 4.0;

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform u32 radiance_atlas_y_offset,
    uniform f32 hysteresis,
    uniform f32 max_brightness_step,
    uniform f32 firefly_ratio,
    uniform f32 hysteresis_dark_bias
) -> void {
    let work = probe_texel_work(thread_id.xy, IRRADIANCE_TEXELS);
    if (work.list_index >= probe_update_args[0].count) {
        return;
    }

    let entry = probe_update_list[work.list_index];
    let volume = probe_volumes[probe_update_entry_volume(entry)];
    let probe_row = volume.probe_offset + probe_update_entry_index(entry);
    let flags = probe_states[probe_row].flags;

    let origin = probe_tile_origin(probe_row, IRRADIANCE_TEXELS);
    let coords = origin + 1 + work.texel;
    let previous = irradiance[coords];
    let history = com::sanitize(previous.rgb);
    let previous_confidence = previous.a > 0.0 ? min(previous.a, 1.0) : 0.0;
    let history_luminance = com::luminance(decode_probe_irradiance(history));
    let reset = probe_is_reset(flags);
    let firefly_limit = reset ? com::HALF_MAX : (history_luminance + FIREFLY_FLOOR) * firefly_ratio;

    let texel_direction = probe_texel_direction(work.texel, IRRADIANCE_TEXELS);
    let rotation = probe_ray_rotation(frame_index);

    var sum = f32x3(0.0);
    var weight_sum = 0.0;
    var backface_count = 0u;
    let updates_radiance = all(work.texel < u32x2(RADIANCE_TEXELS));
    let radiance_direction = probe_texel_direction(
        min(work.texel, u32x2(RADIANCE_TEXELS - 1u)),
        RADIANCE_TEXELS
    );
    var best_radiance_alignment = -2.0;
    var selected_radiance = f32x3(0.0);

    u32 ray_data_width, ray_data_height;
    ray_data.GetDimensions(ray_data_width, ray_data_height);
    let ray_origin = ray_data_row_origin(probe_row, rays_per_probe, ray_data_width);

    for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
        let ray = ray_data.Load(u32x3(ray_origin.x + ray_index, ray_origin.y, 0));
        var radiance = ray.rgb;
        let radiance_luminance = com::luminance(radiance);
        if (radiance_luminance > firefly_limit) {
            radiance *= firefly_limit / radiance_luminance;
        }

        let ray_direction = probe_ray_direction(ray_index, rays_per_probe, rotation);
        let alignment = dot(texel_direction, ray_direction);
        let radiance_alignment = dot(radiance_direction, ray_direction);
        if (updates_radiance && radiance_alignment > best_radiance_alignment) {
            best_radiance_alignment = radiance_alignment;
            selected_radiance = radiance;
        }

        if (ray.a < 0.0) {
            backface_count += 1;
            continue;
        }

        let weight = max(alignment, 0.0);
        if (weight <= 0.0) {
            continue;
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

    let result_texel = f32x4(history + lerp_delta, confidence);
    irradiance[coords] = result_texel;

    let border = probe_border_writes(work.texel, IRRADIANCE_TEXELS);
    if (border.has_x) {
        irradiance[origin + border.x_texel] = result_texel;
    }
    if (border.has_y) {
        irradiance[origin + border.y_texel] = result_texel;
    }
    if (border.has_corner) {
        irradiance[origin + border.corner_texel] = result_texel;
    }

    if (!updates_radiance) {
        return;
    }

    let radiance_origin = probe_radiance_tile_origin(probe_row, radiance_atlas_y_offset);
    let radiance_coords = radiance_origin + 1 + work.texel;
    let radiance_history = com::sanitize(irradiance[radiance_coords].rgb);
    let radiance_rate = reset ? 0.0 : confidence * 0.75;
    let radiance_root = lerp(
        sqrt(max(selected_radiance, f32x3(0.0))),
        sqrt(max(radiance_history, f32x3(0.0))),
        radiance_rate
    );
    let radiance_texel = f32x4(radiance_root * radiance_root, 1.0);
    irradiance[radiance_coords] = radiance_texel;

    let radiance_border = probe_border_writes(work.texel, RADIANCE_TEXELS);
    if (radiance_border.has_x) {
        irradiance[radiance_origin + radiance_border.x_texel] = radiance_texel;
    }
    if (radiance_border.has_y) {
        irradiance[radiance_origin + radiance_border.y_texel] = radiance_texel;
    }
    if (radiance_border.has_corner) {
        irradiance[radiance_origin + radiance_border.corner_texel] = radiance_texel;
    }
}
