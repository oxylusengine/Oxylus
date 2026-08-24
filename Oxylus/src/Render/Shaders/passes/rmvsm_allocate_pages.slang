import common;
import gpu;
import scene;
import rmvsm;

[[vk::binding(0)]] RWStructuredBuffer<VSMPageAllocator> allocator;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;
[[vk::binding(2)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip0;
[[vk::binding(3)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip1;
[[vk::binding(4)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip2;
[[vk::binding(5)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip3;
[[vk::binding(6)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip4;
[[vk::binding(7)]] RWTexture2DArray<u32, ImageFormat::R32UI> pointspot_page_tables_mip5;

func load_pointspot_page(i32x3 address, i32 mip) -> u32 {
    switch (mip) {
        case 0: return pointspot_page_tables_mip0.Load(address);
        case 1: return pointspot_page_tables_mip1.Load(address);
        case 2: return pointspot_page_tables_mip2.Load(address);
        case 3: return pointspot_page_tables_mip3.Load(address);
        case 4: return pointspot_page_tables_mip4.Load(address);
        default: return pointspot_page_tables_mip5.Load(address);
    }
}

func store_pointspot_page(i32x3 address, i32 mip, u32 data) -> void {
    switch (mip) {
        case 0: pointspot_page_tables_mip0.Store(address, data); break;
        case 1: pointspot_page_tables_mip1.Store(address, data); break;
        case 2: pointspot_page_tables_mip2.Store(address, data); break;
        case 3: pointspot_page_tables_mip3.Store(address, data); break;
        case 4: pointspot_page_tables_mip4.Store(address, data); break;
        default: pointspot_page_tables_mip5.Store(address, data); break;
    }
}

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32 thread_id : SV_DispatchThreadID) -> void {
    let request_count = min(allocator[0].active_request_count, allocator[0].request_capacity);
    let has_request = thread_id < request_count;

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
    let is_directional = request.mip < 0;

    let page_data = is_directional //
                        ? page_tables.Load(request.page_table_address)
                        : load_pointspot_page(request.page_table_address, request.mip);
    var page = VSMPageMetadata(page_data);

    if (slot >= allocator[0].free_page_count) {
        page.set_alloc_failed(true);
    } else {
        let physical_page_index = allocator[0].free_page_list[slot];

        page.set_alloc_failed(false);
        page.set_physical_address(physical_page_index);
        page.set_dirty(true);
        page.set_backed(true);
    }

    if (is_directional) {
        page_tables.Store(request.page_table_address, page.data);
    } else {
        store_pointspot_page(request.page_table_address, request.mip, page.data);
    }
}
