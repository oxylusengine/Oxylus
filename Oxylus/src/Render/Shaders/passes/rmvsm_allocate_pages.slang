import common;
import gpu;
import scene;
import rmvsm;

[[vk::binding(0)]] RWStructuredBuffer<VSMPageAllocator> allocator;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32 thread_id : SV_DispatchThreadID) -> void {
    let has_request = thread_id < allocator[0].active_request_count;

    let local_offset = WavePrefixCountBits(has_request);
    let wave_total = WaveActiveCountBits(has_request);

    var wave_base = 0u;
    if (WaveIsFirstLane()) {
        wave_base = pop_free_page_slots(allocator[0], wave_total);
    }
    wave_base = WaveReadLaneFirst(wave_base);

    if (!has_request) {
        return;
    }

    let slot = wave_base + local_offset;
    let request = allocator[0].requests[thread_id];
    let page_data = page_tables.Load(request.page_table_address);
    var page = VSMPageMetadata(page_data);

    if (slot >= allocator[0].free_page_count) {
        page.set_alloc_failed(true);
        return;
    }

    let physical_page_index = allocator[0].free_page_list[slot];

    page.set_physical_address(physical_page_index);
    page.set_dirty(true);
    page.set_backed(true);
    page_tables.Store(request.page_table_address, page.data);
}
