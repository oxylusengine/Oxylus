import common;
import gpu;

static constexpr f32 ALPHA_INTERPOLATION = 1.0;
[[vk::constant_id(0)]] const u32 is_final_composite = 0;
[[vk::binding(0)]] StorageImage2D<f32x4, ImageFormat::R11F_G11F_B10F> result_image;
[[vk::binding(1)]] Image2D<f32x4> source_image_upsample;
[[vk::binding(2)]] Image2D<f32x4> source_image_downsample;
[[vk::binding(3)]] SamplerState sampler;

struct PushConstants {
  u32x2 img_size;
  f32 intensity;
};

func sample_color(Image2D<f32x4> src, SamplerState s, f32x2 uv, f32x2 offset) -> f32x3 {
  return src.SampleLevel(s, uv + offset, 0).rgb;
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
  u32x2 img_coords : SV_DispatchThreadID,
  uniform PushConstants pc
) {
  if (any(img_coords >= pc.img_size)) {
    return;
  }

  let uv = (img_coords + 0.5) / f32x2(pc.img_size);
  let texel_size = 1.0 / f32x2(pc.img_size);

  let a = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2(-1.0,  1.0));
  let b = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 0.0,  1.0));
  let c = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 1.0,  1.0));
  let d = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2(-1.0,  0.0));
  let e = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 0.0,  0.0));
  let f = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 1.0,  0.0));
  let g = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2(-1.0, -1.0));
  let h = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 0.0, -1.0));
  let i = sample_color(source_image_upsample, sampler, uv, texel_size * f32x2( 1.0, -1.0));

  let color_sum = e * 0.25
                + (b + d + f + h) * 0.125
                + (a + c + g + i) * 0.0625;

  let source_color = source_image_downsample.SampleLevel(sampler, uv, 0).rgb;
  let result = is_final_composite == 0
    ? source_color + color_sum
    : lerp(source_color, source_color + pc.intensity * color_sum, ALPHA_INTERPOLATION);

  result_image.Store(img_coords, f32x4(result, 1.0));
}
