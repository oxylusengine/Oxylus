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
    const float saturation  = 1.3;
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
    const float3x3 LINEAR_REC2020_TO_LINEAR_SRGB =
        transpose(float3x3(float3(1.6605, -0.1246, -0.0182), float3(-0.5876, 1.1329, -0.1006), float3(-0.0728, -0.0083, 1.1187)));
    const float3x3 LINEAR_SRGB_TO_LINEAR_REC2020 =
        transpose(float3x3(float3(0.6274, 0.0691, 0.0164), float3(0.3293, 0.9195, 0.0880), float3(0.0433, 0.0113, 0.8956)));

    // LOG2_MIN      = -10.0
    // LOG2_MAX      =  +6.5
    // MIDDLE_GRAY   =  0.18
    const float AgxMinEv = -12.47393; // log2( pow( 2, LOG2_MIN ) * MIDDLE_GRAY )
    const float AgxMaxEv = 4.026069; // log2( pow( 2, LOG2_MAX ) * MIDDLE_GRAY )
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

f32x3x3 inverse(f32x3x3 m)
{
    f32 a = m[0].x, b = m[0].y, c = m[0].z;
    f32 d = m[1].x, e = m[1].y, f = m[1].z;
    f32 g = m[2].x, h = m[2].y, i = m[2].z;

    f32 A =  (e * i - f * h);
    f32 B = -(d * i - f * g);
    f32 C =  (d * h - e * g);
    f32 D = -(b * i - c * h);
    f32 E =  (a * i - c * g);
    f32 F = -(a * h - b * g);
    f32 G =  (b * f - c * e);
    f32 H = -(a * f - c * d);
    f32 I =  (a * e - b * d);

    f32 det = a * A + b * B + c * C;
    f32 invDet = 1.0 / det;

    return f32x3x3(
        A * invDet, D * invDet, G * invDet,
        B * invDet, E * invDet, H * invDet,
        C * invDet, F * invDet, I * invDet
    );
}

f32x3 color_XyYToXYZ(f32x3 xyY)
{
    f32 Y = xyY.z;
    f32 X = (xyY.x * Y) / xyY.y;
    f32 Z = ((1.0 - xyY.x - xyY.y) * Y) / xyY.y;
    return f32x3(X, Y, Z);
}

f32x3 color_Unproject(f32x2 xy)
{
    return color_XyYToXYZ(f32x3(xy.x, xy.y, 1.0));
}

f32x3x3 color_PrimariesToMatrix(f32x2 xy_red, f32x2 xy_green, f32x2 xy_blue, f32x2 xy_white)
{
    f32x3 XYZ_red   = color_Unproject(xy_red);
    f32x3 XYZ_green = color_Unproject(xy_green);
    f32x3 XYZ_blue  = color_Unproject(xy_blue);
    f32x3 XYZ_white = color_Unproject(xy_white);

    // Rows here = GLSL's columns (see gotcha above).
    f32x3x3 temp = f32x3x3(
        XYZ_red.x, XYZ_green.x, XYZ_blue.x,
        1.0,       1.0,         1.0,
        XYZ_red.z, XYZ_green.z, XYZ_blue.z
    );
    f32x3 scale = mul(inverse(temp), XYZ_white);

    // Same story: rows = GLSL's columns.
    return f32x3x3(
        XYZ_red.x * scale.x, XYZ_green.x * scale.y, XYZ_blue.x * scale.z,
        XYZ_red.y * scale.x, XYZ_green.y * scale.y, XYZ_blue.y * scale.z,
        XYZ_red.z * scale.x, XYZ_green.z * scale.y, XYZ_blue.z * scale.z
    );
}

f32x3x3 color_ComputeCompressionMatrix(f32x2 xyR, f32x2 xyG, f32x2 xyB, f32x2 xyW, f32 compression)
{
    f32 scale_factor = 1.0 / (1.0 - compression);
    f32x2 R = lerp(xyW, xyR, scale_factor);
    f32x2 G = lerp(xyW, xyG, scale_factor);
    f32x2 B = lerp(xyW, xyB, scale_factor);
    f32x2 W = xyW;

    return color_PrimariesToMatrix(R, G, B, W);
}

