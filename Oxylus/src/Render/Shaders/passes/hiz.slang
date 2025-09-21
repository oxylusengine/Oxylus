import common;
import gpu;

[[vk::binding(0)]] Sampler sampler;
[[vk::binding(1)]] Texture2D<f32> src_image;
[[vk::binding(2)]] StorageImage2D<f32, ImageFormat::R32F> dst_mip;

[[shader("compute")]]
[[numthreads(32, 32, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform u32x2 src_image_size,
    uniform u32 mip_index,
) -> void {
    let c = src_image.SampleLevel(sampler, (f32x2(thread_id) + 0.5) / f32x2(src_image_size), mip_index).r;
    dst_mip.Store(thread_id.xy, c);
}