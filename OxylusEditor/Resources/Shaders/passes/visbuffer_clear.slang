module visbuffer_clear;

import common;
import gpu;
import passes.visbuffer;

[[vk::binding(0, 0)]]
StorageImage2D<u32, ImageFormat::R32UI> visbuffer_data_image;

[[vk::binding(1, 0)]]
StorageImage2D<u32, ImageFormat::R32UI> overdraw_image;

struct PushConstants {
    u32x2 extent;
};
[[vk::push_constant]] PushConstants C;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
void cs_main(u32x2 thread_id : SV_DispatchThreadID) {
    if (any(C.extent < thread_id)) {
        return;
    }

    const let vis_data = VisBufferData(~0u, ~0u);
    const let vis = VisBuffer(vis_data, 1.0);

    // visbuffer_image[thread_id.xy] = vis.encode();
    visbuffer_data_image[thread_id.xy] = vis_data.encode();
    overdraw_image[thread_id.xy] = 0u;
}
