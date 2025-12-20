import common;

import scene;

struct PushConstants {
  Camera* camera_buffer;
  Transform* transforms;
};
[[vk::push_constant]] PushConstants C;

struct VOutput {
  f32x4 position : SV_Position;
  u32 transform_id: T_ID;
};

struct VertexInput {
  [[vk::location(0)]] u32 material_id16_ypos16 : MAT_INDEX;
  [[vk::location(1)]] u32 flags16_distance16 : FLAGS;
  [[vk::location(2)]] u32 transform_id : T_ID;
};

[[shader("vertex")]]
VOutput vs_main(VertexInput input, u32 vertex_id : SV_VertexID) {
  VOutput output = (VOutput)0;

  mat4 transform = C.transforms[input.transform_id].world;

  const u32 vertex_index = vertex_id % 6;

  f32x3 positions[6] =
    {f32x3(-0.5, -0.5, 0), f32x3(0.5, -0.5, 0), f32x3(0.5, 0.5, 0), f32x3(0.5, 0.5, 0), f32x3(-0.5, 0.5, 0), f32x3(-0.5, -0.5, 0)};

  f32x4 world_position = f32x4(f32x2(positions[vertex_index].xy), 0, 1);
  world_position = mul(transform, world_position);

  output.position = mul(C.camera_buffer.projection_view, f32x4(world_position.xyz, 1.0f));
  output.transform_id = input.transform_id;

  return output;
}

[[shader("fragment")]]
u32 fs_main(VOutput input) : SV_Target0 {
  u32 ti = input.transform_id;
  return ti;
}
