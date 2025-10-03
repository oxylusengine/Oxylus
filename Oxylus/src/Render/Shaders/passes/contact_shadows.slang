import common;
import gpu;
import scene;

import raymarch;

struct ShaderParameters {
  Texture2D<f32> src_depth;
  RWTexture2D<f32> dst_image;
  ConstantBuffer<Camera> camera;
  Sampler sampler_nearest_clamp;
  Sampler sampler_linear_clamp;
};

struct PushConstants {
  f32x3 sun_dir;
  u32 steps;
  f32 thickness;
  f32 shadow_length;
};

f32 linearize_depth(f32 depth, f32 near_plane, f32 far_plane) {
    // Convert from [0,1] depth buffer value to linear world distance
    f32 z_ndc = depth * 2.0f - 1.0f;  // Convert [0,1] to [-1,1] NDC
    f32 linear_depth = (2.0f * near_plane * far_plane) / (far_plane + near_plane - z_ndc * (far_plane - near_plane));
    return linear_depth;
}

[[numthreads(8, 8, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform PushConstants pc
) -> void {
  u32x2 img_coords = thread_id.xy;

  u32 width, height, levels;
  params.src_depth.GetDimensions(0, width, height, levels);

  u32x2 image_size = u32x2(width, height);

  if (any(img_coords >= image_size))
    return;

  f32x2 uv = (img_coords + 0.5f) / image_size;

  f32 shadow = 0;

  f32 raymarch_jitter = 1.0;

  f32 depth_sample = params.src_depth.SampleLevel(params.sampler_nearest_clamp, uv, 0);

  const f32x3 cs = f32x3(uv * 2.0 - 1.0, depth_sample);
  const f32x4 world_position_h = mul(params.camera.inv_projection_view, f32x4(cs, 1.0));
  const f32x3 ws = world_position_h.xyz / world_position_h.w;

  f32 rcp_near_plane_distance = 1.0f / params.camera.near_clip;

  f32 shadow_distance = pc.shadow_length;
  f32x3 shadow_direction = normalize(pc.sun_dir);
  f32x3 shadow_end = ws + shadow_direction * shadow_distance;

  DepthRayMarchResult raymarch_result = DepthRayMarch
    ::new_from_depth(image_size)
    .from_cs(cs)
    .to_ws(shadow_end, params.camera.view, params.camera.projection)
    .with_linear_steps(pc.steps)
    .with_depth_thickness(pc.thickness)
    .with_jitter(raymarch_jitter)
    .with_march_behind_surfaces(false)
    .march(params.src_depth, params.sampler_linear_clamp, params.sampler_nearest_clamp, rcp_near_plane_distance);

  if (raymarch_result.hit) {
      shadow = smoothstep(1.0, 0.3, raymarch_result.hit_penetration_frac);
  }

  params.dst_image.store(img_coords, shadow);
}