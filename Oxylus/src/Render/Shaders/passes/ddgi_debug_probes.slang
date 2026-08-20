import common;
import gpu;
import scene;
import ddgi;

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] StructuredBuffer<ProbeVolume> probe_volumes;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x3 normal : NORMAL;
};

[[shader("vertex")]]
func vs_main(
    u32 vertex_index: SV_VertexID,
    u32 probe_index: SV_InstanceID,
    uniform u32 volume_index,
    uniform f32 probe_radius
) -> VertexOutput {
    let volume = probe_volumes[volume_index];
    let normal = debug_sphere_vertex(vertex_index);
    let world_position = volume.probe_position(probe_index) + normal * probe_radius;

    VertexOutput output;
    output.position = mul(camera.projection_view, f32x4(world_position, 1.0));
    output.normal = normal;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    // TODO: replace with the probe irradiance lookup along the normal.
    return f32x4(normalize(input.normal) * 0.5 + 0.5, 1.0);
}
