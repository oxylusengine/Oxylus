import common;
import gpu;
import fsr3;

static const i32 MIP_LEVELS_TO_USE = 3;

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    // luma pyramid built by fsr3_luma_pyramid: .x is this frame's luma, .y the delta against the
    // previous frame, so the product below is large only where shading actually changed
    Texture2D<f32x2> spd_mips;
    Sampler sampler_linear_clamp;

    RWTexture2D<f32> shading_change;
};
ParameterBlock<ShaderParameters> params;

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let px_pos = i32x2(thread_id.xy);
    let shading_change_size = params.constants.shading_change_render_size();

    if (!fsr3_is_on_screen(px_pos, shading_change_size)) {
        return;
    }

    u32 mip_width, mip_height, mip_levels;
    params.spd_mips.GetDimensions(0, mip_width, mip_height, mip_levels);

    let uv = (f32x2(px_pos) + 0.5) / f32x2(shading_change_size);
    let uv_jittered = uv + params.constants.jitter_offset / f32x2(params.constants.render_size);
    let mip_uv = fsr3_clamp_uv(uv_jittered, shading_change_size, i32x2(mip_width, mip_height));

    f32 samples[MIP_LEVELS_TO_USE];
    [ForceUnroll]
    for (i32 mip = SHADING_CHANGE_MIP_START; mip < MIP_LEVELS_TO_USE; mip++) {
        let s = params.spd_mips.SampleLevel(params.sampler_linear_clamp, mip_uv, f32(mip));
        samples[mip] = abs(s.x * s.y);
    }

    let scale = 1.0 + f32(SHADING_CHANGE_MIP_START) / f32(MIP_LEVELS_TO_USE);
    var shading_change = 0.0;
    [ForceUnroll]
    for (i32 mip = SHADING_CHANGE_MIP_START; mip < MIP_LEVELS_TO_USE; mip++) {
        if (samples[mip] > 0.0) {
            shading_change = max(shading_change, samples[mip]) * scale;
        }
    }

    params.shading_change[thread_id.xy] = saturate(shading_change);
}
