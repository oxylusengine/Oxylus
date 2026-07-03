import common;
import gpu;
import cull;

[[vk::binding(0)]] Texture2D<f32> scene_color;
[[vk::binding(1)]] RWTexture2D<f32> out_color;

[[shader("compute")]] [[numthreads(16, 16, 1)]]
func cs_main(const u32x2 id : SV_DispatchThreadID, uniform u32x2 resolution) -> void {
  u32x2 pixel_coords = id.xy;

  u32x2 target_resolution = resolution >> 1;
  if (pixel_coords.x >= target_resolution.x || pixel_coords.y >= target_resolution.y) {
    return;
  }

  i32x2 base_coord = int2(pixel_coords * 2);
  i32x2 max_coords = int2(resolution) - 1;

  f32 final_depth = min_reduction(scene_color, base_coord, max_coords, 0);

  out_color.Store(pixel_coords, final_depth);
}
