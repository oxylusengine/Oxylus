import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<f32x4> source;
    Sampler linear_sampler;
};

struct PushConstants {
    // premultiplied
    f32x4 color;
    // the shadow's offset in uv
    f32x2 uv_offset;
    f32x2 uv_min;
    f32x2 uv_max;
};
[[vk::push_constant]] PushConstants c;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    let uv = input.tex_coord - c.uv_offset;
    let is_inside = all(uv >= c.uv_min) && all(uv <= c.uv_max);
    if (!is_inside) {
        return f32x4(0.0);
    }

    return params.source.Sample(params.linear_sampler, uv).a * c.color;
}