f32 color_DualSection(f32 x, f32 linear, f32 peak)
{
    // Length of linear section
    f32 S = peak * linear;
    if (x < S)
    {
        return x;
    }
    else
    {
        f32 C = peak / (peak - S);
        return peak - (peak - S) * exp((-C * (x - S)) / peak);
    }
}

f32x3 color_DualSection(f32x3 x, f32 linear, f32 peak)
{
    x.x = color_DualSection(x.x, linear, peak);
    x.y = color_DualSection(x.y, linear, peak);
    x.z = color_DualSection(x.z, linear, peak);
    return x;
}

f32x3 AgX_DS(f32x3 color_srgb)
{
    const let AGX_SATURATION = 1.3;
    const let AGX_COMPRESSION = 0.15;
    const let AGX_LINEAR = 0.10;
    const let AGX_PEAK = 1.0;
    f32x3 workingColor = max(color_srgb, 0.0);

    f32x3x3 sRGB_to_XYZ = color_PrimariesToMatrix(
        f32x2(0.64, 0.33),
        f32x2(0.3, 0.6),
        f32x2(0.15, 0.06),
        f32x2(0.3127, 0.3290));

    f32x3x3 adjusted_to_XYZ = color_ComputeCompressionMatrix(
        f32x2(0.64, 0.33),
        f32x2(0.3, 0.6),
        f32x2(0.15, 0.06),
        f32x2(0.3127, 0.3290),
        AGX_COMPRESSION);

    f32x3x3 XYZ_to_adjusted = inverse(adjusted_to_XYZ);
    f32x3x3 sRGB_to_adjusted = mul(sRGB_to_XYZ, XYZ_to_adjusted);

    workingColor = mul(sRGB_to_adjusted, workingColor);
    workingColor = clamp(color_DualSection(workingColor, AGX_LINEAR, AGX_PEAK), 0.0, 1.0);

    f32x3 luminanceWeight = f32x3(0.2126729, 0.7151522, 0.0721750);
    f32x3 desaturation = f32x3(dot(workingColor, luminanceWeight));
    workingColor = lerp(desaturation, workingColor, AGX_SATURATION);
    workingColor = clamp(workingColor, 0.0, 1.0);

    workingColor = mul(inverse(sRGB_to_adjusted), workingColor);

    return workingColor;
}

// -----------------------------------------------------------------------------
// Mode options.
// -----------------------------------------------------------------------------
#define TONE_MAPPING_UCS_ICTCP  0
#define TONE_MAPPING_UCS_JZAZBZ 1
#define TONE_MAPPING_UCS        TONE_MAPPING_UCS_ICTCP

// -----------------------------------------------------------------------------
// Defines the SDR reference white level used in our tone mapping (typically 250 nits).
// -----------------------------------------------------------------------------
#define GRAN_TURISMO_SDR_PAPER_WHITE 250.0f // cd/m^2

// -----------------------------------------------------------------------------
// Gran Turismo luminance-scale conversion helpers.
// In Gran Turismo, 1.0f in the linear frame-buffer space corresponds to
// REFERENCE_LUMINANCE cd/m^2 of physical luminance (typically 100 cd/m^2).
// -----------------------------------------------------------------------------
#define REFERENCE_LUMINANCE 100.0f // cd/m^2 <-> 1.0f

float
frameBufferValueToPhysicalValue(float fbValue)
{
    // Converts linear frame-buffer value to physical luminance (cd/m^2)
    // where 1.0 corresponds to REFERENCE_LUMINANCE (e.g., 100 cd/m^2).
    return fbValue * REFERENCE_LUMINANCE;
}

float
physicalValueToFrameBufferValue(float physical)
{
    // Converts physical luminance (cd/m^2) to a linear frame-buffer value,
    // where 1.0 corresponds to REFERENCE_LUMINANCE (e.g., 100 cd/m^2).
    return physical / REFERENCE_LUMINANCE;
}

// -----------------------------------------------------------------------------
// Utility functions.
// -----------------------------------------------------------------------------
float
smoothStep(float x, float edge0, float edge1)
{
    float t = (x - edge0) / (edge1 - edge0);

    if (x < edge0)
    {
        return 0.0f;
    }
    if (x > edge1)
    {
        return 1.0f;
    }

    return t * t * (3.0f - 2.0f * t);
}

