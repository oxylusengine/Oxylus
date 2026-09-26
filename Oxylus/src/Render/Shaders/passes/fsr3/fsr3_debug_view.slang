import common;
import gpu;
import fsr3;

// Diagnostic only: replaces the upscaled output with one of the upscaler's intermediates so an
// unstable stage can be identified by eye. The result still goes through tonemap and bloom, so
// absolute values read dark; what matters here is whether a view flickers frame to frame.
struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x2> dilated_motion_vectors;
    Texture2D<f32x4> dilated_reactive_masks;
    Texture2D<f32> luma_instability;
    Texture2D<f32> dilated_depth;
    Sampler sampler_linear_clamp;

    RWTexture2D<f32x4> output;
};
ParameterBlock<ShaderParameters> params;

struct PushConstants {
    u32 mode;
};

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform PushConstants pc) -> void {
    let pos = i32x2(thread_id.xy);
    if (!fsr3_is_on_screen(pos, params.constants.upscale_size)) {
        return;
    }

    let uv = (f32x2(pos) + 0.5) / f32x2(params.constants.upscale_size);
    let uv_hw = fsr3_clamp_uv(uv, params.constants.render_size, params.constants.max_render_size);

    let masks = params.dilated_reactive_masks.SampleLevel(params.sampler_linear_clamp, uv_hw, 0.0);

    var color = f32x3(0.0);

    if (pc.mode == 1u) {
        let mv = params.dilated_motion_vectors.SampleLevel(params.sampler_linear_clamp, uv_hw, 0.0);
        color = f32x3(0.5 + mv * f32x2(params.constants.render_size) * 0.5, 0.5);
    } else if (pc.mode == 2u) {
        color = f32x3(0.0, saturate(masks[DISOCCLUSION]), 0.0);
    } else if (pc.mode == 3u) {
        color = f32x3(0.0, saturate(masks[REACTIVE]), 0.0);
    } else if (pc.mode == 4u) {
        color = f32x3(saturate(masks[SHADING_CHANGE]), 0.0, 0.0);
    } else if (pc.mode == 5u) {
        color = f32x3(saturate(masks[ACCUMULATION]).xxx);
    } else if (pc.mode == 6u) {
        let instability = params.luma_instability.SampleLevel(params.sampler_linear_clamp, uv_hw, 0.0);
        color = f32x3(0.0, 0.0, saturate(instability));
    } else if (pc.mode == 7u) {
        let depth = params.dilated_depth.SampleLevel(params.sampler_linear_clamp, uv_hw, 0.0);
        color = f32x3(saturate(params.constants.get_view_space_depth_in_meters(depth) / 50.0).xxx);
    }

    // scaled so the views survive the tonemapper well enough to read
    params.output[thread_id.xy] = f32x4(color * 4.0, 1.0);
}
