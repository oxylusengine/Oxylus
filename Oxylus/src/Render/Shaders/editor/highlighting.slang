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

func outline(depth_image : Texture2D<f32>, sampler : Sampler, uv : f32x2, threshold : f32, texel_size : f32x2) -> f32 {
  float centerDepth = depth_image.SampleLevel(sampler, uv, 0).r;
  if (centerDepth == 0.0) {
    return 0;
  }

  float leftDepth   = depth_image.SampleLevel(sampler, uv + float2(-texel_size.x, 0), 0).r;
  float rightDepth  = depth_image.SampleLevel(sampler, uv + float2(texel_size.x, 0), 0).r;
  float upDepth     = depth_image.SampleLevel(sampler, uv + float2(0, -texel_size.y), 0).r;
  float downDepth   = depth_image.SampleLevel(sampler, uv + float2(0, texel_size.y), 0).r;

  float depthDiffX = abs(leftDepth - rightDepth);
  float depthDiffY = abs(upDepth - downDepth);
  float maxDiff = max(depthDiffX, depthDiffY);

  float distanceFromCamera = 1.0 - centerDepth;
  float adaptiveThreshold = threshold * (1.0 + distanceFromCamera * 0.1);

  return step(adaptiveThreshold, maxDiff);
}

func sobel_outline(
    pixel_coordinates : uint2,
    resolution : uint2,
    depth_image : Texture2D<f32>,
    sampler_state : SamplerState
) -> f32 {
  let tex_coord = (f32x2(pixel_coordinates) + 0.5) / f32x2(resolution);
  let offset = 1.0 / f32x2(resolution);

  var depth_samples : f32[9];
  var valid_count = 0;

  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let index = (y + 1) * 3 + (x + 1);
        let sample_coord = tex_coord + f32x2(x, y) * offset;
        depth_samples[index] = depth_image.SampleLevel(sampler_state, sample_coord, 0).r;
        if (depth_samples[index] < 0.999) {
          valid_count++;
      }
    }
  }

  if (depth_samples[4] >= 0.999 || valid_count < 5) {
    return 0.0;
  }

  let sobel_x = -1.0 * depth_samples[0] + 1.0 * depth_samples[2] +
                -2.0 * depth_samples[3] + 2.0 * depth_samples[5] +
                -1.0 * depth_samples[6] + 1.0 * depth_samples[8];

  let sobel_y = -1.0 * depth_samples[0] - 2.0 * depth_samples[1] - 1.0 * depth_samples[2] +
                1.0 * depth_samples[6] + 2.0 * depth_samples[7] + 1.0 * depth_samples[8];

  let edge_magnitude = sqrt(sobel_x * sobel_x + sobel_y * sobel_y);

  let adaptive_threshold = 0.01 + (depth_samples[4] * 0.02);

  return smoothstep(adaptive_threshold * 0.5, adaptive_threshold, edge_magnitude);
}


[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
  const uint3 id : SV_DispatchThreadID,
  uniform ParameterBlock<ShaderParameters> params,
  uniform u32 buffer_size
) -> void {
  uint2 pixel_coords = id.xy;

  u32 width, height, levels;
  params.visbuffer.GetDimensions(0, width, height, levels);

  if (pixel_coords.x >= width || pixel_coords.y >= height)
      return;

  let texel = params.visbuffer.load(pixel_coords);
  if (texel == ~0u) {
    return;
  }

  let vis = VisBufferData(texel);
  let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
  let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];

  bool found = false;
  for (u32 i = 0; i < buffer_size; i++) {
    let transform_index = params.transform_indices[i];
    if (mesh_instance.transform_index == transform_index && transform_index != ~0u && transform_index > 0) {
      found = true;
    }
  }

  if (!found) {
    return;
  }

  u32x2 resolution = u32x2(width, height);
  float2 uv = (pixel_coords + 0.5) / resolution;

  float depth = params.depth_image.SampleLevel(params.point_clamp_sampler, uv, 0).r;
  if (depth == 0.0) {
    return;
  }

#define SOBEL

#ifdef SIMPLE
  let edge = outline(params.depth_image, params.point_clamp_sampler, uv, 0.0001f, 1.0f / resolution);
#endif
#ifdef SOBEL
  let edge = sobel_outline(pixel_coords, resolution, params.depth_image, params.point_clamp_sampler);
#endif

  params.result_image[pixel_coords] = edge;
}
