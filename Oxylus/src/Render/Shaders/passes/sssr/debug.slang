import common;
import gpu;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(1, 0)]] Texture2D<f32x4> reflection_image;
[[vk::binding(2, 0)]] Texture2D<f32> confidence_image;
[[vk::binding(3, 0)]] Texture2D<f32> variance_image;
[[vk::binding(4, 0)]] Texture2D<f32> sample_count_image;

// the denoiser accumulates up to 32 samples
static const f32 MAX_SAMPLE_COUNT = 32.0;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform DebugView debug_view) -> f32x4 {
    switch (debug_view) {
        case DebugView::SSSRConfidence: {
            return f32x4(confidence_image.SampleLevel(linear_clamp_sampler, input.tex_coord, 0.0).rrr, 1.0);
        }
        case DebugView::SSSRVariance: {
            return f32x4(saturate(variance_image.SampleLevel(linear_clamp_sampler, input.tex_coord, 0.0)).rrr, 1.0);
        }
        case DebugView::SSSRSampleCount: {
            let sample_count = sample_count_image.SampleLevel(linear_clamp_sampler, input.tex_coord, 0.0);
            return f32x4((sample_count / MAX_SAMPLE_COUNT).rrr, 1.0);
        }
        default: {
            return f32x4(reflection_image.SampleLevel(linear_clamp_sampler, input.tex_coord, 0.0).rgb, 1.0);
        }
    }
}
