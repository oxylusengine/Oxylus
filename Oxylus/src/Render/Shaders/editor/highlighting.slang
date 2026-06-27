import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
  StructuredBuffer<MeshletInstance> meshlet_instances;
  StructuredBuffer<MeshInstance> mesh_instances;
  Image2D<u32> visbuffer;
  Image2D<f32> depth_image;
  RWTexture2D<f32> result_image;
  StructuredBuffer<u32> transform_indices;
  Sampler point_clamp_sampler;
};

bool is_texel_selected(u32 texel, uniform ParameterBlock<ShaderParameters> params, u32 buffer_size) {
  if (texel == ~0u)
    return false;

  let vis = VisBufferData(texel);
  let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
  let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];

  for (u32 i = 0; i < buffer_size; i++) {
    let transform_index = params.transform_indices[i];
    if (mesh_instance.transform_index == transform_index && transform_index != ~0u) {
      return true;
    }
  }
  return false;
}

[[shader("compute")]] [[numthreads(16, 16, 1)]]
func cs_main(
  const uint3 id : SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params, uniform u32 buffer_size
) -> void {
  uint2 pixel_coords = id.xy;

  u32 width, height, levels;
  params.visbuffer.GetDimensions(0, width, height, levels);

  if (pixel_coords.x >= width || pixel_coords.y >= height)
    return;

  let center_texel = params.visbuffer.Load(u32x3(pixel_coords, 0));
  bool center_selected = is_texel_selected(center_texel, params, buffer_size);

  const i32 RADIUS = 3;
  bool neighbor_selected = false;

  for (i32 y = -RADIUS; y <= RADIUS; y++) {
    for (i32 x = -RADIUS; x <= RADIUS; x++) {
      if (x == 0 && y == 0)
        continue;

      uint2 sample_coords = uint2(
        clamp(int(pixel_coords.x) + x, 0, int(width) - 1),
        clamp(int(pixel_coords.y) + y, 0, int(height) - 1)
      );

      let neighbor_texel = params.visbuffer.Load(u32x3(sample_coords, 0));
      if (is_texel_selected(neighbor_texel, params, buffer_size)) {
        neighbor_selected = true;
        break;
      }
    }
    if (neighbor_selected)
      break;
  }

  f32 edge = 0.0;

  if (neighbor_selected && !center_selected) {
    edge = 1.0;
  }

  params.result_image[pixel_coords] = edge;
}
