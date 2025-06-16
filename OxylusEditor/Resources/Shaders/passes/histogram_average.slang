module histogram_average;

import common;
import gpu;

#include <passes/histogram.slang>

struct PushConstants {
    u32 *histogram;
    HistogramLuminance *luminance;
    f32 pixel_count;
    f32 min_exposure;
    f32 exposure_range;
    f32 time_coeff;
    f32 ev100_bias;
};
[[vk::push_constant]] PushConstants C;

groupshared f32 histogram_shared[HISTOGRAM_BIN_COUNT];

func ev100_from_luminance(f32 luminance) -> f32 {
    const f32 K = 12.5;
    const f32 ISO = 100.0;
    return log2(luminance * (ISO * C.ev100_bias / K));
}

[[shader("compute")]]
[[numthreads(HISTOGRAM_BIN_COUNT, 1, 1)]]
func cs_main(u32 gid : SV_GroupIndex) -> void {
    const let count_for_this_bin = gid == 0 ? 0.0 : f32(C.histogram[gid]);
    histogram_shared[gid] = count_for_this_bin * f32(gid);
    com::control_barrier(com::memory_order_acq_rel);

    [[unroll]]
    for (u32 cutoff = (HISTOGRAM_BIN_COUNT >> 1); cutoff > 0; cutoff >>= 1) {
        if (gid < cutoff) {
            histogram_shared[gid] += histogram_shared[gid + cutoff];
        }

        com::control_barrier(com::memory_order_acq_rel);
    }

    if (gid == 0) {
        const f32 weighted_average_log2 = (histogram_shared[0] / max(C.pixel_count - count_for_this_bin, 1.0)) - 1.0;
        const f32 desired_luminance = exp2(((weighted_average_log2 / (HISTOGRAM_BIN_COUNT - 1)) * C.exposure_range) + C.min_exposure);
        const f32 last_luminance = C.luminance->adapted_luminance;
        const f32 adapted_luminance = last_luminance + (desired_luminance - last_luminance) * C.time_coeff;
        const f32 ev100 = ev100_from_luminance(adapted_luminance);
        C.luminance->adapted_luminance = adapted_luminance;
        C.luminance->exposure = 1.0 / (exp2(ev100) * 1.2);
    }
}

