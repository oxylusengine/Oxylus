import common;
import gpu;

struct ShaderParameters {
  StorageImage2D<f32x4, ImageFormat::R11F_G11F_B10F> result_image;
  Image2D<f32x4> source_image;
  SamplerState sampler;
};

func sample_color(Image2D<f32x4> src, SamplerState s, f32x2 uv, f32x2 offset) -> f32x3 {
  return src.SampleLevel(s, uv + offset, 0).rgb;
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
  u32x2 img_coords : SV_DispatchThreadID,
  uniform ParameterBlock<ShaderParameters> params,
  uniform u32x2 img_size
) {
  if (any(img_coords >= img_size))
    return;

  let uv = (img_coords + 0.5) / f32x2(img_size);
  let texel_size = 1.0 / f32x2(img_size);

  let a = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2(-2.0,  2.0));
  let b = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 0.0,  2.0));
  let c = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 2.0,  2.0));
  let d = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2(-2.0,  0.0));
  let e = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 0.0,  0.0));
  let f = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 2.0,  0.0));
  let g = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2(-2.0, -2.0));
  let h = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 0.0, -2.0));
  let i = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 2.0, -2.0));
  let j = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2(-1.0,  1.0));
  let k = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 1.0,  1.0));
  let l = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2(-1.0, -1.0));
  let m = sample_color(params.source_image, params.sampler, uv, texel_size * f32x2( 1.0, -1.0));

  let result = (a + c + g + i) * 0.03125
             + (b + d + f + h) * 0.0625
             + (e + j + k + l + m) * 0.125;

  params.result_image.Store(img_coords, f32x4(result, 1.0));
}
