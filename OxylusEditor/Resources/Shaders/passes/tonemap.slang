module tonemap;

import common;
import gpu;

#include <passes/histogram.slang>

[[vk::binding(0, 0)]]
Sampler sampler;
[[vk::binding(1, 0)]]
Image2D<f32x4> input_image;

struct PushConstants {
    HistogramLuminance *luminance;
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

f32x3 ACES_Film(f32x3 x) {
    const f32 a = 2.51;
    const f32 b = 0.03;
    const f32 c = 2.43;
    const f32 d = 0.59;
    const f32 e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
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

f32x3 PBRNeutralToneMapping(f32x3 color) {
    const f32 startCompression = 0.8 - 0.04;
    const f32 desaturation = 0.15;

    f32 x = min(color.r, min(color.g, color.b));
    f32 offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    f32 peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const f32 d = 1.0 - startCompression;
    f32 newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    f32 g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak, g);
}

#define SRGB_2_XYZ_MAT \
transpose(float3x3( \
	0.4124564, 0.3575761, 0.1804375, \
    0.2126729, 0.7151522, 0.0721750, \
    0.0193339, 0.1191920, 0.9503041 \
))

float luminance_from_col(float3 color)
{
    float3 luminance_coefficients = transpose(SRGB_2_XYZ_MAT)[1];
    return dot(color, luminance_coefficients);
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
float3 agx_tonemapping(float3 color) {
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
    const float3x3 LINEAR_REC2020_TO_LINEAR_SRGB = transpose(float3x3(
        float3(1.6605, -0.1246, -0.0182),
        float3(-0.5876, 1.1329, -0.1006),
        float3(-0.0728, -0.0083, 1.1187)));
    const float3x3 LINEAR_SRGB_TO_LINEAR_REC2020 = transpose(float3x3(
        float3(0.6274, 0.0691, 0.0164),
        float3(0.3293, 0.9195, 0.0880),
        float3(0.0433, 0.0113, 0.8956)));
    // LOG2_MIN      = -10.0
    // LOG2_MAX      =  +6.5
    // MIDDLE_GRAY   =  0.18
    const float AgxMinEv = -12.47393; // log2( pow( 2, LOG2_MIN ) * MIDDLE_GRAY )
    const float AgxMaxEv = 4.026069;  // log2( pow( 2, LOG2_MAX ) * MIDDLE_GRAY )
    color = mul(LINEAR_SRGB_TO_LINEAR_REC2020, color);
    color = mul(AgXInsetMatrix, color);
    // Log2 encoding
    color = max(color, 1e-10); // avoid 0 or negative numbers for log2
    color = log2(color);
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    color = clamp(color, 0.0, 1.0);
    // Apply sigmoid
    color = agxDefaultContrastApproximation(color);
    // Apply AgX look
    agxLook(color);
    color = mul(AgXOutsetMatrix, color);
    // Linearize
    color = pow(max(float3(0.0), color), float3(2.2));
    color = mul(LINEAR_REC2020_TO_LINEAR_SRGB, color);
    // Gamut mapping. Simple clamp for now.
    color = clamp(color, 0.0, 1.0);
    return color;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) {
    f32x3 color = input_image.sample_mip(sampler, input.tex_coord, 0.0).rgb;
    const f32 exposure = C.luminance->exposure;
    color = color * (exposure + 1.0);

    //color = ACES_Film(color);
    //color = ACES_Fitted(color);
    //color = PBRNeutralToneMapping(color);  // this looks like shit, figure out why
    color = agx_tonemapping(color);

    return f32x4(color, 1.0);
}

