import common;

import visbuffer;
import gpu;
import scene;

struct MaskParameters {
  StructuredBuffer<MeshletInstance> meshlet_instances;
  StructuredBuffer<MeshInstance> mesh_instances;
  Image2D<u32> visbuffer;
  RWTexture2D<f32> out_mask;
  StructuredBuffer<u32> transform_indices;
};

bool is_texel_selected(
  u32 texel, uniform ParameterBlock<MaskParameters> params, u32 buffer_size, u32 terrain_transform_index
) {
  if (texel == ~0u)
    return false;

  u32 texel_transform_index;
  if (is_terrain(texel)) {
    texel_transform_index = terrain_transform_index;
  } else {
    let vis = VisBufferData(texel);
    let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    texel_transform_index = mesh_instance.transform_index;
  }

  if (texel_transform_index == ~0u)
    return false;

  for (u32 i = 0; i < buffer_size; i++) {
    let transform_index = params.transform_indices[i];
    if (texel_transform_index == transform_index && transform_index != ~0u) {
      return true;
    }
  }
  return false;
}

[[shader("compute")]] [[numthreads(16, 16, 1)]]
func cs_main(
  const uint3 id : SV_DispatchThreadID,
  uniform ParameterBlock<MaskParameters> params,
  uniform u32 buffer_size,
  uniform u32 terrain_transform_index
) -> void {
  u32x2 pixel_coords = id.xy;

  u32 width, height, levels;
  params.visbuffer.GetDimensions(0, width, height, levels);

  if (pixel_coords.x >= width || pixel_coords.y >= height)
    return;

  let center_texel = params.visbuffer.Load(u32x3(pixel_coords, 0));
  let center_selected = is_texel_selected(center_texel, params, buffer_size, terrain_transform_index);

  // 1.0 = selected, 0.0 = empty
  params.out_mask[pixel_coords] = center_selected ? 1.0 : 0.0;
}
