import common;
import gpu;

#include <passes/histogram.slang>

[[vk::binding(0, 0)]]
Image2D<f32x4> src_image;

struct PushConstants {
    u32 *histogram;
    u32x2 src_image_size;
    f32 min_exposure;
    f32 max_exposure;
};
[[vk::push_constant]] PushConstants C;

groupshared u32 histogram_shared[HISTOGRAM_BIN_COUNT];

func bin_lum(f32 luminance) -> u32 {
    if (luminance < LUMINANCE_EPSILON) {
        return 0;
    }

    let mapped = com::remap(log2(luminance), C.min_exposure, C.max_exposure, 1.0, float(HISTOGRAM_BIN_COUNT - 1));
    return clamp(i32(mapped), 0, HISTOGRAM_BIN_COUNT - 1);
}

[[shader("compute")]]
[[numthreads(HISTOGRAM_THREADS_X, HISTOGRAM_THREADS_Y, 1)]]
func cs_main(u32 group_index : SV_GroupIndex, u32x3 thread_id : SV_DispatchThreadID) -> void {
    histogram_shared[group_index] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (all(thread_id.xy < C.src_image_size)) {
        let color = src_image.Load(u32x3(thread_id.xy, 0)).rgb;
        let luminance = dot(color, f32x3(0.2127, 0.7152, 0.0722));
        let bin_index = bin_lum(luminance);
        __atomic_add(histogram_shared[bin_index], 1, MemoryOrder::AcquireRelease);
    }

    GroupMemoryBarrierWithGroupSync();
    __atomic_add(C.histogram[group_index], histogram_shared[group_index], MemoryOrder::Relaxed);
}

