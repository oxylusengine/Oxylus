import common;
import gpu;
import cull;

[[vk::binding(0)]] Texture2D<f32> src_image;
[[vk::binding(1)]] StorageImage2D<f32, ImageFormat::R32F> dst_mip;

[[shader("compute")]]
[[numthreads(32, 32, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform u32x2 src_image_size,
) -> void {
    let uv = (f32x2(thread_id) + 0.5) / f32x2(src_image_size);
    let c = sample_level_min_reduction_2x2(src_image, uv, 0);
    dst_mip.Store(thread_id.xy, c);
}
