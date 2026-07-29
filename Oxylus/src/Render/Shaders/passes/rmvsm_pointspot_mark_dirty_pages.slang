import common;
import gpu;
import scene;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] Texture2DArray<u32> page_tables;
[[vk::binding(1)]] RWStructuredBuffer<DispatchIndirectCommand> clear_cmd;
[[vk::binding(2)]] RWStructuredBuffer<VSMPageAllocator> allocator;
[[vk::binding(3)]] RWStructuredBuffer<u32> layer_dirty_mask;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform VSMPointSpotContext ctx
) -> void {
    for (var mip = 0; mip < VSM_POINT_SPOT_MIP_COUNT; mip++) {
        let mip_size = u32(VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip);
        if (any(thread_id.xy >= mip_size)) {
            break;
        }

        let page = VSMPageMetadata(page_tables.Load(i32x4(thread_id, mip)));
        if (page.is_backed() && page.is_dirty()) {
            push_dirty_physical_page(allocator[0], page.get_physical_coords(VSM_PHYSICAL_PAGE_TABLE_SIZE, VSM_PAGE_SIZE));
            __atomic_add(clear_cmd[0].z, 1, MemoryOrder::Relaxed);
            __atomic_or(layer_dirty_mask[thread_id.z], 1u << u32(mip), MemoryOrder::Relaxed);
        }
    }
}
