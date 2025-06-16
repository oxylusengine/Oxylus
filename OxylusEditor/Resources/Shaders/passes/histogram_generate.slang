module histogram_generate;

import common;
import gpu;

#include <passes/histogram.slang>

[[vk::binding(0, 0)]]
Image2D<f32x4> src_image;

struct PushConstants {
    u32 *histogram;
    u32x2 src_image_size;
    f32 min_exposure;
    f32 exposure_range_inv;
};
[[vk::push_constant]] PushConstants C;

groupshared u32 histogram_shared[HISTOGRAM_BIN_COUNT];

func bin_lum(f32 luminance) -> u32 {
    if (luminance < LUMINANCE_EPSILON) {
        return 0;
    }

    const f32 log_luminance = saturate((log2(luminance) - C.min_exposure) * C.exposure_range_inv);
    return u32(log_luminance * f32(HISTOGRAM_BIN_COUNT - 1) + 1.0);
}

[[shader("compute")]]
[[numthreads(HISTOGRAM_THREADS_X, HISTOGRAM_THREADS_Y, 1)]]
func cs_main(u32 group_index : SV_GroupIndex, u32x3 thread_id : SV_DispatchThreadID) -> void {
    histogram_shared[group_index] = 0;
    com::control_barrier(com::memory_order_acq_rel);

    if (all(thread_id.xy < C.src_image_size)) {
        const f32x3 color = src_image.load(thread_id.xy).rgb;
        const f32 luminance = com::rec2020_to_xyz(color).y;
        const u32 bin_index = bin_lum(luminance);
        com::atomic_add(histogram_shared[bin_index], 1, com::memory_order_acq_rel);
    }

    com::control_barrier(com::memory_order_acq_rel);
    com::atomic_add(C.histogram[group_index], histogram_shared[group_index], com::memory_order_relaxed);
}

