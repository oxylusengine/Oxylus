import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<f32x4> source;
    Image2D<f32x4> mask;
    Sampler linear_sampler;
};

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    let color = params.source.Sample(params.linear_sampler, input.tex_coord);

    return color * params.mask.Sample(params.linear_sampler, input.tex_coord).a;
}
