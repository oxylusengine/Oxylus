import common;
import gpu;
import scene;
import visbuffer;

#include <defines.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] ConstantBuffer<TerrainData> terrain;
[[vk::binding(2)]] StructuredBuffer<u32> visible_patches;
[[vk::binding(3)]] Image2D<f32> heightmap;
[[vk::binding(4)]] SamplerState heightmap_sampler;

struct VertexOutput {
    f32x3 world_position;
};

struct PatchConstants {
    f32 edges[4] : SV_TessFactor;
    f32 inside[2] : SV_InsideTessFactor;
};

struct DomainOutput {
    f32x4 position : SV_Position;
};

func sample_height(f32x2 world_xz) -> f32 {
    let uv = terrain.world_to_uv(world_xz);
    return terrain.decode_height(heightmap.SampleLevel(heightmap_sampler, uv, 0.0));
}

func edge_tessellation(f32x3 p0, f32x3 p1) -> f32 {
    let center = (p0 + p1) * 0.5;
    let radius = distance(p0, p1) * 0.5;
    let view_distance = max(distance(center, camera.position.xyz) - radius, camera.near_clip);
    let projection_scale = camera.resolution.y * 0.5 / tan(radians(camera.fov) * 0.5);
    let projected_pixels = 2.0 * radius * projection_scale / view_distance;

    return clamp(projected_pixels / terrain.target_edge_pixels, 1.0, terrain.max_tessellation);
}

[[shader("vertex")]]
func vs_main(u32 vertex_index: SV_VertexID, u32 instance_index: SV_InstanceID) -> VertexOutput {
    const f32x2 corners[4] = { f32x2(0, 0), f32x2(1, 0), f32x2(1, 1), f32x2(0, 1) };

    let patch_index = visible_patches[instance_index];
    let patch = u32x2(patch_index % terrain.patch_count.x, patch_index / terrain.patch_count.x);
    let world_xz = terrain.patch_corner(patch, u32x2(corners[vertex_index]));

    VertexOutput output;
    output.world_position = f32x3(world_xz.x, sample_height(world_xz), world_xz.y);

    return output;
}

func hs_constants(InputPatch<VertexOutput, 4> patch) -> PatchConstants {
    let edge_u0 = edge_tessellation(patch[0].world_position, patch[3].world_position);
    let edge_v0 = edge_tessellation(patch[0].world_position, patch[1].world_position);
    let edge_u1 = edge_tessellation(patch[1].world_position, patch[2].world_position);
    let edge_v1 = edge_tessellation(patch[3].world_position, patch[2].world_position);

    PatchConstants output;
    output.edges[0] = edge_u0;
    output.edges[1] = edge_v0;
    output.edges[2] = edge_u1;
    output.edges[3] = edge_v1;
    output.inside[0] = max(edge_v0, edge_v1);
    output.inside[1] = max(edge_u0, edge_u1);

    return output;
}

[[shader("hull")]]
[[domain("quad")]]
[[partitioning("fractional_odd")]]
[[outputtopology("triangle_ccw")]]
[[outputcontrolpoints(4)]]
[[patchconstantfunc("hs_constants")]]
func hs_main(InputPatch<VertexOutput, 4> patch, u32 control_point_id: SV_OutputControlPointID) -> VertexOutput {
    return patch[control_point_id];
}

[[shader("domain")]]
[[domain("quad")]]
[[partitioning("fractional_odd")]]
[[outputtopology("triangle_ccw")]]
func ds_main(PatchConstants constants, f32x2 uv: SV_DomainLocation, const OutputPatch<VertexOutput, 4> patch)
    -> DomainOutput {
    let edge_v0 = lerp(patch[0].world_position, patch[1].world_position, uv.x);
    let edge_v1 = lerp(patch[3].world_position, patch[2].world_position, uv.x);
    let interpolated = lerp(edge_v0, edge_v1, uv.y);

    let world_position = f32x3(interpolated.x, sample_height(interpolated.xz), interpolated.z);

    DomainOutput output;
    output.position = camera.jitter_clip(mul(camera.projection_view, f32x4(world_position, 1.0)));

    return output;
}

[[shader("fragment")]]
func fs_main() -> u32 {
    return VisBufferData(TERRAIN_INSTANCE_ID, 0u).encode();
}
