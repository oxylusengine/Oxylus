import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<f32x4> source;
    Sampler linear_sampler;
};

struct PushConstants {
    // where the output's uv lands on the source
    f32x2 uv_offset;
    f32x2 uv_scale;
    // opacity, premultiplied so it scales every channel
    f32 factor;
};
[[vk::push_constant]] PushConstants c;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    let uv = c.uv_offset + input.tex_coord * c.uv_scale;

    return params.source.Sample(params.linear_sampler, uv) * c.factor;
}
