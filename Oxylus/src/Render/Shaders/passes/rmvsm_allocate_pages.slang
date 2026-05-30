import common;
import gpu;
import scene;
import rmvsm;

struct ShaderParameters {
    ConstantBuffer<VSMPageAllocator> allocator;

    RWStructuredBuffer<u32> page_visibility_mask;
    RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;
};
ParameterBlock<ShaderParameters> params;

func poll_requests(u32 mask_count, u32 request_index) -> void {
    let request = params.allocator.requests[request_index];

    for (var i = 0; i < mask_count; i++) {
        let mask = params.page_visibility_mask[i];
        let bit = firstbitlow(~mask);
        if (bit != -1) {
            params.page_visibility_mask[i] |= 1 << bit;
            let physical_page_index = i * 32 + bit;

            let page_data = params.page_tables.Load(request.page_table_address);
            var page = VSMPageMetadata(page_data);
            page.set_physical_address(physical_page_index);
            page.set_dirty(true);
            page.set_backed(true);
            params.page_tables.Store(request.page_table_address, page.data);
            return;
        }
    }
}

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform u32 visible_bit_mask_count,
) -> void {
    for (var i = 0; i < params.allocator.active_request_count; i++) {
        poll_requests(visible_bit_mask_count, i);
    }
}
