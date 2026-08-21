import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWTexture2D<f32x2, 1, ImageFormat.RG16F> probe_distance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;

[[shader("compute")]]
[[numthreads(14, 14, 1)]]
func cs_main(
    u32x3 group_id : SV_GroupID,
    u32x3 thread_id : SV_GroupThreadID,
    uniform u32 volume_index,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform f32 hysteresis
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

    let max_distance = volume.max_probe_distance();
    let texel_direction = probe_texel_direction(thread_id.xy, DISTANCE_TEXELS);
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

        let hit_distance = ray_data.Load(u32x3(best_ray, probe_row, 0)).a;
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

            let hit_distance = ray_data.Load(u32x3(ray_index, probe_row, 0)).a;
            let distance = hit_distance < 0.0 ? 0.0 : min(hit_distance, max_distance);

            sum += f32x2(distance, distance * distance) * weight;
            weight_sum += weight;
        }

        if (weight_sum > 0.0) {
            result = sum / weight_sum;
        }
    }

    let coords = probe_tile_origin(probe_row, DISTANCE_TEXELS) + 1 + thread_id.xy;

    let previous = probe_distance[coords];
    let history = all(isfinite(previous)) ? previous : result;

    let rate = probe_is_reset(flags) ? 0.0 : hysteresis;

    probe_distance[coords] = lerp(result, history, rate);
}
