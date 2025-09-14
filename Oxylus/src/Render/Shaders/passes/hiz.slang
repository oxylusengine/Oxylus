import common;
import gpu;

struct ShaderParameters {
    Sampler sampler;
    Image2D<f32> src_image;
    StorageImage2D<f32, ImageFormat::R32F> dst_mip;
}

[[shader("compute")]]
[[numthreads(32, 32, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform u32x2 src_image_size,
    uniform u32 mip_index
) -> void {
    let c = params.src_image.SampleLevel(params.sampler, (f32x2(thread_id) + 0.5) / f32x2(src_image_size), mip_index).r;
    params.dst_mip.Store(thread_id.xy, c);
}
