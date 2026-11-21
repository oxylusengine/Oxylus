import common;
import gpu;
import scene;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] RWStructuredBuffer<u32> decal_tiles;

struct CullPush {
    Decal *decals;
    u32 decal_count;
};
[[vk::push_constant]] CullPush push;

groupshared u32 depth_min_shared;
groupshared u32 depth_max_shared;
groupshared u32 tile_words_shared[DECAL_TILE_WORDS];

// the box of the tile's view slice in decal space, against the unit box. only the decal's axes separate,
// so a tile that spans a large depth range keeps more decals than it needs, never fewer
func overlaps_tile(in Decal decal, f32x2 uv_min, f32x2 uv_max, f32 depth_near, f32 depth_far) -> bool {
    var local_min = f32x3(1e30);
    var local_max = f32x3(-1e30);
    [ForceUnroll]
    for (u32 corner = 0; corner < 8; corner++) {
        let uv = f32x2((corner & 1u) != 0 ? uv_max.x : uv_min.x, (corner & 2u) != 0 ? uv_max.y : uv_min.y);
        let depth = (corner & 4u) != 0 ? depth_far : depth_near;
        let local = mul(decal.world_to_decal, f32x4(camera.unproject_uv(uv, depth), 1.0)).xyz;
        local_min = min(local_min, local);
        local_max = max(local_max, local);
    }

    return all(local_min <= 0.5) && all(local_max >= -0.5);
}

[[shader("compute")]]
[[numthreads(DECAL_TILE_SIZE, DECAL_TILE_SIZE, 1)]]
func cs_main(u32x2 tile : SV_GroupID, u32x2 pixel : SV_DispatchThreadID, u32 thread_index : SV_GroupIndex) -> void {
    if (thread_index == 0) {
        depth_min_shared = 0x7F7FFFFFu;
        depth_max_shared = 0u;
    }

    if (thread_index < DECAL_TILE_WORDS) {
        tile_words_shared[thread_index] = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    let resolution = u32x2(camera.resolution);
    if (all(pixel < resolution)) {
        // reversed z clears to 0, so only geometry widens the range and positive float bits sort like the floats
        let depth = depth_image.Load(u32x3(pixel, 0));
        if (depth > 0.0) {
            InterlockedMin(depth_min_shared, asuint(depth));
            InterlockedMax(depth_max_shared, asuint(depth));
        }
    }

    GroupMemoryBarrierWithGroupSync();

    let is_empty = depth_max_shared == 0u;
    if (!is_empty && thread_index < min(push.decal_count, MAX_DECALS)) {
        // a pixel of slack on each side covers the jitter the depth was rasterized with
        let texel = 1.0 / camera.resolution;
        let uv_min = (f32x2(tile * DECAL_TILE_SIZE) - 1.0) * texel;
        let uv_max = (f32x2((tile + 1u) * DECAL_TILE_SIZE) + 1.0) * texel;
        if (overlaps_tile(push.decals[thread_index], uv_min, uv_max, asfloat(depth_max_shared), asfloat(depth_min_shared))) {
            InterlockedOr(tile_words_shared[thread_index / 32u], 1u << (thread_index % 32u));
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (thread_index < DECAL_TILE_WORDS) {
        let tile_count_x = (resolution.x + DECAL_TILE_SIZE - 1u) / DECAL_TILE_SIZE;
        decal_tiles[(tile.y * tile_count_x + tile.x) * DECAL_TILE_WORDS + thread_index] = tile_words_shared[thread_index];
    }
}
