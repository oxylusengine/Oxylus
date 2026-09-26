import common;
import scene;
import gpu;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Image2D<f32> depth_image;

// mirrored in Debug.cpp
static const u32 DEBUG_FLAG_DEPTH_TESTED = 1u << 0u;
static const u32 DEBUG_FLAG_SHADED = 1u << 1u;

struct PushConstants {
  u32 flags;
};
[[vk::push_constant]] PushConstants C;

struct VertexInput {
  f32x3 position;
  u32 color;
};

struct VertexOutput {
  f32x4 position : SV_Position;
  f32x4 color;
  f32x3 world_position;
};

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput {
  VertexOutput output;

  output.position = mul(camera.projection_view, f32x4(input.position, 1.0));
  // rgba8, r in the lowest byte
  output.color = f32x4((u32x4(input.color) >> u32x4(0, 8, 16, 24)) & 0xFFu) / 255.0;
  output.world_position = input.position;

  return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
  f32x4 color = input.color;

  if ((C.flags & DEBUG_FLAG_SHADED) != 0) {
    // flat headlight shading, otherwise solid shapes read as a single blob of color
    const f32x3 normal = normalize(cross(ddx(input.world_position), ddy(input.world_position)));
    const f32x3 view_dir = normalize(camera.position.xyz - input.world_position);
    color.rgb *= 0.35 + 0.65 * abs(dot(normal, view_dir));
  }

  // reversed z, the relative bias keeps shapes lying on scene surfaces from flickering
  const f32 scene_depth = depth_image.Load(u32x3(u32x2(input.position.xy), 0));
  const bool occluded = scene_depth > input.position.z * 1.001;
  if (occluded) {
    if ((C.flags & DEBUG_FLAG_DEPTH_TESTED) != 0)
      discard;

    color.rgb *= 0.4;
  }

  return color;
}
