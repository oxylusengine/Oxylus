module visbuffer_clear;

import common;
import gpu;
import visbuffer;

[[vk::binding(0, 0)]]
StorageImage2D<u32, ImageFormat::R32UI> visbuffer_image;

[[vk::binding(1, 0)]]
StorageImage2D<u32, ImageFormat::R32UI> overdraw_image;

[[vk::constant_id(0)]] const bool CLEAR_OVERDRAW = false;

struct PushConstants {
    u32x2 extent;
};
[[vk::push_constant]] PushConstants C;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
void cs_main(u32x2 thread_id : SV_DispatchThreadID) {
    if (any(thread_id >= C.extent)) {
        return;
    }

    const let vis_data = VisBufferData(~0u, ~0u);
    visbuffer_image[thread_id.xy] = vis_data.encode();
    if (CLEAR_OVERDRAW) {
        overdraw_image[thread_id.xy] = 0u;
    }
}
