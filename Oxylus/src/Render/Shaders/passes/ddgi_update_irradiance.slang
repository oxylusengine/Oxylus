import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> irradiance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;

[[shader("compute")]]
[[numthreads(6, 6, 1)]]
func cs_main(
    u32x3 group_id: SV_GroupID,
    u32x3 thread_id: SV_GroupThreadID,
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

        sum += ray.rgb * weight;
        weight_sum += weight;
    }

    if (f32(backface_count) / f32(rays_per_probe) > PROBE_INSIDE_BACKFACE_RATIO) {
        return;
    }

    let result = weight_sum > 0.0 ? sum / weight_sum : f32x3(0.0);
    let coords = probe_tile_origin(probe_row, IRRADIANCE_TEXELS) + 1 + thread_id.xy;
    let history = com::sanitize(irradiance[coords].rgb);

    let rate = probe_is_reset(flags) ? 0.0 : pow(hysteresis, f32(probe_update_interval(flags)));

    irradiance[coords] = f32x4(lerp(result, history, rate), 1.0);
}
