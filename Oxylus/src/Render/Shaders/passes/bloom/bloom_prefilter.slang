module bloom_prefilter;

import common;
import gpu;
import passes.bloom.bloom;

struct ShaderParameters {
  StorageImage2D<f32x4, ImageFormat::R11F_G11F_B10F> result_image;
  Image2D<f32x4> source_image;
  SamplerState sampler;
};

struct PushConstants {
  f32 threshold;
  f32 clamp;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, 
            uniform ParameterBlock<ShaderParameters> params, 
            uniform PushConstants pc) {
  u32x2 img_coords = thread_id.xy;

  u32 width, height, levels;
  params.result_image.GetDimensions(0, width, height, levels);
  u32x2 img_size = u32x2(width, height);

  if (any(img_coords >= img_size))
    return;

  f32x2 uv = (img_coords + 0.5f) / img_size;

  const f32 epsilon = 0.0001f;

  f32x3 result = downsample(params.source_image, params.sampler, uv, 0);
  result = prefilter(result, pc.clamp, pc.threshold, epsilon);
  params.result_image.store(img_coords, f32x4(result, 1.0f));
}