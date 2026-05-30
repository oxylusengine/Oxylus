import common;
import base;
import rmvsm;

[[vk::binding(0)]] Texture2DArray<u32> page_hash_table;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_table;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform VSMContext ctx
) -> void {
    if (any(thread_id.xy >= ctx.get_page_table_extent().xy)) {
        return;
    }

    let page_address = i32x3(thread_id);
    var page = VSMPageMetadata(page_table.Load(page_address));
    if (page.is_backed()) {
        page.reset();
        page.set_invalidated(true);
        page_table.Store(page_address, page.data);
    }
}