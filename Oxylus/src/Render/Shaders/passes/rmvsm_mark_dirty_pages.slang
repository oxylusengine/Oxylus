import common;
import gpu;
import rmvsm;

struct ShaderParameters {
    Texture2DArray<u32> page_tables;

    RWStructuredBuffer<DispatchIndirectCommand> clear_cmd;
    RWStructuredBuffer<VSMPageAllocator> allocator;
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

    var page = VSMPageMetadata(params.page_tables.Load(i32x4(thread_id, 0)));
    if (page.is_backed() && page.is_dirty()) {
        push_dirty_physical_page(params.allocator[0], page.get_physical_coords(ctx.physcial_page_table_size, page_size));
        __atomic_add(params.clear_cmd[0].z, 1, MemoryOrder::Relaxed);
    }
}