float
chromaCurve(float x, float a, float b)
{
    return 1.0f - smoothStep(x, a, b);
}


// -----------------------------------------------------------------------------
// "GT Tone Mapping" curve with convergent shoulder.
// -----------------------------------------------------------------------------
struct GTToneMappingCurveV2
{
    float peakIntensity_;
    float alpha_;
    float midPoint_;
    float linearSection_;
    float toeStrength_;
    float kA_, kB_, kC_;

    [mutating]
    void initializeCurve(float monitorIntensity,
                         float alpha,
                         float grayPoint,
                         float linearSection,
                         float toeStrength)
    {
        peakIntensity_ = monitorIntensity;
        alpha_         = alpha;
        midPoint_      = grayPoint;
        linearSection_ = linearSection;
        toeStrength_   = toeStrength;

        // Pre-compute constants for the shoulder region.
        float k = (linearSection_ - 1.0f) / (alpha_ - 1.0f);
        kA_     = peakIntensity_ * linearSection_ + peakIntensity_ * k;
        kB_     = -peakIntensity_ * k * exp(linearSection_ / k);
        kC_     = -1.0f / (k * peakIntensity_);
    }

    float evaluateCurve(float x)
    {
        if (x < 0.0f)
        {
            return 0.0f;
        }

        float weightLinear = smoothStep(x, 0.0f, midPoint_);
        float weightToe    = 1.0f - weightLinear;

        // Shoulder mapping for highlights.
        float shoulder = kA_ + kB_ * exp(x * kC_);

        if (x < linearSection_ * peakIntensity_)
        {
            float toeMapped = midPoint_ * pow(x / midPoint_, toeStrength_);
            return weightToe * toeMapped + weightLinear * x;
        }
        else
        {
            return shoulder;
        }
    }
};

// -----------------------------------------------------------------------------
// EOTF / inverse-EOTF for ST-2084 (PQ).
// Note: Introduce exponentScaleFactor to allow scaling of the exponent in the EOTF for Jzazbz.
// -----------------------------------------------------------------------------
float
eotfSt2084(float n, float exponentScaleFactor = 1.0f)
{
    if (n < 0.0f)
    {
        n = 0.0f;
    }
    if (n > 1.0f)
    {
        n = 1.0f;
    }

    // Base functions from SMPTE ST 2084:2014
    // Converts from normalized PQ (0-1) to absolute luminance in cd/m^2 (linear light)
    // Assumes float input; does not handle integer encoding (Annex)
    // Assumes full-range signal (0-1)
    const float m1  = 0.1593017578125f;                // (2610 / 4096) / 4
    const float m2  = 78.84375f * exponentScaleFactor; // (2523 / 4096) * 128
    const float c1  = 0.8359375f;                      // 3424 / 4096
    const float c2  = 18.8515625f;                     // (2413 / 4096) * 32
    const float c3  = 18.6875f;                        // (2392 / 4096) * 32
    const float pqC = 10000.0f;                        // Maximum luminance supported by PQ (cd/m^2)

    // Does not handle signal range from 2084 - assumes full range (0-1)
    float np = pow(n, 1.0f / m2);
    float l  = np - c1;

    if (l < 0.0f)
    {
        l = 0.0f;
    }

    l = l / (c2 - c3 * np);
    l = pow(l, 1.0f / m1);

    // Convert absolute luminance (cd/m^2) into the frame-buffer linear scale.
    return physicalValueToFrameBufferValue(l * pqC);
}

float
inverseEotfSt2084(float v, float exponentScaleFactor = 1.0f)
{
    const float m1  = 0.1593017578125f;
    const float m2  = 78.84375f * exponentScaleFactor;
    const float c1  = 0.8359375f;
    const float c2  = 18.8515625f;
    const float c3  = 18.6875f;
    const float pqC = 10000.0f;

    // Convert the frame-buffer linear scale into absolute luminance (cd/m^2).
    float physical = frameBufferValueToPhysicalValue(v);
    float y        = physical / pqC; // Normalize for the ST-2084 curve

    float ym = pow(y, m1);
    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0f + c3 * ym)));
}

