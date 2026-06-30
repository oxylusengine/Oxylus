import common;

import visbuffer;
import gpu;
import scene;

struct CompositeParameters {
  Texture2D<f32> original_mask;
  Texture2D<f32> dilated_horiz_mask;
  Texture2D<f32> scene_depth;
  Texture2D<f32x4> scene_color;
  RWTexture2D<f32x4> out_color;
};

struct PushConstants {
  f32x4 outline_color; // RGBA
  u32x2 resolution;
  i32 outline_width;   // Dilate radius (pixels)
  i32 occluded_mode;   // 0 = Hide behind walls, 1 = Faded behind walls, 2 = Always visible
};

[[shader("compute")]] [[numthreads(16, 16, 1)]]
func cs_main(
  const u32x3 id : SV_DispatchThreadID, uniform ParameterBlock<CompositeParameters> params, uniform PushConstants config
) -> void {
  u32x2 pixel_coords = id.xy;
  if (pixel_coords.x >= config.resolution.x || pixel_coords.y >= config.resolution.y) {
    return;
  }

  i32 r = max(1, config.outline_width);
  f32 max_mask = 0.0;

  // 1D Vertical Sweep (Completes the 2D separable dilation)
  for (i32 y = -r; y <= r; y++) {
    i32 sample_y = clamp(int(pixel_coords.y) + y, 0, int(config.resolution.y) - 1);
    f32 val = params.dilated_horiz_mask.Load(i32x3(pixel_coords.x, sample_y, 0)).r;
    max_mask = max(max_mask, val);
  }

  // Isolate the outer edge boundary
  f32 center_mask = params.original_mask.Load(i32x3(pixel_coords, 0)).r;
  f32 edge_intensity = max_mask - center_mask;

  f32x4 back_color = params.scene_color.Load(i32x3(pixel_coords, 0));

  if (edge_intensity > 0.01) {
    f32 pixel_depth = params.scene_depth.Load(i32x3(pixel_coords, 0)).r;
    f32x4 final_outline = config.outline_color;

    bool is_occluded = (pixel_depth > 0.00001f);

    if (is_occluded && config.occluded_mode == 0) {
      params.out_color[pixel_coords] = back_color;
    } else if (is_occluded && config.occluded_mode == 1) {
      params.out_color[pixel_coords] = lerp(back_color, final_outline, 0.35);
    } else {
      params.out_color[pixel_coords] = final_outline;
    }
  } else {
    params.out_color[pixel_coords] = back_color;
  }
}
