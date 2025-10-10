import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
  Image2D<f32x4> grid_image;
  Image2D<f32x4> source_image;
  Sampler linear_sampler;
};

#include <fullscreen.slang>

[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  uniform ParameterBlock<ShaderParameters> params
) -> f32x4 {
    let source = params.source_image.Sample(params.linear_sampler, input.tex_coord);
    let grid = params.grid_image.Sample(params.linear_sampler, input.tex_coord);

    return source + grid;
}
