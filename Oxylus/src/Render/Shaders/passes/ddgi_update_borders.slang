import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> irradiance;
[[vk::binding(2, 0)]] RWTexture2D<f32x2, 1, ImageFormat.RG16F> probe_distance;
[[vk::binding(3, 0)]] StructuredBuffer<ProbeState> probe_states;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 group_id: SV_GroupID, u32x3 thread_id: SV_GroupThreadID, uniform u32 volume_index) -> void {
    let volume = probe_volumes[volume_index];
    let probe_index = group_id.x;
    if (probe_index >= volume.probe_count) {
        return;
    }

    let probe_row = volume.probe_offset + probe_index;
    if (!probe_updates_this_frame(probe_states[probe_row].flags)) {
        return;
    }

    let index = thread_id.x;

    if (index < probe_border_texel_count(IRRADIANCE_TEXELS)) {
        let origin = probe_tile_origin(probe_row, IRRADIANCE_TEXELS);
        let texel = probe_border_texel(index, IRRADIANCE_TEXELS);
        irradiance[origin + texel] = irradiance[origin + probe_border_source(texel, IRRADIANCE_TEXELS)];
    }

    if (index < probe_border_texel_count(DISTANCE_TEXELS)) {
        let origin = probe_tile_origin(probe_row, DISTANCE_TEXELS);
        let texel = probe_border_texel(index, DISTANCE_TEXELS);
        probe_distance[origin + texel] = probe_distance[origin + probe_border_source(texel, DISTANCE_TEXELS)];
    }
}
