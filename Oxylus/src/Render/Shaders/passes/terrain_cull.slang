import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

[[vk::constant_id(0)]] const u32 CULL_FLAGS = 0;

[[vk::binding(0)]] ConstantBuffer<TerrainData> terrain;
[[vk::binding(1)]] Image2D<f32x2> patch_minmax;
[[vk::binding(2)]] Image2D<f32> hiz_image;
[[vk::binding(3)]] RWStructuredBuffer<u32> visible_patches;
[[vk::binding(4)]] RWStructuredBuffer<u32> patch_visibility_mask;
[[vk::binding(5)]] RWStructuredBuffer<DrawIndirectCommand> draw_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESH_COUNT, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform CullCamera camera) -> void {
    let patch_index = thread_id.x;
    let patch_total = terrain.patch_count.x * terrain.patch_count.y;

    var visible = false;
    var emit = false;
    if (patch_index < patch_total) {
        let patch = u32x2(patch_index % terrain.patch_count.x, patch_index / terrain.patch_count.x);

        let corner_min = terrain.patch_corner(patch, u32x2(0, 0));
        let corner_max = terrain.patch_corner(patch, u32x2(1, 1));
        let bounds = patch_minmax.Load(i32x3(i32(patch.x), i32(patch.y), 0));

        let aabb_center = f32x3(
            (corner_min.x + corner_max.x) * 0.5,
            terrain.decode_height((bounds.x + bounds.y) * 0.5),
            (corner_min.y + corner_max.y) * 0.5
        );
        let aabb_extent = f32x3(
            corner_max.x - corner_min.x,
            max(terrain.height_scale * (bounds.y - bounds.x), 1e-3),
            corner_max.y - corner_min.y
        );

        let mask_word = patch_index / 32u;
        let mask_bit = 1u << (patch_index % 32u);
        let was_visible = (patch_visibility_mask[mask_word] & mask_bit) != 0u;

        visible = HAS_FLAG(CULL_FLAGS, CullFlag.LatePass) ? true : was_visible;

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestFrustum)) {
            visible = visible && test_frustum(camera.projection_view, aabb_center, aabb_extent);
        }

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion | CullFlag.LatePass) && visible) {
            if (let screen_aabb = project_aabb(camera.projection_view, camera.near_clip, aabb_center, aabb_extent)) {
                visible = !test_occlusion(screen_aabb, hiz_image);
            }
        }

        emit = visible && (!HAS_FLAG(CULL_FLAGS, CullFlag.LatePass) || !was_visible);

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion | CullFlag.LatePass)) {
            if (visible) {
                __atomic_or(patch_visibility_mask[mask_word], mask_bit, MemoryOrder::Relaxed);
            } else {
                __atomic_and(patch_visibility_mask[mask_word], ~mask_bit, MemoryOrder::Relaxed);
            }
        }
    }

    let lane_count = emit ? 1u : 0u;
    let lane_offset = WavePrefixSum(lane_count);
    let wave_count = WaveActiveSum(lane_count);

    var base = 0u;
    if (WaveIsFirstLane() && wave_count > 0u) {
        base = __atomic_add(draw_cmd[0].instance_count, wave_count, MemoryOrder::Relaxed);
    }
    base = WaveReadLaneFirst(base);

    if (emit) {
        visible_patches[base + lane_offset] = patch_index;
    }
}
