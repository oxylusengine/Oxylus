import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
    Image2D<u32> visbuffer;
};

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
  uniform ParameterBlock<ShaderParameters> params,
  uniform u32x2 texel,
  uniform u32 *transform_id
) -> void {
    let texel = params.visbuffer.load(texel);
    if (texel == ~0u) {
        *transform_id = ~0u;
        return;
    }

    *transform_id = texel;
}
