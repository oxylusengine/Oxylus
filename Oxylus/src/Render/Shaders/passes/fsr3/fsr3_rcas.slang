import common;
import fsr3;

// limit beyond which sharpening starts producing unnatural results
static const f32 FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x4> rcas_input;
    ConstantBuffer<FSR3Exposure> exposure;

    RWTexture2D<f32x4> upscaled_output;
};
ParameterBlock<ShaderParameters> params;

struct PushConstants {
    // linear sharpness, already transformed out of the SDK's "stops" scale by the cpu side
    f32 sharpness;
};

func min3(f32 a, f32 b, f32 c) -> f32 { return min(a, min(b, c)); }
func max3(f32 a, f32 b, f32 c) -> f32 { return max(a, max(b, c)); }

func rcas_load(i32x2 p, f32 exposure) -> f32x3 {
    return params.rcas_input.Load(i32x3(p, 0)).rgb * exposure;
}

// the SDK runs this as 4 pixels per thread over a 16x16 tile with a quad swizzle for cache
// locality. one pixel per thread is the same math and the same output, just without the AMD
// specific remap, which needs bitfield helpers we would otherwise have to port for no result change
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform PushConstants pc) -> void {
    let pos = i32x2(thread_id.xy);
    if (!fsr3_is_on_screen(pos, params.constants.upscale_size)) {
        return;
    }

    let exposure = params.exposure.value();

    // minimal 3x3 neighbourhood
    //     b
    //   d e f
    //     h
    let b = rcas_load(pos + i32x2(0, -1), exposure);
    let d = rcas_load(pos + i32x2(-1, 0), exposure);
    let e = rcas_load(pos, exposure);
    let f = rcas_load(pos + i32x2(1, 0), exposure);
    let h = rcas_load(pos + i32x2(0, 1), exposure);

    // luma times 2
    let bl = b.b * 0.5 + (b.r * 0.5 + b.g);
    let dl = d.b * 0.5 + (d.r * 0.5 + d.g);
    let el = e.b * 0.5 + (e.r * 0.5 + e.g);
    let fl = f.b * 0.5 + (f.r * 0.5 + f.g);
    let hl = h.b * 0.5 + (h.r * 0.5 + h.g);

    // noise detection, so grain does not get sharpened along with real detail
    var nz = 0.25 * bl + 0.25 * dl + 0.25 * fl + 0.25 * hl - el;
    nz = saturate(abs(nz) * rcp(max3(max3(bl, dl, el), fl, hl) - min3(min3(bl, dl, el), fl, hl)));
    nz = -0.5 * nz + 1.0;

    // min and max of the ring
    let mn4 = min(min3(b, d, f), h);
    let mx4 = max(max3(b, d, f), h);

    const f32x2 peak_c = f32x2(1.0, -1.0 * 4.0);

    let lower_limiter_multiplier = saturate(el / min(min3(bl, dl, fl), hl));
    let hit_min = mn4 * rcp(4.0 * mx4) * lower_limiter_multiplier;
    let hit_max = (peak_c.x - mx4) * rcp(4.0 * mn4 + peak_c.y);
    let lobe_rgb = max(-hit_min, hit_max);

    var lobe = max(-FSR_RCAS_LIMIT, min(max3(lobe_rgb.r, lobe_rgb.g, lobe_rgb.b), 0.0)) * pc.sharpness;
    lobe *= nz;

    let rcp_l = rcp(4.0 * lobe + 1.0);
    let result = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l;

    params.upscaled_output[thread_id.xy] = f32x4(result / exposure, 1.0);
}