// -----------------------------------------------------------------------------
// ICtCp conversion.
// Reference: ITU-T T.302 (https://www.itu.int/rec/T-REC-T.302/en)
// -----------------------------------------------------------------------------
void
rgbToICtCp(f32x3 rgb, out f32x3 ictCp) // Input: linear Rec.2020
{
    float l = (rgb[0] * 1688.0f + rgb[1] * 2146.0f + rgb[2] * 262.0f) / 4096.0f;
    float m = (rgb[0] * 683.0f + rgb[1] * 2951.0f + rgb[2] * 462.0f) / 4096.0f;
    float s = (rgb[0] * 99.0f + rgb[1] * 309.0f + rgb[2] * 3688.0f) / 4096.0f;

    float lPQ = inverseEotfSt2084(l);
    float mPQ = inverseEotfSt2084(m);
    float sPQ = inverseEotfSt2084(s);

    ictCp[0] = (2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f;
    ictCp[1] = (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f;
    ictCp[2] = (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f;
}

void
iCtCpToRgb(f32x3 ictCp, out f32x3 rgb) // Output: linear Rec.2020
{
    float l = ictCp[0] + 0.00860904f * ictCp[1] + 0.11103f * ictCp[2];
    float m = ictCp[0] - 0.00860904f * ictCp[1] - 0.11103f * ictCp[2];
    float s = ictCp[0] + 0.560031f * ictCp[1] - 0.320627f * ictCp[2];

    float lLin = eotfSt2084(l);
    float mLin = eotfSt2084(m);
    float sLin = eotfSt2084(s);

    rgb[0] = max(3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin, 0.0f);
    rgb[1] = max(-0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin, 0.0f);
    rgb[2] = max(-0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin, 0.0f);
}

// -----------------------------------------------------------------------------
// Jzazbz conversion.
// Reference:
// Muhammad Safdar, Guihua Cui, Youn Jin Kim, and Ming Ronnier Luo,
// "Perceptually uniform color space for image signals including high dynamic
// range and wide gamut," Opt. Express 25, 15131-15151 (2017)
// Note: Coefficients adjusted for linear Rec.2020
// -----------------------------------------------------------------------------
#define JZAZBZ_EXPONENT_SCALE_FACTOR 1.7f // Scale factor for exponent

void
rgbToJzazbz(f32x3 rgb, out f32x3 jab) // Input: linear Rec.2020
{
    float l = rgb[0] * 0.530004f + rgb[1] * 0.355704f + rgb[2] * 0.086090f;
    float m = rgb[0] * 0.289388f + rgb[1] * 0.525395f + rgb[2] * 0.157481f;
    float s = rgb[0] * 0.091098f + rgb[1] * 0.147588f + rgb[2] * 0.734234f;

    float lPQ = inverseEotfSt2084(l, JZAZBZ_EXPONENT_SCALE_FACTOR);
    float mPQ = inverseEotfSt2084(m, JZAZBZ_EXPONENT_SCALE_FACTOR);
    float sPQ = inverseEotfSt2084(s, JZAZBZ_EXPONENT_SCALE_FACTOR);

    float iz = 0.5f * lPQ + 0.5f * mPQ;

    jab[0] = (0.44f * iz) / (1.0f - 0.56f * iz) - 1.6295499532821566e-11f;
    jab[1] = 3.524000f * lPQ - 4.066708f * mPQ + 0.542708f * sPQ;
    jab[2] = 0.199076f * lPQ + 1.096799f * mPQ - 1.295875f * sPQ;
}

void
jzazbzToRgb(f32x3 jab, out f32x3 rgb) // Output: linear Rec.2020
{
    float jz = jab[0] + 1.6295499532821566e-11f;
    float iz = jz / (0.44f + 0.56f * jz);
    float a  = jab[1];
    float b  = jab[2];

    float l = iz + a * 1.386050432715393e-1f + b * 5.804731615611869e-2f;
    float m = iz + a * -1.386050432715393e-1f + b * -5.804731615611869e-2f;
    float s = iz + a * -9.601924202631895e-2f + b * -8.118918960560390e-1f;

    float lLin = eotfSt2084(l, JZAZBZ_EXPONENT_SCALE_FACTOR);
    float mLin = eotfSt2084(m, JZAZBZ_EXPONENT_SCALE_FACTOR);
    float sLin = eotfSt2084(s, JZAZBZ_EXPONENT_SCALE_FACTOR);

    rgb[0] = lLin * 2.990669f + mLin * -2.049742f + sLin * 0.088977f;
    rgb[1] = lLin * -1.634525f + mLin * 3.145627f + sLin * -0.483037f;
    rgb[2] = lLin * -0.042505f + mLin * -0.377983f + sLin * 1.448019f;
}

// -----------------------------------------------------------------------------
// Unified color space (UCS): ICtCp or Jzazbz.
// -----------------------------------------------------------------------------
#if TONE_MAPPING_UCS == TONE_MAPPING_UCS_ICTCP
void
rgbToUcs(f32x3 rgb, out f32x3 ucs)
{
    rgbToICtCp(rgb, ucs);
}
void
ucsToRgb(f32x3 ucs, out f32x3 rgb)
{
    iCtCpToRgb(ucs, rgb);
}
#elif TONE_MAPPING_UCS == TONE_MAPPING_UCS_JZAZBZ
void
rgbToUcs(f32x3 rgb, out f32x3 ucs)
{
    rgbToJzazbz(rgb, ucs);
}
void
ucsToRgb(f32x3 ucs, out f32x3 rgb)
{
    jzazbzToRgb(ucs, rgb);
}
#else
#error "Unsupported TONE_MAPPING_UCS value. Please define TONE_MAPPING_UCS as either TONE_MAPPING_UCS_ICTCP or TONE_MAPPING_UCS_JZAZBZ."
#endif

// -----------------------------------------------------------------------------
// GT7 Tone Mapping class.
// -----------------------------------------------------------------------------
struct GT7ToneMapping
{
    float sdrCorrectionFactor_;

    float framebufferLuminanceTarget_;
    float framebufferLuminanceTargetUcs_; // Target luminance in UCS space
    GTToneMappingCurveV2 curve_;

    float blendRatio_;
    float fadeStart_;
    float fadeEnd_;

    // Initializes the tone mapping curve and related parameters based on the target display luminance.
    // This method should not be called directly. Use initializeAsHDR() or initializeAsSDR() instead.
    [mutating]
    void initializeParameters(float physicalTargetLuminance)
    {
        framebufferLuminanceTarget_ = physicalValueToFrameBufferValue(physicalTargetLuminance);

        // Initialize the curve (slightly different parameters from GT Sport).
        curve_.initializeCurve(framebufferLuminanceTarget_, 0.25f, 0.538f, 0.444f, 1.280f);

        // Default parameters.
        blendRatio_ = 0.6f;
        fadeStart_  = 0.98f;
        fadeEnd_    = 1.16f;

        f32x3 ucs;
        f32x3 rgb = { framebufferLuminanceTarget_,
                      framebufferLuminanceTarget_,
                      framebufferLuminanceTarget_ };
        rgbToUcs(rgb, ucs);
        framebufferLuminanceTargetUcs_ =
            ucs[0]; // Use the first UCS component (I or Jz) as luminance
    }

    // Initialize for HDR (High Dynamic Range) display.
    // Input: target display peak luminance in nits (range: 250 to 10,000)
    // Note: The lower limit is 250 because the parameters for GTToneMappingCurveV2
    //       were determined based on an SDR paper white assumption of 250 nits (GRAN_TURISMO_SDR_PAPER_WHITE).
    [mutating]
    void initializeAsHDR(float physicalTargetLuminance)
    {
        sdrCorrectionFactor_ = 1.0f;
        initializeParameters(physicalTargetLuminance);
    }

    // Initialize for SDR (Standard Dynamic Range) display.
    [mutating]
    void initializeAsSDR()
    {
        // Regarding SDR output:
        // First, in GT (Gran Turismo), it is assumed that a maximum value of 1.0 in SDR output
        // corresponds to GRAN_TURISMO_SDR_PAPER_WHITE (typically 250 nits).
        // Therefore, tone mapping for SDR output is performed based on GRAN_TURISMO_SDR_PAPER_WHITE.
        // However, in the sRGB standard, 1.0f corresponds to 100 nits,
        // so we need to "undo" the tone-mapped values accordingly.
        // To match the sRGB range, the tone-mapped values are scaled using sdrCorrectionFactor_.
        //
        // * These adjustments ensure that the visual appearance (in terms of brightness)
        //   stays generally consistent across both HDR and SDR outputs for the same rendered content.
        sdrCorrectionFactor_ = 1.0f / physicalValueToFrameBufferValue(GRAN_TURISMO_SDR_PAPER_WHITE);
        initializeParameters(GRAN_TURISMO_SDR_PAPER_WHITE);
    }

    // Input:  linear Rec.2020 RGB (frame buffer values)
    // Output: tone-mapped RGB (frame buffer values);
    //         - in SDR mode: mapped to [0, 1], ready for sRGB OETF
    //         - in HDR mode: mapped to [0, framebufferLuminanceTarget_], ready for PQ inverse-EOTF
    // Note: framebufferLuminanceTarget_ represents the display's target peak luminance converted to a frame buffer value.
    //       The returned values are suitable for applying the appropriate OETF to generate final output signal.
    void applyToneMapping(f32x3 rgb, out f32x3 out)
    {
        // Convert to UCS to separate luminance and chroma.
        f32x3 ucs;
        rgbToUcs(rgb, ucs);

        // Per-channel tone mapping ("skewed" color).
        f32x3 skewedRgb = { curve_.evaluateCurve(rgb[0]),
                            curve_.evaluateCurve(rgb[1]),
                            curve_.evaluateCurve(rgb[2]) };

        f32x3 skewedUcs;
        rgbToUcs(skewedRgb, skewedUcs);

        float chromaScale =
            chromaCurve(ucs[0] / framebufferLuminanceTargetUcs_, fadeStart_, fadeEnd_);

        const f32x3 scaledUcs = { skewedUcs[0],         // Luminance from skewed color
                                  ucs[1] * chromaScale, // Scaled chroma components
                                  ucs[2] * chromaScale };

        // Convert back to RGB.
        f32x3 scaledRgb;
        ucsToRgb(scaledUcs, scaledRgb);

        // Final blend between per-channel and UCS-scaled results.
        for (int i = 0; i < 3; ++i)
        {
            float blended = (1.0f - blendRatio_) * skewedRgb[i] + blendRatio_ * scaledRgb[i];
            // When using SDR, apply the correction factor.
            // When using HDR, sdrCorrectionFactor_ is 1.0f, so it has no effect.
            out[i] = sdrCorrectionFactor_ * min(blended, framebufferLuminanceTarget_);
        }
    }
};

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) {
    var color = input_image.SampleLevel(sampler, input.tex_coord, 0.0).rgb;

    if ((scene_flags & SceneFlags::HasEyeAdaptation) != 0) {
        let exposure = histogram_luminance.exposure;
        color *= exposure;
    }

    if ((scene_flags & SceneFlags::HasBloom) != 0) {
        let bloom = bloom_image.SampleLevel(sampler, input.tex_coord, 0.0).rgb;
        color += bloom;
    }

    switch (tonemap_type) {
        case TonemapType::ACES: {
            color = ACES_Fitted(color);
        } break;
        case TonemapType::AgX: {
            color = AgX_DS(color);
        } break;
        case TonemapType::GT7: {
            var gt7 = GT7ToneMapping();
            gt7.initializeAsSDR();
            // GT7 tone mapping operates in linear Rec.2020.
            f32x3 rec2020In = com::rec709_to_rec2020(color);
            f32x3 rec2020Out = f32x3(0.0);
            gt7.applyToneMapping(rec2020In, rec2020Out);
            color = com::rec2020_to_rec709(rec2020Out);
        } break;
        default:;
    }

    if ((scene_flags & SceneFlags::HasChromaticAberration) != 0) {
        let rgmag = FfxLensGetRGMag(C.post_process_settings.chromatic_aberration_amount);
        color = FfxLensSampleWithChromaticAberration(input_image, sampler, i32x2(input.position.xy), C.screen_size / 2, rgmag.x, rgmag.y);
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

    return f32x4(color, 1.0);
}
