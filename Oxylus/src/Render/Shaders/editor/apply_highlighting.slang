import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
  Image2D<f32> edge_image;
  Image2D<f32x4> source_image;
  Sampler linear_sampler;
};

#include <fullscreen.slang>

func gaussian_blur(
    uv : f32x2,
    resolution : u32x2,
    source_texture : Texture2D<f32>,
    sampler_state : SamplerState,
    blur_radius : f32,
    sigma : f32
) -> f32 {
    var blurred_value = 0.0;
    var total_weight = 0.0;

    let kernel_radius = i32(ceil(blur_radius));
    let pixel_size = 1.0 / f32x2(resolution);

    for (var y = -kernel_radius; y <= kernel_radius; y++) {
        for (var x = -kernel_radius; x <= kernel_radius; x++) {
            // Calculate Gaussian weight
            let distance_sq = f32(x * x + y * y);
            let weight = exp(-distance_sq / (2.0 * sigma * sigma));

            // Sample texture with offset
            let sample_uv = uv + f32x2(x, y) * pixel_size * blur_radius;
            let sample_value = source_texture.Sample(sampler_state, sample_uv).r;

            blurred_value += sample_value * weight;
            total_weight += weight;
        }
    }

    return total_weight > 0.0 ? blurred_value / total_weight : 0.0;
}

[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  uniform ParameterBlock<ShaderParameters> params
) -> f32x4 {
    let source = params.source_image.Sample(params.linear_sampler, input.tex_coord);
    let outline = params.edge_image.Sample(params.linear_sampler, input.tex_coord).r;

    u32 width, height;
    params.edge_image.GetDimensions(width, height);
    let resolution = u32x2(width, height);
    let blurred_outline = gaussian_blur(
        input.tex_coord,
        resolution,
        params.edge_image,
        params.linear_sampler,
        1.0,  // blur radius
        1.0   // sigma
    );

    let outline_color = f32x4(0, 1, 0, 1);
    return lerp(source, outline_color, blurred_outline);
}
