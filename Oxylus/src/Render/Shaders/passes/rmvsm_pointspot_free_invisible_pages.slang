import common;
import gpu;
import scene;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip0;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip1;
[[vk::binding(2)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip2;
[[vk::binding(3)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip3;
[[vk::binding(4)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip4;
[[vk::binding(5)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip5;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID
) -> void {
    for (var mip = 0; mip < VSM_POINT_SPOT_MIP_COUNT; mip++) {
        let mip_size = u32(VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip);
        if (any(thread_id.xy >= mip_size)) {
            break;
        }

        let address = i32x3(thread_id);
        var page_data = 0u;
        switch (mip) {
            case 0: page_data = page_tables_mip0.Load(address); break;
            case 1: page_data = page_tables_mip1.Load(address); break;
            case 2: page_data = page_tables_mip2.Load(address); break;
            case 3: page_data = page_tables_mip3.Load(address); break;
            case 4: page_data = page_tables_mip4.Load(address); break;
            default: page_data = page_tables_mip5.Load(address); break;
        }

        var page = VSMPageMetadata(page_data);
        if (!page.is_visible() && page.is_backed()) {
            page.set_backed(false);
            switch (mip) {
                case 0: page_tables_mip0.Store(address, page.data); break;
                case 1: page_tables_mip1.Store(address, page.data); break;
                case 2: page_tables_mip2.Store(address, page.data); break;
                case 3: page_tables_mip3.Store(address, page.data); break;
                case 4: page_tables_mip4.Store(address, page.data); break;
                default: page_tables_mip5.Store(address, page.data); break;
            }
        }
    }
}
