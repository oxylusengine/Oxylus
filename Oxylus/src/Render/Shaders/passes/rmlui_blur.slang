import common;
import gpu;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<f32x4> source;
    Sampler linear_sampler;
};

struct PushConstants {
    // one texel along the blur direction
    f32x2 texel;
    // the region that holds the layer's pixels, everything past it reads as transparent
    f32x2 uv_min;
    f32x2 uv_max;
    f32 sigma;
    // taps either side of the centre, `step` texels apart so a wide blur keeps a bounded tap count
    i32 radius;
    i32 step;
};
[[vk::push_constant]] PushConstants c;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    if (c.radius == 0) {
        return params.source.Sample(params.linear_sampler, input.tex_coord);
    }

    var color = f32x4(0.0);
    var total = 0.0;
    for (i32 i = -c.radius; i <= c.radius; i++) {
        let distance = f32(i * c.step);
        let weight = exp(-distance * distance / (2.0 * c.sigma * c.sigma));
        let uv = input.tex_coord + distance * c.texel;
        let is_inside = all(uv >= c.uv_min) && all(uv <= c.uv_max);
        if (is_inside) {
            color += params.source.Sample(params.linear_sampler, uv) * weight;
        }

        total += weight;
    }

    return color / total;
}
