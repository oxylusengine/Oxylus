import common;
import gpu;

struct ShaderParameters {
  StorageImage2D<f32x4, ImageFormat::R11F_G11F_B10F> result_image;
  Image2D<f32x4> source_image;
  SamplerState sampler;
};

struct PushConstants {
  f32 threshold;
  f32 soft_threshold;
  u32x3 img_size;
};

func sample_color(Image2D<f32x4> src, SamplerState s, f32x2 uv, f32x2 offset) -> f32x3 {
  return src.SampleLevel(s, uv + offset, 0).rgb;
}

func karis_average(f32x3 color) -> f32 {
  return 1.0 / (1.0 + com::luminance(color));
}

func prefilter(f32x3 color, f32 threshold, f32 soft_threshold) -> f32x3 {
  let brightness = max(color.r, max(color.g, color.b));
  let knee = threshold * soft_threshold;
  var soft = brightness - threshold + knee;
  soft = clamp(soft, 0.0, 2.0 * knee);
  soft = soft * soft * 0.25 / (knee + 1.0e-5);

  var contribution = max(soft, brightness - threshold);
  contribution /= max(brightness, 1.0e-5);
  return color * contribution;
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
  u32x3 thread_id : SV_DispatchThreadID,
  uniform ParameterBlock<ShaderParameters> params,
  uniform PushConstants pc
) {
  let texel = thread_id.xy;
  if (any(texel >= pc.img_size.xy))
    return;

  let uv = (texel + 0.5) / f32x2(pc.img_size.xy);
  let texel_size = 1.0 / f32x2(pc.img_size.xy);

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

  let groups = Array<f32x3, 5>(
    (a + b + d + e) * 0.25,
    (b + c + e + f) * 0.25,
    (d + e + g + h) * 0.25,
    (e + f + h + i) * 0.25,
    (j + k + l + m) * 0.25
  );

  var color_sum = f32x3(0.0);
  var weight_sum = 0.0;
  for (u32 gid = 0; gid < 5; gid++) {
    let weight = karis_average(groups[gid]);
    color_sum += groups[gid] * weight;
    weight_sum += weight;
  }

  let result = prefilter(color_sum / (weight_sum + 1.0e-5), pc.threshold, pc.soft_threshold);
  params.result_image.Store(texel, f32x4(result, 1.0));
}
