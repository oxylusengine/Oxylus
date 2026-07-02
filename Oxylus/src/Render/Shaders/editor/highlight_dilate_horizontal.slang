import common;

import visbuffer;
import gpu;
import scene;

struct DilateParameters {
  Texture2D<f32> input_mask;
  RWTexture2D<f32> output_mask; // horizontal mask
};

struct PushConstants {
  u32x2 resolution;
  i32 radius;
};

[[shader("compute")]] [[numthreads(16, 16, 1)]]
func cs_main(
  const uint3 id : SV_DispatchThreadID, uniform ParameterBlock<DilateParameters> params, uniform PushConstants config
) -> void {
  u32x2 pixel_coords = id.xy;
  if (pixel_coords.x >= config.resolution.x || pixel_coords.y >= config.resolution.y) {
    return;
  }

  f32 max_mask = 0.0;
  i32 r = max(1, config.radius);

  for (int x = -r; x <= r; x++) {
    i32 sample_x = clamp(int(pixel_coords.x) + x, 0, int(config.resolution.x) - 1);
    f32 val = params.input_mask.Load(i32x3(sample_x, pixel_coords.y, 0)).r;
    max_mask = max(max_mask, val);
  }

  params.output_mask[pixel_coords] = max_mask;
}
