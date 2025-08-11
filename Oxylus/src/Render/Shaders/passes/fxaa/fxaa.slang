module fxaa;

#include <fullscreen.slang>

import common;
import gpu;

struct ShaderParameters {
  Image2D<f32x4> source_image_upsample;
  SamplerState sampler;
};

#define EDGE_THRESHOLD_MIN 0.0312
#define EDGE_THRESHOLD_MAX 0.125
#define QUALITY(q) ((q) < 5 ? 1.0 : ((q) > 5 ? ((q) < 10 ? 2.0 : ((q) < 11 ? 4.0 : 8.0)) : 1.5))
#define ITERATIONS 12
#define SUBPIXEL_QUALITY 0.75

f32 rgb2luma(f32x3 rgb) { return sqrt(dot(rgb, f32x3(0.299, 0.587, 0.114))); }

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params, uniform f32x2 inverse_screen_size) -> f32x4 {
  f32x3 color_center = params.source_image_upsample.Sample(params.sampler, input.tex_coord).rgb;

  // Luma at the current fragment
  f32 luma_center = rgb2luma(color_center);

  // Luma at the four direct neighbours of the current fragment.
  f32 luma_down = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(0, -1)).rgb);
  f32 luma_up = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(0, 1)).rgb);
  f32 luma_left = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(-1, 0)).rgb);
  f32 luma_right = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(1, 0)).rgb);

  // Find the maximum and minimum luma around the current fragment.
  f32 luma_min = min(luma_center, min(min(luma_down, luma_up), min(luma_left, luma_right)));
  f32 luma_max = max(luma_center, max(max(luma_down, luma_up), max(luma_left, luma_right)));

  // Compute the delta.
  f32 luma_range = luma_max - luma_min;

  // If the luma variation is lower that a threshold (or if we are in a really dark area), we are not on an edge, don't perform any AA.
  if (luma_range < max(EDGE_THRESHOLD_MIN, luma_max * EDGE_THRESHOLD_MAX)) {
    return f32x4(color_center, 1.0);
  }

  // Query the 4 remaining corners lumas.
  f32 luma_down_left = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(-1, -1)).rgb);
  f32 luma_up_right = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(1, 1)).rgb);
  f32 luma_up_left = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(-1, 1)).rgb);
  f32 luma_down_right = rgb2luma(params.source_image_upsample.Sample(params.sampler, input.tex_coord, i32x2(1, -1)).rgb);

  // Combine the four edges lumas (using intermediary variables for future computations with the same values).
  f32 luma_down_up = luma_down + luma_up;
  f32 lumaLeftRight = luma_left + luma_right;

  // Same for corners
  f32 luma_left_corners = luma_down_left + luma_up_left;
  f32 luma_down_corners = luma_down_left + luma_down_right;
  f32 luma_right_corners = luma_down_right + luma_up_right;
  f32 luma_up_corners = luma_up_right + luma_up_left;

  // Compute an estimation of the gradient along the horizontal and vertical axis.
  f32 edge_horizontal =
      abs(-2.0 * luma_left + luma_left_corners) + abs(-2.0 * luma_center + luma_down_up) * 2.0 + abs(-2.0 * luma_right + luma_right_corners);
      f32 edgeVertical = abs(-2.0 * luma_up + luma_up_corners) + abs(-2.0 * luma_center + lumaLeftRight) * 2.0 + abs(-2.0 * luma_down + luma_down_corners);

  // Is the local edge horizontal or vertical ?
  bool is_horizontal = (edge_horizontal >= edgeVertical);

  // Choose the step size (one pixel) accordingly.
  f32 step_length = is_horizontal ? inverse_screen_size.y : inverse_screen_size.x;

  // Select the two neighboring texels lumas in the opposite direction to the local edge.
  f32 luma1 = is_horizontal ? luma_down : luma_left;
  f32 luma2 = is_horizontal ? luma_up : luma_right;
  // Compute gradients in this direction.
  f32 gradient1 = luma1 - luma_center;
  f32 gradient2 = luma2 - luma_center;

  // Which direction is the steepest ?
  bool is1_steepest = abs(gradient1) >= abs(gradient2);

  // Gradient in the corresponding direction, normalized.
  f32 gradient_scaled = 0.25 * max(abs(gradient1), abs(gradient2));

  // Average luma in the correct direction.
  f32 luma_local_average = 0.0;
  if (is1_steepest) {
    // Switch the direction
    step_length = -step_length;
    luma_local_average = 0.5 * (luma1 + luma_center);
  } else {
    luma_local_average = 0.5 * (luma2 + luma_center);
  }

  // Shift UV in the correct direction by half a pixel.
  f32x2 current_uv = input.tex_coord;
  if (is_horizontal) {
    current_uv.y += step_length * 0.5;
  } else {
    current_uv.x += step_length * 0.5;
  }

  // Compute offset (for each iteration step) in the right direction.
  f32x2 offset = is_horizontal ? f32x2(inverse_screen_size.x, 0.0) : f32x2(0.0, inverse_screen_size.y);
  // Compute UVs to explore on each side of the edge, orthogonally. The QUALITY allows us to step faster.
  f32x2 uv1 = current_uv - offset * QUALITY(0);
  f32x2 uv2 = current_uv + offset * QUALITY(0);

  // Read the lumas at both current extremities of the exploration segment, and compute the delta wrt to the local average luma.
  float luma_end1 = rgb2luma(params.source_image_upsample.Sample(params.sampler, uv1).rgb);
  float luma_end2 = rgb2luma(params.source_image_upsample.Sample(params.sampler, uv2).rgb);
  luma_end1 -= luma_local_average;
  luma_end2 -= luma_local_average;

  // If the luma deltas at the current extremities is larger than the local gradient, we have reached the side of the edge.
  bool reached1 = abs(luma_end1) >= gradient_scaled;
  bool reached2 = abs(luma_end2) >= gradient_scaled;
  bool reached_both = reached1 && reached2;

  // If the side is not reached, we continue to explore in this direction.
  if (!reached1) {
    uv1 -= offset * QUALITY(1);
  }
  if (!reached2) {
    uv2 += offset * QUALITY(1);
  }

  // If both sides have not been reached, continue to explore.
  if (!reached_both) {
    for (int i = 2; i < ITERATIONS; i++) {
      // If needed, read luma in 1st direction, compute delta.
      if (!reached1) {
        luma_end1 = rgb2luma(params.source_image_upsample.Sample(params.sampler, uv1).rgb);
        luma_end1 = luma_end1 - luma_local_average;
      }
      // If needed, read luma in opposite direction, compute delta.
      if (!reached2) {
        luma_end2 = rgb2luma(params.source_image_upsample.Sample(params.sampler, uv2).rgb);
        luma_end2 = luma_end2 - luma_local_average;
      }
      // If the luma deltas at the current extremities is larger than the local gradient, we have reached the side of the edge.
      reached1 = abs(luma_end1) >= gradient_scaled;
      reached2 = abs(luma_end2) >= gradient_scaled;
      reached_both = reached1 && reached2;

      // If the side is not reached, we continue to explore in this direction, with a variable quality.
      if (!reached1) {
        uv1 -= offset * QUALITY(i);
      }
      if (!reached2) {
        uv2 += offset * QUALITY(i);
      }

      // If both sides have been reached, stop the exploration.
      if (reached_both) {
        break;
      }
    }
  }

  // Compute the distances to each side edge of the edge (!).
  float distance1 = is_horizontal ? (input.tex_coord.x - uv1.x) : (input.tex_coord.y - uv1.y);
  float distance2 = is_horizontal ? (uv2.x - input.tex_coord.x) : (uv2.y - input.tex_coord.y);

  // In which direction is the side of the edge closer ?
  bool is_direction1 = distance1 < distance2;
  float distance_final = min(distance1, distance2);

  // Thickness of the edge.
  float edge_thickness = (distance1 + distance2);

  // Is the luma at center smaller than the local average ?
  bool is_luma_center_smaller = luma_center < luma_local_average;

  // If the luma at center is smaller than at its neighbour, the delta luma at each end should be positive (same variation).
  bool correct_variation1 = (luma_end1 < 0.0) != is_luma_center_smaller;
  bool correctVariation2 = (luma_end2 < 0.0) != is_luma_center_smaller;

  // Only keep the result in the direction of the closer side of the edge.
  bool correct_variation = is_direction1 ? correct_variation1 : correctVariation2;

  // UV offset: read in the direction of the closest side of the edge.
  float pixel_offset = -distance_final / edge_thickness + 0.5;

  // If the luma variation is incorrect, do not offset.
  float final_offset = correct_variation ? pixel_offset : 0.0;

  // Sub-pixel shifting
  // Full weighted average of the luma over the 3x3 neighborhood.
  float luma_average = (1.0 / 12.0) * (2.0 * (luma_down_up + lumaLeftRight) + luma_left_corners + luma_right_corners);
  // Ratio of the delta between the global average and the center luma, over the luma range in the 3x3 neighborhood.
  float sub_pixel_offset1 = clamp(abs(luma_average - luma_center) / luma_range, 0.0, 1.0);
  float sub_pixel_offset2 = (-2.0 * sub_pixel_offset1 + 3.0) * sub_pixel_offset1 * sub_pixel_offset1;
  // Compute a sub-pixel offset based on this delta.
  float sub_pixel_offset_final = sub_pixel_offset2 * sub_pixel_offset2 * SUBPIXEL_QUALITY;

  // Pick the biggest of the two offsets.
  final_offset = max(final_offset, sub_pixel_offset_final);

  // Compute the final UV coordinates.
  f32x2 final_uv = input.tex_coord;
  if (is_horizontal) {
    final_uv.y += final_offset * step_length;
  } else {
    final_uv.x += final_offset * step_length;
  }

  // Read the color at the new UV coordinates, and use it.
  f32x3 final_color = params.source_image_upsample.Sample(params.sampler, final_uv).rgb;
  return f32x4(final_color, 1.0);
}