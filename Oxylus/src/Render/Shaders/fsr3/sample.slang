implementing fsr3;

// the SDK builds these fetch/sample pairs out of DeclareCustomFetchBicubicSamples and friends,
// once per texture and once per precision. slang generics express the same thing directly, so the
// macro layer is gone and each call site just supplies a loader
public interface ITexelLoader {
    func load(i32x2 pos) -> f32x4;
};

public struct FetchedBilinearSamples {
    public f32x4 color00;
    public f32x4 color10;
    public f32x4 color01;
    public f32x4 color11;
};

public struct FetchedBicubicSamples {
    public f32x4 color00;
    public f32x4 color10;
    public f32x4 color20;
    public f32x4 color30;

    public f32x4 color01;
    public f32x4 color11;
    public f32x4 color21;
    public f32x4 color31;

    public f32x4 color02;
    public f32x4 color12;
    public f32x4 color22;
    public f32x4 color32;

    public f32x4 color03;
    public f32x4 color13;
    public f32x4 color23;
    public f32x4 color33;
};

public func fsr3_fetch_bicubic_samples<L : ITexelLoader>(L loader, i32x2 px_sample, i32x2 texture_size)
    -> FetchedBicubicSamples {
    FetchedBicubicSamples samples;

    samples.color00 = loader.load(fsr3_clamp_load(px_sample, i32x2(-1, -1), texture_size));
    samples.color10 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, -1), texture_size));
    samples.color20 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, -1), texture_size));
    samples.color30 = loader.load(fsr3_clamp_load(px_sample, i32x2(+2, -1), texture_size));

    samples.color01 = loader.load(fsr3_clamp_load(px_sample, i32x2(-1, +0), texture_size));
    samples.color11 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, +0), texture_size));
    samples.color21 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, +0), texture_size));
    samples.color31 = loader.load(fsr3_clamp_load(px_sample, i32x2(+2, +0), texture_size));

    samples.color02 = loader.load(fsr3_clamp_load(px_sample, i32x2(-1, +1), texture_size));
    samples.color12 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, +1), texture_size));
    samples.color22 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, +1), texture_size));
    samples.color32 = loader.load(fsr3_clamp_load(px_sample, i32x2(+2, +1), texture_size));

    samples.color03 = loader.load(fsr3_clamp_load(px_sample, i32x2(-1, +2), texture_size));
    samples.color13 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, +2), texture_size));
    samples.color23 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, +2), texture_size));
    samples.color33 = loader.load(fsr3_clamp_load(px_sample, i32x2(+2, +2), texture_size));

    return samples;
}

public func fsr3_fetch_bilinear_samples<L : ITexelLoader>(L loader, i32x2 px_sample, i32x2 texture_size)
    -> FetchedBilinearSamples {
    FetchedBilinearSamples samples;

    samples.color00 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, +0), texture_size));
    samples.color10 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, +0), texture_size));
    samples.color01 = loader.load(fsr3_clamp_load(px_sample, i32x2(+0, +1), texture_size));
    samples.color11 = loader.load(fsr3_clamp_load(px_sample, i32x2(+1, +1), texture_size));

    return samples;
}

public func fsr3_linear(f32x4 a, f32x4 b, f32 t) -> f32x4 {
    return a + (b - a) * t;
}

public func fsr3_bilinear(FetchedBilinearSamples samples, f32x2 px_frac) -> f32x4 {
    let color_x0 = fsr3_linear(samples.color00, samples.color10, px_frac.x);
    let color_x1 = fsr3_linear(samples.color01, samples.color11, px_frac.x);
    return fsr3_linear(color_x0, color_x1, px_frac.y);
}

public func fsr3_lanczos2_no_clamp(f32 x) -> f32 {
    const f32 pi = 3.141592653589793;
    return abs(x) < FSR3_EPSILON ? 1.0 : (sin(pi * x) / (pi * x)) * (sin(0.5 * pi * x) / (0.5 * pi * x));
}

public func fsr3_lanczos2(f32 x) -> f32 {
    return fsr3_lanczos2_no_clamp(min(abs(x), 2.0));
}

public func fsr3_lanczos2_approx_sq_no_clamp(f32 x2) -> f32 {
    let a = (2.0 / 5.0) * x2 - 1.0;
    let b = (1.0 / 4.0) * x2 - 1.0;
    return ((25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0)) * (b * b);
}

public func fsr3_lanczos2_approx_sq(f32 x2) -> f32 {
    return fsr3_lanczos2_approx_sq_no_clamp(min(x2, 4.0));
}

public func fsr3_lanczos2_approx_no_clamp(f32 x) -> f32 {
    return fsr3_lanczos2_approx_sq_no_clamp(x * x);
}

public func fsr3_lanczos2_approx(f32 x) -> f32 {
    return fsr3_lanczos2_approx_sq(x * x);
}

public func fsr3_lanczos2(f32x4 color0, f32x4 color1, f32x4 color2, f32x4 color3, f32 t) -> f32x4 {
    let weight0 = fsr3_lanczos2(-1.0 - t);
    let weight1 = fsr3_lanczos2(-0.0 - t);
    let weight2 = fsr3_lanczos2(+1.0 - t);
    let weight3 = fsr3_lanczos2(+2.0 - t);

    return (weight0 * color0 + weight1 * color1 + weight2 * color2 + weight3 * color3) /
           (weight0 + weight1 + weight2 + weight3);
}

// separable lanczos2 over the 4x4 neighbourhood, then deringed against the inner 2x2 so the
// negative lobes cannot overshoot past the samples that actually surround the target
public func fsr3_lanczos2(FetchedBicubicSamples samples, f32x2 px_frac) -> f32x4 {
    let color_x0 = fsr3_lanczos2(samples.color00, samples.color10, samples.color20, samples.color30, px_frac.x);
    let color_x1 = fsr3_lanczos2(samples.color01, samples.color11, samples.color21, samples.color31, px_frac.x);
    let color_x2 = fsr3_lanczos2(samples.color02, samples.color12, samples.color22, samples.color32, px_frac.x);
    let color_x3 = fsr3_lanczos2(samples.color03, samples.color13, samples.color23, samples.color33, px_frac.x);
    var color_xy = fsr3_lanczos2(color_x0, color_x1, color_x2, color_x3, px_frac.y);

    var deringing_min = samples.color11;
    var deringing_max = samples.color11;

    deringing_min = min(deringing_min, samples.color21);
    deringing_max = max(deringing_max, samples.color21);
    deringing_min = min(deringing_min, samples.color12);
    deringing_max = max(deringing_max, samples.color12);
    deringing_min = min(deringing_min, samples.color22);
    deringing_max = max(deringing_max, samples.color22);

    return clamp(color_xy, deringing_min, deringing_max);
}

// note the SDK's warning here: precision can make the base coordinate jitter by an ulp, so the
// clamp below is on the float position rather than the integer one
public func fsr3_sample_bicubic<L : ITexelLoader>(L loader, f32x2 uv_sample, i32x2 texture_size) -> f32x4 {
    var px_sample = (uv_sample * f32x2(texture_size)) - f32x2(0.5);
    let px_frac = frac(px_sample);

    px_sample.x = max(0.0, min(f32(texture_size.x - 1), px_sample.x));
    px_sample.y = max(0.0, min(f32(texture_size.y - 1), px_sample.y));

    let px_sample_i = i32x2(floor(px_sample));

    return fsr3_lanczos2(fsr3_fetch_bicubic_samples(loader, px_sample_i, texture_size), px_frac);
}
