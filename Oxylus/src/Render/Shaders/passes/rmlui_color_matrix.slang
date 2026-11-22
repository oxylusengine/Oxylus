import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<f32x4> source;
    Sampler linear_sampler;
};

struct PushConstants {
    // rgb only, applied to premultiplied colour, so the constant column is scaled by alpha
    f32x4x4 color_matrix;
};
[[vk::push_constant]] PushConstants c;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    let color = params.source.Sample(params.linear_sampler, input.tex_coord);

    return f32x4(mul(c.color_matrix, color).rgb, color.a);
}
