import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWTexture2D<f32x2, 1, ImageFormat.RG16F> probe_distance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(4, 0)]] StructuredBuffer<u32> probe_update_list;
[[vk::binding(5, 0)]] StructuredBuffer<ProbeUpdateArgs> probe_update_args;

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform f32 hysteresis
) -> void {
    let work = probe_texel_work(thread_id.xy, DISTANCE_TEXELS);
    if (work.list_index >= probe_update_args[0].count) {
        return;
    }

    let entry = probe_update_list[work.list_index];
    let volume = probe_volumes[probe_update_entry_volume(entry)];
    let probe_row = volume.probe_offset + probe_update_entry_index(entry);
    let flags = probe_states[probe_row].flags;

    u32 ray_data_width, ray_data_height;
    ray_data.GetDimensions(ray_data_width, ray_data_height);
    let ray_origin = ray_data_row_origin(probe_row, rays_per_probe, ray_data_width);

    let max_distance = volume.max_probe_distance;
    let texel_direction = probe_texel_direction(work.texel, DISTANCE_TEXELS);
    let rotation = probe_ray_rotation(frame_index);

    var result = f32x2(max_distance, max_distance * max_distance);
    if (SHARP_DISTANCE) {
        var best_alignment = -1.0;
        var best_ray = 0u;
        for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
            let alignment = dot(texel_direction, probe_ray_direction(ray_index, rays_per_probe, rotation));
            if (alignment > best_alignment) {
                best_alignment = alignment;
                best_ray = ray_index;
            }
        }

        let hit_distance = ray_data.Load(u32x3(ray_origin.x + best_ray, ray_origin.y, 0)).a;
        let distance = hit_distance < 0.0 ? 0.0 : min(hit_distance, max_distance);

        result = f32x2(distance, distance * distance);
    } else {
        var sum = f32x2(0.0);
        var weight_sum = 0.0;
        for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
            let weight = pow(
                max(dot(texel_direction, probe_ray_direction(ray_index, rays_per_probe, rotation)), 0.0),
                DISTANCE_SHARPNESS
            );
            if (weight <= 0.0) {
                continue;
            }

            let hit_distance = ray_data.Load(u32x3(ray_origin.x + ray_index, ray_origin.y, 0)).a;
            let distance = hit_distance < 0.0 ? 0.0 : min(hit_distance, max_distance);

            sum += f32x2(distance, distance * distance) * weight;
            weight_sum += weight;
        }

        if (weight_sum > 0.0) {
            result = sum / weight_sum;
        }
    }

    let origin = probe_tile_origin(probe_row, DISTANCE_TEXELS);
    let coords = origin + 1 + work.texel;

    let previous = probe_distance[coords];
    let history = all(isfinite(previous)) ? previous : result;

    let rate = probe_is_reset(flags) ? 0.0 : hysteresis;

    let result_texel = lerp(result, history, rate);
    probe_distance[coords] = result_texel;

    let border = probe_border_writes(work.texel, DISTANCE_TEXELS);
    if (border.has_x) {
        probe_distance[origin + border.x_texel] = result_texel;
    }
    if (border.has_y) {
        probe_distance[origin + border.y_texel] = result_texel;
    }
    if (border.has_corner) {
        probe_distance[origin + border.corner_texel] = result_texel;
    }
}
