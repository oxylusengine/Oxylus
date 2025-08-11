module bloom_upsample;

import common;
import gpu;

import passes.bloom.bloom;

struct ShaderParameters {
  StorageImage2D<f32x4, ImageFormat::R11F_G11F_B10F> result_image;
  Image2D<f32x4> source_image_upsample;
  Image2D<f32x4> source_image_downsample;
  SamplerState sampler;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params) {
  u32x2 img_coords = u32x2(thread_id.xy);

  u32 width, height, levels;
  params.result_image.GetDimensions(0, width, height, levels);
  u32x2 img_size = u32x2(width, height);

  if (any(img_coords >= img_size))
    return;

  f32x2 uv = (img_coords + 0.5) / img_size;

  f32x3 result = upsample(params.source_image_upsample, params.sampler, uv) 
                + params.source_image_downsample.SampleLevel(params.sampler, uv, 0).rgb;
  params.result_image.Store(img_coords, f32x4(result, 1.0));
}