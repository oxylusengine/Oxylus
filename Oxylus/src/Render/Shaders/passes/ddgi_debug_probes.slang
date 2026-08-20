import common;
import gpu;
import scene;
import ddgi;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] Image2D<f32x4> ray_data;

struct PushConstants {
    u32 volume_index;
    u32 rays_per_probe;
    u32 frame_index;
    f32 probe_radius;
};
[[vk::push_constant]] PushConstants C;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x3 normal : NORMAL;
    nointerpolation u32 probe_row : PROBE_ROW;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index: SV_VertexID, u32 probe_index: SV_InstanceID) -> VertexOutput {
    let volume = probe_volumes[C.volume_index];
    let normal = debug_sphere_vertex(vertex_index);
    let world_position = volume.probe_position(probe_index) + normal * C.probe_radius;

    VertexOutput output;
    output.position = mul(camera.projection_view, f32x4(world_position, 1.0));
    output.normal = normal;
    output.probe_row = volume.probe_offset + probe_index;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let N = normalize(input.normal);
    if (C.rays_per_probe == 0) {
        return f32x4(N * 0.5 + 0.5, 1.0);
    }

    var radiance = f32x3(0.0);
    var weight_sum = 0.0;
    for (u32 ray_index = 0; ray_index < C.rays_per_probe; ray_index++) {
        let direction = probe_ray_direction(ray_index, C.rays_per_probe, C.frame_index);
        let weight = max(dot(direction, N), 0.0);
        if (weight <= 0.0) {
            continue;
        }

        let ray = ray_data.Load(u32x3(ray_index, input.probe_row, 0));
        if (ray.a < 0.0) {
            continue;
        }

        radiance += ray.rgb * weight;
        weight_sum += weight;
    }

    return f32x4(weight_sum > 0.0 ? radiance / weight_sum : f32x3(0.0), 1.0);
}
