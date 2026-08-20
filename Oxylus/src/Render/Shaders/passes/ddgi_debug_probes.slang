import common;
import gpu;
import scene;
import ddgi;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] Image2D<f32x4> irradiance;
[[vk::binding(3, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(4, 0)]] StructuredBuffer<ProbeState> probe_states;

struct PushConstants {
    u32 volume_index;
    u32 atlas_valid;
    f32 probe_radius;
};
[[vk::push_constant]] PushConstants C;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x3 normal : NORMAL;
    nointerpolation u32 probe_row : PROBE_ROW;
    nointerpolation u32 inactive : PROBE_INACTIVE;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index: SV_VertexID, u32 probe_index: SV_InstanceID) -> VertexOutput {
    let volume = probe_volumes[C.volume_index];
    let normal = debug_sphere_vertex(vertex_index);
    let world_position = probe_world_position(volume, probe_states, probe_index) + normal * C.probe_radius;
    let probe_row = volume.probe_offset + probe_index;

    VertexOutput output;
    output.position = mul(camera.projection_view, f32x4(world_position, 1.0));
    output.normal = normal;
    output.probe_row = probe_row;
    output.inactive = probe_states[probe_row].inactive;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let N = normalize(input.normal);
    if (C.atlas_valid == 0) {
        return f32x4(N * 0.5 + 0.5, 1.0);
    }

    if (input.inactive != 0) {
        return f32x4(0.4, 0.0, 0.0, 1.0);
    }

    return f32x4(sample_probe_irradiance(irradiance, linear_clamp_sampler, input.probe_row, N), 1.0);
}
