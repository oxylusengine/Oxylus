import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] RWStructuredBuffer<ProbeUpdateArgs> probe_update_args;

func texel_update_groups_x(u32 count, u32 interior_texels) -> u32 {
    let probes_per_column = TEXEL_UPDATE_THREADS_Y / interior_texels;
    let columns = (count + probes_per_column - 1) / probes_per_column;
    let threads_x = columns * interior_texels;

    return (threads_x + TEXEL_UPDATE_GROUP - 1) / TEXEL_UPDATE_GROUP;
}

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main() -> void {
    let count = probe_update_args[0].count;

    DispatchIndirectCommand irradiance;
    irradiance.x = texel_update_groups_x(count, IRRADIANCE_TEXELS);
    irradiance.y = TEXEL_UPDATE_THREADS_Y / TEXEL_UPDATE_GROUP;
    irradiance.z = 1;

    DispatchIndirectCommand distance;
    distance.x = texel_update_groups_x(count, DISTANCE_TEXELS);
    distance.y = TEXEL_UPDATE_THREADS_Y / TEXEL_UPDATE_GROUP;
    distance.z = 1;

    DispatchIndirectCommand relocate;
    relocate.x = (count + PROBE_SELECT_GROUP - 1) / PROBE_SELECT_GROUP;
    relocate.y = 1;
    relocate.z = 1;

    probe_update_args[0].irradiance = irradiance;
    probe_update_args[0].distance = distance;
    probe_update_args[0].relocate = relocate;
}
