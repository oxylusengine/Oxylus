import common;
import gpu;
import scene;

import lens;

#include <passes/histogram.slang>
#include <passes/color_grading_common.slang>
#include <passes/tonemap_operators.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 tonemap_type = TonemapType::None;
[[vk::binding(0, 0)]] Sampler sampler;
[[vk::binding(1, 0)]] Image2D<f32x4> input_image;
[[vk::binding(2, 0)]] Image2D<f32x4> bloom_image;
[[vk::binding(3, 0)]] ConstantBuffer<HistogramLuminance> histogram_luminance;
[[vk::binding(4, 0)]] Image3D<f32x4> color_grading_lut;

struct PushConstants {
    PostProcessSettings post_process_settings;
    i32x2 screen_size;
    f32 bloom_intensity;
};
[[vk::push_constant]] PushConstants C;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
};

[[shader("vertex")]]
VertexOutput vs_main(u32 vertex_id : SV_VertexID) {
    VertexOutput output;
    output.tex_coord = f32x2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = f32x4(2.0 * output.tex_coord - 1.0, 1.0, 1.0);

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) {
    let source = input_image.SampleLevel(sampler, input.tex_coord, 0.0);
    var color = source.rgb;

    if ((scene_flags & SceneFlags::HasChromaticAberration) != 0) {
        let rgmag = FfxLensGetRGMag(C.post_process_settings.chromatic_aberration_amount);
        color = FfxLensSampleWithChromaticAberration(input_image, sampler, i32x2(input.position.xy), C.screen_size / 2, rgmag.x, rgmag.y);
    }

    if ((scene_flags & SceneFlags::HasEyeAdaptation) != 0) {
        let exposure = histogram_luminance.exposure;
        color *= exposure;
    } else {
        color *= C.post_process_settings.exposure;
    }

    if ((scene_flags & SceneFlags::HasBloom) != 0) {
        let bloom = bloom_image.SampleLevel(sampler, input.tex_coord, 0.0).rgb;
        color += bloom * C.bloom_intensity;
    }

    if ((scene_flags & SceneFlags::HasColorGrading) != 0) {
        color = color_grading_lut.SampleLevel(sampler, color_grading_lut_uvw(color), 0.0).rgb;
    } else {
        color = apply_tonemap_operator(color, tonemap_type);
    }

    if ((scene_flags & SceneFlags::HasVignette) != 0) {
        FfxLensApplyVignette(i32x2(input.position.xy), C.screen_size / 2, color, C.post_process_settings.vignette_amount);
    }

    if ((scene_flags & SceneFlags::HasFilmGrain) != 0) {
        FfxLensApplyFilmGrain(i32x2(input.position.xy),
        color,
        C.post_process_settings.film_grain_scale,
        C.post_process_settings.film_grain_amount,
        C.post_process_settings.film_grain_seed);
    }

    if ((scene_flags & SceneFlags::HasLetterbox) != 0) {
        let screen_aspect = f32(C.screen_size.x) / max(f32(C.screen_size.y), 1.0);
        let target_aspect = C.post_process_settings.letterbox_aspect;
        // fraction of the screen height one bar covers
        let bar = saturate(target_aspect > 0.0
            ? saturate(0.5 * (1.0 - screen_aspect / target_aspect)) * C.post_process_settings.letterbox_amount
            : C.post_process_settings.letterbox_amount * 0.5);

        // one pixel of feather at each edge, so the bar does not crawl while it animates
        let feather = 1.0 / max(f32(C.screen_size.y), 1.0);
        let v = input.tex_coord.y;
        let inside = min(smoothstep(bar - feather, bar + feather, v),
                         smoothstep(bar - feather, bar + feather, 1.0 - v));
        color = lerp(C.post_process_settings.letterbox_color, color, inside);
    }

    // last, so it covers the bars too
    if ((scene_flags & SceneFlags::HasScreenFade) != 0) {
        color = lerp(color, C.post_process_settings.fade_color, saturate(C.post_process_settings.fade_amount));
    }

    let alpha = ((scene_flags & SceneFlags::TransparentBackground) != 0) ? source.a : 1.0;
    return f32x4(color, alpha);
}
