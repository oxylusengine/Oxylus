import common;
import gpu;
import rmvsm;

[[vk::constant_id(0)]] const bool IS_FIRST_PASS = false;
[[vk::binding(0)]] Texture2DArray<u32> page_tables;
[[vk::binding(1)]] Texture2DArray<u8> src_hpb;
[[vk::binding(2)]] RWTexture2DArray<u8, ImageFormat::R8UI> dst_hpb;

// per-mip occupancy pyramid for meshlet culling, its base predicate must match
// rmvsm_pointspot_draw_pages
[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32x3 dst_extent,
    uniform i32 page_table_mip
) -> void {
    if (any(thread_id >= dst_extent)) {
        return;
    }

    var occupied = false;
    if (IS_FIRST_PASS) {
        let page = VSMPageMetadata(page_tables.Load(i32x4(thread_id, page_table_mip)));
        occupied = page.is_backed() && page.is_dirty();
    } else {
        let tl = src_hpb.Load(i32x4(thread_id.xy * 2 + u32x2(0, 0), thread_id.z, 0));
        let tr = src_hpb.Load(i32x4(thread_id.xy * 2 + u32x2(0, 1), thread_id.z, 0));
        let bl = src_hpb.Load(i32x4(thread_id.xy * 2 + u32x2(1, 0), thread_id.z, 0));
        let br = src_hpb.Load(i32x4(thread_id.xy * 2 + u32x2(1, 1), thread_id.z, 0));
        occupied = (tl | tr | bl | br) == 1;
    }

    dst_hpb.Store(thread_id, occupied);
}
