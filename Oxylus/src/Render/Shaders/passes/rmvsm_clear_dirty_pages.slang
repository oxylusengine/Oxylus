import common;
import gpu;
import rmvsm;

struct ShaderParameters {
    ConstantBuffer<VSMPageAllocator> allocator;
    RWTexture2D<f32, ImageFormat::R32F> physical_pages;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform VSMContext ctx,
) -> void {
    if (any(thread_id.xy >= ctx.page_size)) {
        return;
    }

    let physical_page_coords = params.allocator.dirty_physical_page_coords[thread_id.z];
    let physical_page_offset = physical_page_coords * ctx.page_size;
    let physical_texel = physical_page_offset + thread_id.xy;
    params.physical_pages.Store(physical_texel, 1.0);
}
