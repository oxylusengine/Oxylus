import common;

import scene;
import gpu;

struct ShaderParameters {
  ConstantBuffer<Camera> camera;
  Image2D<f32> depth_image;
};

struct VertexInput {
  f32x3 position;
  u32 color;
};

struct VertexOutput {
  f32x4 position : SV_Position;
  f32x4 color : COLOR;
};

f32x3 oct_to_vec3(f32x2 e) {
  f32x3 v = f32x3(e.xy, 1.0 - abs(e.x) - abs(e.y));
  const f32x2 sign_not_zero = f32x2((v.x >= 0.0) ? +1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
  if (v.z < 0.0)
    v.xy = (1.0 - abs(v.yx)) * sign_not_zero;
  return normalize(v);
}

f32x2 unpack_snorm2_x16(const u32 packed) {
  i32x2 signextended;
  signextended.x = (int)(packed << 16) >> 16;
  signextended.y = (int)(packed & 0xFFFF0000) >> 16;
  return max(f32x2(signextended) / 32767.0f, -1.0f);
}

[[shader("vertex")]]
func vs_main(VertexInput input, uniform ParameterBlock<ShaderParameters> params) -> VertexOutput {
  VertexOutput output;

  output.position = mul(params.camera.projection_view, f32x4(input.position, 1.0));
  output.color = f32x4(oct_to_vec3(unpack_snorm2_x16(input.color)), 1.0f);

  return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
  const f32 scene_depth = params.depth_image.load(u32x2(input.position.xy));
  const f32 fragment_depth = input.position.z;

  f32x4 final_color = input.color;

  const f32 near_geometry_threshold = 0.99; // Adjust based on your depth range

  bool is_occluded = !(scene_depth > 0.0 && scene_depth < fragment_depth && scene_depth < near_geometry_threshold);
  if (is_occluded && scene_depth != 0.0) {
    final_color.rgb *= 0.4;
  }

  return final_color;
}
