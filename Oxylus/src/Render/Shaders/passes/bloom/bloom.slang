module bloom;

import common;
import gpu;

public f32x3 downsample(Image2D<f32x4> src, SamplerState sampler, f32x2 uv, float lod) {
  f32x3 center = src.SampleLevel(sampler, uv, lod).rgb;
  f32x3 yellow_up_right = src.SampleLevel(sampler, uv, lod, i32x2( 0,  2)).rgb;
  f32x3 yellow_down_left = src.SampleLevel(sampler, uv, lod, i32x2(-2,  0)).rgb;
  f32x3 green_down_right = src.SampleLevel(sampler, uv, lod, i32x2( 2,  0)).rgb;
  f32x3 blue_down_left = src.SampleLevel(sampler, uv, lod, i32x2( 0, -2)).rgb;

  f32x3 yellow = src.SampleLevel(sampler, uv, lod, i32x2(-2,  2)).rgb;
  yellow += yellow_up_right;
  yellow += center;
  yellow += yellow_down_left;

  f32x3 green = yellow_up_right;
  green += src.SampleLevel(sampler, uv, lod, i32x2( 2,  2)).rgb;
  green += green_down_right;
  green += center;

  f32x3 blue = center;
  blue += green_down_right;
  blue += src.SampleLevel(sampler, uv, lod, i32x2( 2, -2)).rgb;
  blue += blue_down_left;

  f32x3 lila = yellow_down_left;
  lila += center;
  lila += blue_down_left;
  lila += src.SampleLevel(sampler, uv, lod, i32x2(-2, -2)).rgb;

  f32x3 red = src.SampleLevel(sampler, uv, lod, i32x2(-1,  1)).rgb;
  red += src.SampleLevel(sampler, uv, lod, i32x2( 1,  1)).rgb;
  red += src.SampleLevel(sampler, uv, lod, i32x2( 1, -1)).rgb;
  red += src.SampleLevel(sampler, uv, lod, i32x2(-1, -1)).rgb;

  return (red * 0.5 + (yellow + green + blue + lila) * 0.125) * 0.25;
}

public f32x3 upsample(Image2D<f32x4> src, SamplerState sampler, f32x2 uv) {
  u32 width, height, levels;
  src.GetDimensions(0, width, height, levels);
  f32x2 texel_size = 1.0 / f32x2(width, height);

  f32x4 red_samples = src.GatherRed(sampler, uv);
  f32x4 green_samples = src.GatherGreen(sampler, uv);
  f32x4 blue_samples = src.GatherBlue(sampler, uv);

  f32x3 top_left = src.SampleLevel(sampler, uv + f32x2(-texel_size.x, texel_size.y), 0).rgb;
  f32x3 top_center = src.SampleLevel(sampler, uv + f32x2(0, texel_size.y), 0).rgb;
  f32x3 left_center = src.SampleLevel(sampler, uv + f32x2(-texel_size.x, 0), 0).rgb;
  f32x3 bottom_left = src.SampleLevel(sampler, uv + f32x2(-texel_size.x, -texel_size.y), 0).rgb;
  f32x3 bottom_center = src.SampleLevel(sampler, uv + f32x2(0, -texel_size.y), 0).rgb;

  f32x3 top_right = f32x3(red_samples.x, green_samples.x, blue_samples.x);
  f32x3 center = f32x3(red_samples.z, green_samples.z, blue_samples.z);
  f32x3 right_center = f32x3(red_samples.w, green_samples.w, blue_samples.w);
  f32x3 bottom_right = f32x3(red_samples.y, green_samples.y, blue_samples.y);

  f32x3 result = top_left * 1.0 + top_center * 2.0 + top_right * 1.0;
  result += left_center * 2.0 + center * 4.0 + right_center * 2.0;
  result += bottom_left * 1.0 + bottom_center * 2.0 + bottom_right * 1.0;

  return result / 16.0;
}

public f32x3 upsample_simple(Image2D<f32x4> src, SamplerState sampler, f32x2 uv, f32x2 texel_size) {
  f32x2 offsets[4] = {
    f32x2(-texel_size.x, -texel_size.y),
    f32x2( texel_size.x, -texel_size.y),
    f32x2(-texel_size.x,  texel_size.y),
    f32x2( texel_size.x,  texel_size.y)
  };

  f32x3 result = 0.0;
  for (u32 i = 0; i < 4; i++) {
    result += src.SampleLevel(sampler, uv + offsets[i], 0).rgb;
  }

  return result * 0.25;
}

public f32x3 prefilter(f32x3 color, f32 clamp_value, f32 threshold, f32 epsilon) {
  const f32 Knee = 0.2;
  color = min(f32x3(clamp_value), color);

  f32 brightness = max(max(color.r, color.g), color.b);

  f32x3 curve = f32x3(threshold - Knee, Knee * 2.0, 0.25 / Knee);
  f32 rq = clamp(brightness - curve.x, 0.0, curve.y);
  rq = (rq * rq) * curve.z;
  color *= max(rq, brightness - threshold) / max(brightness, epsilon);

  return color;
}