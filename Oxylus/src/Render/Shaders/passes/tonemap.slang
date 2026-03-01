import common;
import gpu;
import scene;

import lens;

#include <passes/histogram.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 tonemap_type = TonemapType::None;
[[vk::binding(0, 0)]] Sampler sampler;
[[vk::binding(1, 0)]] Image2D<f32x4> input_image;
[[vk::binding(2, 0)]] Image2D<f32x4> bloom_image;
[[vk::binding(3, 0)]] ConstantBuffer<HistogramLuminance> histogram_luminance;

struct PushConstants {
    PostProcessSettings post_process_settings;
    i32x2 screen_size;
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

static const f32x3x3 ACESInputMat = {
    { 0.59719, 0.35458, 0.04823 },
    { 0.07600, 0.90834, 0.01566 },
    { 0.02840, 0.13383, 0.83777 }
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const f32x3x3 ACESOutputMat = {
    { 1.60475, -0.53108, -0.07367 },
    { -0.10208, 1.10813, -0.00605 },
    { -0.00327, -0.07276, 1.07602 }
};

f32x3 RRTAndODTFit(f32x3 v) {
    f32x3 a = v * (v + 0.0245786f) - 0.000090537f;
    f32x3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

// ACES Filmic (Fitted)
f32x3 ACES_Fitted(f32x3 color) {
    color = mul(ACESInputMat, color);

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
    color = saturate(color);

    return color;
}

float luminance_from_col(float3 color) {
    return dot(color, com::REC709_TO_XYZ_MAT[1]);
}

float3 agxDefaultContrastApproximation(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return +15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
void agxLook(inout float3 color)
{
    const float3 slope      = (1.0f).xxx;
    const float3 power      = (1.1f).xxx;
    const float saturation  = 1.1;
    float luma = luminance_from_col(color);
    color = pow(color * slope, power);
    color = max(luma + saturation * (color - luma), float3(0.0));
}
float3 agx_tonemapping(float3 rec2020) {
    // AgX constants
    const float3x3 AgXInsetMatrix = transpose(float3x3(
        float3(0.856627153315983, 0.137318972929847, 0.11189821299995),
        float3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
        float3(0.0482516061458583, 0.101439036467562, 0.811302368396859)));
    // explicit AgXOutsetMatrix generated from Filaments AgXOutsetMatrixInv
    const float3x3 AgXOutsetMatrix = transpose(float3x3(
        float3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
        float3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
        float3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405)));
    // LOG2_MIN      = -10.0
    // LOG2_MAX      =  +6.5
    // MIDDLE_GRAY   =  0.18
    const float AgxMinEv = -12.47393; // log2( pow( 2, LOG2_MIN ) * MIDDLE_GRAY )
    const float AgxMaxEv = 4.026069;  // log2( pow( 2, LOG2_MAX ) * MIDDLE_GRAY )
    rec2020 = mul(AgXInsetMatrix, rec2020);
    // Log2 encoding
    rec2020 = max(rec2020, 1e-10); // avoid 0 or negative numbers for log2
    rec2020 = log2(rec2020);
    rec2020 = (rec2020 - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    rec2020 = clamp(rec2020, 0.0, 1.0);
    // Apply sigmoid
    rec2020 = agxDefaultContrastApproximation(rec2020);
    // Apply AgX look
    agxLook(rec2020);
    return mul(AgXOutsetMatrix, rec2020);
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) {
    var rec709 = input_image.SampleLevel(sampler, input.tex_coord, 0.0).rgb;

    if ((scene_flags & SceneFlags::HasChromaticAberration) != 0) {
        let rgmag = FfxLensGetRGMag(C.post_process_settings.chromatic_aberration_amount);
        rec709 = FfxLensSampleWithChromaticAberration(input_image, sampler, i32x2(input.position.xy), C.screen_size / 2, rgmag.x, rgmag.y);
    }

    if ((scene_flags & SceneFlags::HasBloom) != 0) {
        let bloom = bloom_image.SampleLevel(sampler, input.tex_coord, 0.0).rgb;
        rec709 += bloom;
    }

    if ((scene_flags & SceneFlags::HasEyeAdaptation) != 0) {
        let exposure = histogram_luminance.exposure;
        rec709 = rec709 * (exposure + 1.0);
    }

    switch (tonemap_type) {
        case TonemapType::ACES: {
            rec709 = ACES_Fitted(rec709);
        } break;
        case TonemapType::AgX: {
            var rec2020 = com::rec709_to_rec2020(rec709);
            rec2020 = agx_tonemapping(rec2020);
            rec2020 = pow(max(float3(0.0), rec2020), float3(2.2));
            rec2020 = clamp(rec2020, 0.0, 1.0);
            rec709 = com::rec2020_to_rec709(rec2020);
        } break;
        default:;
    }

    if ((scene_flags & SceneFlags::HasVignette) != 0) {
        FfxLensApplyVignette(i32x2(input.position.xy), C.screen_size / 2, rec709, C.post_process_settings.vignette_amount);
    }

    if ((scene_flags & SceneFlags::HasFilmGrain) != 0) {
        FfxLensApplyFilmGrain(i32x2(input.position.xy),
        rec709,
        C.post_process_settings.film_grain_scale,
        C.post_process_settings.film_grain_amount,
        C.post_process_settings.film_grain_seed);
    }

    return f32x4(rec709, 1.0);
}

