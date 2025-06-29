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
  f32x3 result = src.SampleLevel(sampler, uv, 0, i32x2(-1, 1)).rgb * 1.0;
  result += src.SampleLevel(sampler, uv, 0, i32x2( 0, 1)).rgb * 2.0;
  result += src.SampleLevel(sampler, uv, 0, i32x2( 1, 1)).rgb * 1.0;

  result += src.SampleLevel(sampler, uv, 0, i32x2(-1, 0)).rgb * 2.0;
  result += src.SampleLevel(sampler, uv, 0).rgb * 4.0;
  result += src.SampleLevel(sampler, uv, 0, i32x2( 1, 0)).rgb * 2.0;

  result += src.SampleLevel(sampler, uv, 0, i32x2(-1, -1)).rgb * 1.0;
  result += src.SampleLevel(sampler, uv, 0, i32x2( 0, -1)).rgb * 2.0;
  result += src.SampleLevel(sampler, uv, 0, i32x2( 1, -1)).rgb * 1.0;

  return result / 16.0;
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