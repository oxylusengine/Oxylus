import common;
import gpu;
import scene;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] RWTexture2D<f32> tile_depth;

groupshared u32 group_min_depth;

// the view depth of the farthest surface in each froxel tile, or a huge value where the tile only sees sky
[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 id : SV_DispatchThreadID, u32x3 group_id : SV_GroupID, u32 local_index : SV_GroupIndex) -> void {
    if (local_index == 0) {
        group_min_depth = 0x7f800000u;
    }

    GroupMemoryBarrierWithGroupSync();

    let extent = u32x2(camera.resolution);
    if (all(id.xy < extent)) {
        let depth = depth_image.Load(u32x3(id.xy, 0));
        // reverse-z, so the farthest surface has the smallest depth and nothing at all is 0
        InterlockedMin(group_min_depth, asuint(max(depth, 0.0)));
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_index != 0) {
        return;
    }

    let min_depth = asfloat(group_min_depth);
    var view_depth = 3.0e38;
    if (min_depth > 0.0 && min_depth < 1.0e30) {
        let tile_center = (f32x2(group_id.xy) + 0.5) * f32(FROXEL_TILE_SIZE);
        let uv = min(tile_center, camera.resolution - 1.0) / camera.resolution;
        view_depth = dot(camera.unproject_uv(uv, min_depth) - camera.position.xyz, camera.forward);
    }

    tile_depth.Store(group_id.xy, view_depth);
}
