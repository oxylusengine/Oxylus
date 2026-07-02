import common;
import gpu;
import rmvsm;

struct ShaderParameters {
    RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;
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
    if (!page.is_visible() && page.is_backed()) {
        page.set_backed(false);
        params.page_tables.Store(thread_id, page.data);
    }
}
