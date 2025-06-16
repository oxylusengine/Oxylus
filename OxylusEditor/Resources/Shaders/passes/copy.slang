module copy;

import common;
import gpu;

[[vk::binding(0)]] Image2D<f32x4> src_image;
[[vk::binding(1)]] StorageImage2D<f32x4> dst_image;

[[vk::push_constant]] u32x3 extent;

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    if (any(extent < thread_id)) {
        return;
    }

    dst_image.Store(thread_id.xy, src_image.load(thread_id.xy, 0));
}
