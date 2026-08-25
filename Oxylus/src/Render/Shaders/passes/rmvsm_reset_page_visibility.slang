import common;
import gpu;
import rmvsm;

struct ShaderParameters {
    RWTexture2DArray<u32, 1, ImageFormat::R32UI> page_tables;
    StructuredBuffer<VirtualClipmap> clipmaps;
    StructuredBuffer<i32x2> clipmap_scroll_deltas;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform VSMContext ctx
) -> void {
    if (any(thread_id.xy >= ctx.get_page_table_extent().xy)) {
        return;
    }

    var page = VSMPageMetadata(params.page_tables.Load(thread_id));
    let clipmap = params.clipmaps[thread_id.z];
    let virtual_coords = i32x2(com::mod(i32x2(thread_id.xy) - clipmap.page_offset, ctx.page_table_size));
    let previous_virtual_coords = virtual_coords + params.clipmap_scroll_deltas[thread_id.z];
    let was_in_previous_clipmap = all(previous_virtual_coords >= 0) &&
                                   all(previous_virtual_coords < ctx.page_table_size);

    if (was_in_previous_clipmap) {
        page.set_visible(false);
        page.set_dirty(false);
        page.set_invalidated(false);
        page.set_alloc_failed(false);
    } else {
        page.reset();
        page.set_invalidated(true);
    }
    params.page_tables.Store(thread_id, page.data);
}
