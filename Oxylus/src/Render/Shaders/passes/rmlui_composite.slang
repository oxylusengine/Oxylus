import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Texture2D<f32x4> scene;
    // premultiplied, sized to the RmlUi context rather than the display
    Image2D<f32x4> ui;
    Sampler linear_sampler;
};
ParameterBlock<ShaderParameters> params;

struct PushConstants {
    u32 is_target_srgb;
};
[[vk::push_constant]] PushConstants c;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let pixel = i32x3(i32x2(input.position.xy), 0);
    var scene = params.scene.Load(pixel);
    let ui = params.ui.Sample(params.linear_sampler, input.tex_coord);

    let is_srgb = c.is_target_srgb != 0;
    if (is_srgb) {
        scene.rgb = com::linear_to_srgb(saturate(scene.rgb));
    }

    var result = ui + scene * (1.0 - ui.a);
    if (is_srgb) {
        result.rgb = com::srgb_to_linear(result.rgb);
    }

    return result;
}
