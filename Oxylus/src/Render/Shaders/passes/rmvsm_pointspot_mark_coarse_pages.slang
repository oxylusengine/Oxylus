import common;
import gpu;
import scene;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_last_mip;
[[vk::binding(2)]] RWStructuredBuffer<u32> page_occupancy;
[[vk::binding(3)]] RWStructuredBuffer<VSMPageAllocator> allocator;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32 thread_id : SV_DispatchThreadID,
) -> void {
    if (thread_id >= VSM_POINT_SPOT_LAYER_COUNT) {
        return;
    }

    let view = views[thread_id];
    if (view.range <= 0.0) {
        return;
    }

    let address = i32x3(0, 0, thread_id);
    let last_mip = VSM_POINT_SPOT_MIP_COUNT - 1;
    let prev_page_data = __atomic_or(page_tables_last_mip[address], u32(VSMPageState.Visible), MemoryOrder::Relaxed);
    let prev_page = VSMPageMetadata(prev_page_data);

    if (!prev_page.is_visible()) {
        if (prev_page.is_backed()) {
            page_occupancy[prev_page.get_physical_address()] = 1u;
        } else {
            let alloc_request = VSMAllocRequest(address, last_mip);
            push_page_alloc_request(allocator[0], alloc_request);
        }
    }
}
