import common;

import scene;

#define RENDER_FLAGS_2D_FLIP_X 1u << 1u

struct PushConstants {
  Material* materials;
  Camera* camera_buffer;
  Transform* transforms;
};
[[vk::push_constant]] PushConstants C;

struct VOutput {
  f32x4 position : SV_Position;
  f32x3 normal : NORMAL;
  f32x4 uv_alpha : UV;
  u32 material_index : MAT_INDEX;
  u32 flags : FLAGS;
};

struct VertexInput {
  [[vk::location(0)]] u32 material_id16_ypos16 : MAT_INDEX;
  [[vk::location(1)]] u32 flags16_distance16 : FLAGS;
  [[vk::location(2)]] u32 transform_id : T_ID;
};

[[shader("vertex")]]
VOutput vs_main(VertexInput input, u32 vertex_id : SV_VertexID) {
  VOutput output = (VOutput)0;

  const u32 flags = com::unpack_u32_low(input.flags16_distance16);
  output.flags = flags;

  const u32 material_index = com::unpack_u32_low(input.material_id16_ypos16);
  Material material = C.materials[material_index];

  mat4 transform = C.transforms[input.transform_id].world;
  
  f32x4 uv_size_offset = f32x4(material.uv_size, material.uv_offset);

  const u32 vertex_index = vertex_id % 6;

  f32x3 positions[6] =
    {f32x3(-0.5, -0.5, 0), f32x3(0.5, -0.5, 0), f32x3(0.5, 0.5, 0), f32x3(0.5, 0.5, 0), f32x3(-0.5, 0.5, 0), f32x3(-0.5, -0.5, 0)};
  f32x2 uvs[6] = {f32x2(0.0, 1.0), f32x2(1.0, 1.0), f32x2(1.0, 0.0), f32x2(1.0, 0.0), f32x2(0.0, 0.0), f32x2(0.0, 1.0)};

  output.uv_alpha.xy = uvs[vertex_index];
  output.uv_alpha.xy = (output.uv_alpha.xy * uv_size_offset.xy) + (uv_size_offset.zw);

  const int flip = flags & RENDER_FLAGS_2D_FLIP_X;
  f32x4 world_position = f32x4(f32x2(positions[vertex_index].xy * f32x2((f32)((bool)flip ? -1 : 1), 1)), 0, 1);
  world_position = mul(transform, world_position);

  output.position = mul(C.camera_buffer[0].projection_view, f32x4(world_position.xyz, 1.0f));
  output.material_index = material_index;

  return output;
}

[[shader("fragment")]]
f32x4 ps_main(VOutput input) : SV_Target0 {
  const Material material = C.materials[input.material_index];

  f32x2 uv = input.uv_alpha.xy;
  f32x4 color = material.sample_albedo_color(uv);

  return color;
}
