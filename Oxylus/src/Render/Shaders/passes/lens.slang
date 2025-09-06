// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/lens/ffx_lens.h#L264

module lens;

import common;
import scene;
import gpu;

u32x3 pcg3d16(u32x3 v) {
    v = v * 12829u + 47989u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v >>= 16u;
    return v;
}

// Simplex noise, transforms given position onto triangle grid
// This logic should be kept at 32-bit floating point precision. 16 bits causes artifacting.
f32x2 simplex(const in f32x2 P) {
    // Skew and unskew factors are a bit hairy for 2D, so define them as constants
    const f32 F2 = (sqrt(3.0) - 1.0) / 2.0;  // 0.36602540378
    const f32 G2 = (3.0 - sqrt(3.0)) / 6.0;  // 0.2113248654

    // Skew the (x,y) space to determine which cell of 2 simplices we're in
    f32   u   = (P.x + P.y) * F2;
    f32x2 Pi  = round(P + u);
    f32   v   = (Pi.x + Pi.y) * G2;
    f32x2 P0  = Pi - v;  // Unskew the cell origin back to (x,y) space
    f32x2 Pf0 = P - P0;  // The x,y distances from the cell origin

    return f32x2(Pf0);
}

f32x2 toFloat16(u32x2 inputVal) {
    return f32x2(inputVal * (1.0 / 65536.0) - 0.5);
}

f32x3 toFloat16(u32x3 inputVal) {
    return f32x3(inputVal * (1.0 / 65536.0) - 0.5);
}

/// Function call to calculate the red and green wavelength/channel sample offset values.
///
/// @param chromAbIntensity Intensity constant value for the chromatic aberration effect.
/// @return f32x2 containing the red and green wavelength/channel magnitude values
/// @ingroup FfxGPULens
public func FfxLensGetRGMag(f32 chromAbIntensity) -> f32x2 {
    const f32 A = 1.5220;
    const f32 B = 0.00459 * chromAbIntensity;  // um^2

    const f32 redWaveLengthUM   = 0.612;
    const f32 greenWaveLengthUM = 0.549;
    const f32 blueWaveLengthUM  = 0.464;

    const f32 redIdxRefraction   = A + B / (redWaveLengthUM * redWaveLengthUM);
    const f32 greenIdxRefraction = A + B / (greenWaveLengthUM * greenWaveLengthUM);
    const f32 blueIdxRefraction  = A + B / (blueWaveLengthUM * blueWaveLengthUM);

    const f32 redMag   = (redIdxRefraction - 1) / (blueIdxRefraction - 1);
    const f32 greenMag = (greenIdxRefraction - 1) / (blueIdxRefraction - 1);

    return f32x2(redMag, greenMag);
}

/// Function call to apply chromatic aberration effect when sampling the color input texture.
///
/// @param coord The input window coordinate [0, widthPixels), [0, heightPixels).
/// @param centerCoord The center window coordinate of the screen.
/// @param redMag Magnitude value for the offset calculation of the red wavelength (texture channel).
/// @param greenMag Magnitude value for the offset calculation of the green wavelength (texture channel).
/// @return The final sampled RGB color.
/// @ingroup FfxGPULens
public func FfxLensSampleWithChromaticAberration(Image2D<f32x4> input_image, Sampler sampler, i32x2 coord, i32x2 centerCoord, f32 redMag, f32 greenMag) -> f32x3 {
    f32x2 redShift = (coord - centerCoord) * redMag + centerCoord + 0.5;
    redShift *= rcp(2 * centerCoord);
    f32x2 greenShift = (coord - centerCoord) * greenMag + centerCoord + 0.5;
    greenShift *= rcp(2 * centerCoord);

    f32 red   = input_image.sample_mip(sampler, redShift, 0.0).r;
    f32 green = input_image.sample_mip(sampler, greenShift, 0.0).g;
    f32 blue  = input_image.sample_mip(sampler, coord * rcp(2 * centerCoord), 0.0).b;

    return f32x3(red, green, blue);
}

/// Function call to apply film grain effect to inout color. This call could be skipped entirely as the choice to use the film grain is optional.
///
/// @param coord The input window coordinate [0, widthPixels), [0, heightPixels).
/// @param color The current running color, or more clearly, the sampled input color texture color after being modified by chromatic aberration function.
/// @param grainScaleVal Scaling constant value for the grain's noise frequency.
/// @param grainAmountVal Intensity constant value of the grain effect.
/// @param grainSeedVal Seed value for the grain noise, for example, to change how the noise functions effect the grain frame to frame.
/// @ingroup FfxGPULens
public func FfxLensApplyFilmGrain(i32x2 coord, inout f32x3 color, f32 grainScaleVal, f32 grainAmountVal, uint grainSeedVal) -> void {
    f32x2     randomNumberFine = toFloat16(pcg3d16(u32x3(coord / (i32)(grainScaleVal / 8), grainSeedVal)).xy).xy;
    f32x2     simplexP         = simplex(coord / grainScaleVal + randomNumberFine);
    const f32 grainShape       = 3;

    f32 grain = 1 - 2 * exp2(-length(simplexP) * grainShape);

    color += grain * min(color, 1 - color) * grainAmountVal;
}

/// Function call to apply vignette effect to inout color. This call could be skipped entirely as the choice to use the vignette is optional.
///
/// @param coord The input window coordinate [0, widthPixels), [0, heightPixels).
/// @param centerCoord The center window coordinate of the screen.
/// @param color The current running color, or more clearly, the sampled input color texture color after being modified by chromatic aberration and film grain functions.
/// @param vignetteAmount Intensity constant value of the vignette effect.
/// @ingroup FfxGPULens
public func FfxLensApplyVignette(i32x2 coord, i32x2 centerCoord, inout f32x3 color, f32 vignetteAmount) -> void {
    f32x2 vignetteMask    = f32x2(0.0, 0.0);
    f32x2 coordFromCenter = abs(coord - centerCoord) / f32x2(centerCoord);

    const f32 piOver4 = PI * 0.25;
    vignetteMask = cos(coordFromCenter * vignetteAmount * piOver4);
    vignetteMask = vignetteMask * vignetteMask;
    vignetteMask = vignetteMask * vignetteMask;

    color *= clamp(vignetteMask.x * vignetteMask.y, 0, 1);
}