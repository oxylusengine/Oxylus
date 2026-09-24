import common;

import visbuffer;
import gpu;
import scene;

struct CompositeParameters {
  Texture2D<f32> original_mask;
  Texture2D<f32> dilated_horiz_mask;
  Texture2D<f32> scene_depth;
  Texture2D<f32x4> scene_color;
  Sampler linear_sampler;
};

struct PushConstants {
  f32x4 outline_color; // RGBA
  u32x2 resolution;
  // The silhouette masks come from the visbuffer, so they sit at render resolution and have to be
  // indexed separately from the display sized output whenever an upscaler is running. Must stay in
  // sync with the PC struct in ViewportPanel.cpp.
  u32x2 mask_resolution;
  i32 outline_width;   // Dilate radius (mask pixels)
  i32 occluded_mode;   // 0 = Hide behind walls, 1 = Faded behind walls, 2 = Always visible
};

#include <fullscreen.slang>

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<CompositeParameters> params, uniform PushConstants config)
  -> f32x4 {
  u32x2 pixel_coords = u32x2(input.position.xy);

  if (pixel_coords.x >= config.resolution.x || pixel_coords.y >= config.resolution.y) {
    discard;
  }

  i32 r = max(1, config.outline_width);
  f32 max_mask = 0.0;

  u32x2 mask_coords = min(u32x2(input.tex_coord * f32x2(config.mask_resolution)), config.mask_resolution - 1u);

  // 1D Vertical Sweep (Completes the 2D separable dilation)
  for (i32 y = -r; y <= r; y++) {
    i32 sample_y = clamp(int(mask_coords.y) + y, 0, int(config.mask_resolution.y) - 1);
    f32 val = params.dilated_horiz_mask.Load(i32x3(mask_coords.x, sample_y, 0)).r;
    max_mask = max(max_mask, val);
  }

  // Isolate the outer edge boundary
  f32 center_mask = params.original_mask.Load(i32x3(mask_coords, 0)).r;
  f32 edge_intensity = max_mask - center_mask;

  f32x4 back_color = params.scene_color.Sample(params.linear_sampler, input.tex_coord);

  if (edge_intensity > 0.01) {
    f32 pixel_depth = params.scene_depth.Load(i32x3(pixel_coords, 0)).r;
    f32x4 final_outline = f32x4(com::srgb_to_linear(config.outline_color.rgb), config.outline_color.a);

    bool is_occluded = (pixel_depth > 0.00001f);

    f32x4 final_color;
    if (is_occluded && config.occluded_mode == 0) {
      final_color = back_color;
    } else if (is_occluded && config.occluded_mode == 1) {
      final_color = lerp(back_color, final_outline, 0.35);
    } else {
      final_color = final_outline;
    }

    return final_color;
  }

  return back_color;
}
