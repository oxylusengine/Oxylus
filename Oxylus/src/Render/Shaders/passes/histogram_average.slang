import common;
import gpu;

#include <passes/histogram.slang>

[[vk::binding(0, 0)]]
StructuredBuffer<u32> histogram_bin_indices;
[[vk::binding(1, 0)]]
RWStructuredBuffer<HistogramLuminance> luminance;

struct PushConstants {
    f32 pixel_count;
    f32 min_exposure;
    f32 exposure_range;
    f32 time_coeff;
    f32 ev100_bias;
};
[[vk::push_constant]] PushConstants C;

groupshared u32 weighted_sum_shared;

func ev100_from_luminance(f32 luminance) -> f32 {
    const f32 K = 12.5;
    const f32 ISO = 100.0;
    return log2(luminance * (ISO * C.ev100_bias / K));
}

[[shader("compute")]]
[[numthreads(HISTOGRAM_BIN_COUNT, 1, 1)]]
func cs_main(u32 gid : SV_GroupIndex) -> void {
    if (gid == 0) {
        weighted_sum_shared = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const u32 bin_count = histogram_bin_indices[gid];
    InterlockedAdd(weighted_sum_shared, bin_count * gid);

    GroupMemoryBarrierWithGroupSync();

    if (gid == 0) {
        const f32 dark_pixel_count = f32(histogram_bin_indices[0]);
        const f32 weighted_average_log2 = (f32(weighted_sum_shared) / max(C.pixel_count - dark_pixel_count, 1.0)) - 1.0;
        const f32 desired_luminance = exp2(((weighted_average_log2 / (HISTOGRAM_BIN_COUNT - 2)) * C.exposure_range) + C.min_exposure);
        const f32 last_luminance = luminance[0].adapted_luminance;
        const f32 adapted_luminance = last_luminance + (desired_luminance - last_luminance) * C.time_coeff;
        const f32 ev100 = ev100_from_luminance(adapted_luminance);
        luminance[0].adapted_luminance = adapted_luminance;
        luminance[0].exposure = 1.0 / (exp2(ev100) * 1.2);
    }
}
