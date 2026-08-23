import common;
import gpu;
import scene;
import rmvsm;

[[vk::binding(0)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(1)]] StructuredBuffer<u32> layer_dirty_mask;
[[vk::binding(2)]] RWStructuredBuffer<u32> active_layers;
[[vk::binding(3)]] RWStructuredBuffer<DispatchIndirectCommand> cull_meshes_cmd;

// indirect Y group count also serves as the active-layer append cursor
[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32 thread_id : SV_DispatchThreadID,
    uniform VSMPointSpotContext ctx
) -> void {
    if (thread_id >= VSM_POINT_SPOT_LAYER_COUNT) {
        return;
    }

    let view = views[thread_id];
    let has_dirty_pages = (layer_dirty_mask[thread_id] & (1u << u32(ctx.curr_mip))) != 0u;
    if (view.range <= 0.0 || !has_dirty_pages) {
        return;
    }

    let index = __atomic_add(cull_meshes_cmd[0].y, 1u, MemoryOrder::Relaxed);
    active_layers[index] = thread_id;
}
